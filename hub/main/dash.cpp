/* dash.cpp - see dash.h.
 *
 * A `dash` runs a small state machine on the CHIP task: read device[idx], and on
 * its read-done OR a per-device timeout, advance to the next paired device; after
 * the last one, print the assembled table. Sequential (one read in flight) keeps
 * the packet-buffer pool from being exhausted the way a concurrent fan-out does.
 */
#include "dash.h"
#include "device_model.h"
#include "matter_names.h"
#include "tlv_decode.h"

#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"

#include <esp_matter.h>
#include <esp_matter_controller_read_command.h>
#include <platform/CHIPDeviceLayer.h>
#include <system/SystemClock.h>

using chip::app::AttributePathParams;
using chip::app::ConcreteDataAttributePath;
using chip::app::EventPathParams;
using chip::Platform::ScopedMemoryBufferWithSize;
using chip::TLV::TLVReader;

static const char *TAG = "dash";

/* Value attributes read from every device (wildcard endpoint). A device only
 * returns the ones it actually has, so the same set works for sensors and plugs. */
#define CL_TEMP     0x0402UL
#define CL_HUMID    0x0405UL
#define CL_CO2      0x040DUL
#define CL_PM25     0x042AUL
#define CL_ONOFF_   0x0006UL
#define CL_POWERSRC 0x002FUL
#define A_MEASURED  0x0000UL
#define A_BATPCT    0x000CUL      /* PowerSource.BatPercentRemaining (half-percent) */
#define CL_SWITCH_  0x003BUL      /* Generic Switch: button events                  */

static const struct { uint32_t cl, at; } DASH_ATTRS[] = {
    { CL_TEMP,     A_MEASURED },
    { CL_HUMID,    A_MEASURED },
    { CL_CO2,      A_MEASURED },
    { CL_PM25,     A_MEASURED },
    { CL_ONOFF_,   A_MEASURED },
    { CL_POWERSRC, A_BATPCT   },
};
#define DASH_ATTR_N ((int)(sizeof(DASH_ATTRS) / sizeof(DASH_ATTRS[0])))

#define DASH_DEV_TIMEOUT_MS 2500

struct dval { bool has; double v; };
struct ddev {
    bool  active;      /* a paired device we are sweeping    */
    bool  reached;     /* got at least one attribute back     */
    dval  temp, humid, co2, pm25, onoff, batt;
};

static struct dash_ctx {
    volatile bool busy;
    int  idx;
    bool settled;      /* current device already advanced (dedupe done vs timeout) */
    ddev dev[MAX_DEVICES];
} D;

/* Last button (Switch) event per slot - filled from the event-report callback. */
struct devent { bool has; uint32_t cluster, event; int64_t us; uint16_t ep; };
static devent g_lastev[MAX_DEVICES];

/* Last-known battery per slot (raw BatPercentRemaining, half-percent) with the
 * time it was seen. Fed passively from the subscription AND from live dash reads,
 * so an asleep device still shows its last battery (with an age) when a live read
 * misses it. */
struct dbatt { bool valid; double raw; int64_t us; };
static dbatt g_batt[MAX_DEVICES];

/* Last-known OnOff per slot, cached passively from the subscription so a plug's
 * on/off state is available in `status` without a live read (v1.9). */
struct donoff { bool valid; bool on; };
static donoff g_onoff[MAX_DEVICES];

int  dash_sub_path_count(void) { return 2; }
void dash_sub_path(int i, uint32_t *cluster, uint32_t *attr)
{
    if      (i == 0) { if (cluster) *cluster = CL_POWERSRC; if (attr) *attr = A_BATPCT;  }
    else if (i == 1) { if (cluster) *cluster = CL_ONOFF_;   if (attr) *attr = A_MEASURED; }  /* OnOff attr 0x0000 */
    else             { if (cluster) *cluster = 0;           if (attr) *attr = 0;         }
}

void dash_on_report(uint64_t node_id, uint16_t, uint32_t cluster_id, uint32_t attr_id, double value)
{
    int slot = dev_slot_by_node(node_id);
    if (slot < 0) return;
    if (cluster_id == CL_POWERSRC && attr_id == A_BATPCT) {
        g_batt[slot].valid = true;
        g_batt[slot].raw = value;
        g_batt[slot].us = esp_timer_get_time();
    } else if (cluster_id == CL_ONOFF_ && attr_id == A_MEASURED) {
        g_onoff[slot].valid = true;
        g_onoff[slot].on = (value != 0);
    }
}

bool dash_get_battery(int slot, int *pct, int *age_s)
{
    if (slot < 0 || slot >= MAX_DEVICES || !g_batt[slot].valid) return false;
    if (pct)   *pct   = (int)(g_batt[slot].raw / 2.0 + 0.5);   /* raw is half-percent */
    if (age_s) *age_s = (int)((esp_timer_get_time() - g_batt[slot].us) / 1000000);
    return true;
}

bool dash_get_onoff(int slot, bool *on)
{
    if (slot < 0 || slot >= MAX_DEVICES || !g_onoff[slot].valid) return false;
    if (on) *on = g_onoff[slot].on;
    return true;
}

void dash_on_event(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id, uint32_t event_id)
{
    if (cluster_id != CL_SWITCH_) return;
    int slot = dev_slot_by_node(node_id);
    if (slot < 0) return;
    g_lastev[slot].has = true;
    g_lastev[slot].cluster = cluster_id;
    g_lastev[slot].event = event_id;
    g_lastev[slot].us = esp_timer_get_time();
    g_lastev[slot].ep = endpoint_id;
}

/* ---- read callbacks + sweep state machine (all on the CHIP task) ---------- */
static void dash_attr_cb(uint64_t node, const ConcreteDataAttributePath &path, TLVReader *data)
{
    int slot = dev_slot_by_node(node);
    if (slot < 0 || !data) return;
    double v;
    { TLVReader r; r.Init(*data); if (!tlv_get_scalar(r, v)) return; }
    ddev &d = D.dev[slot];
    d.reached = true;
    uint32_t cl = path.mClusterId, at = path.mAttributeId;
    if      (cl == CL_TEMP  && at == A_MEASURED) d.temp  = { true, v };
    else if (cl == CL_HUMID && at == A_MEASURED) d.humid = { true, v };
    else if (cl == CL_CO2   && at == A_MEASURED) d.co2   = { true, v };
    else if (cl == CL_PM25  && at == A_MEASURED) d.pm25  = { true, v };
    else if (cl == CL_ONOFF_&& at == A_MEASURED) d.onoff = { true, v };
    else if (cl == CL_POWERSRC && at == A_BATPCT) {
        d.batt = { true, v };
        g_batt[slot].valid = true; g_batt[slot].raw = v; g_batt[slot].us = esp_timer_get_time();
    }
}

static void dash_print(void);
static void dash_fire_current(void);
static void dash_timeout(chip::System::Layer *, void *);

static void dash_advance(void) { D.idx++; dash_fire_current(); }

static void dash_settle_and_advance(bool cancel_timer)
{
    if (D.settled) return;
    D.settled = true;
    if (cancel_timer) chip::DeviceLayer::SystemLayer().CancelTimer(dash_timeout, nullptr);
    dash_advance();
}

static void dash_done_cb(uint64_t, const ScopedMemoryBufferWithSize<AttributePathParams> &,
                         const ScopedMemoryBufferWithSize<EventPathParams> &)
{
    dash_settle_and_advance(true);
}
static void dash_timeout(chip::System::Layer *, void *) { dash_settle_and_advance(false); }

static void dash_fire_current(void)
{
    while (D.idx < MAX_DEVICES && !D.dev[D.idx].active) ++D.idx;
    if (D.idx >= MAX_DEVICES) { dash_print(); D.busy = false; return; }

    int slot = D.idx;
    D.settled = false;

    ScopedMemoryBufferWithSize<AttributePathParams> ap;
    ScopedMemoryBufferWithSize<EventPathParams>     ev;
    if (!ap.Alloc(DASH_ATTR_N)) { ESP_LOGE(TAG, "OOM: dash paths"); dash_settle_and_advance(false); return; }
    for (int i = 0; i < DASH_ATTR_N; ++i) ap[i] = AttributePathParams(DASH_ATTRS[i].cl, DASH_ATTRS[i].at);

    auto *cmd = chip::Platform::New<esp_matter::controller::read_command>(
        g_dev[slot].node_id, std::move(ap), std::move(ev), dash_attr_cb, dash_done_cb, nullptr);
    if (!cmd) { ESP_LOGE(TAG, "OOM: dash read_command"); dash_settle_and_advance(false); return; }
    if (cmd->send_command() != ESP_OK) {
        chip::Platform::Delete(cmd);
        dash_settle_and_advance(false);
        return;
    }
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(DASH_DEV_TIMEOUT_MS), dash_timeout, nullptr);
}

static void dash_start_work(intptr_t) { D.idx = 0; dash_fire_current(); }

/* ---- rendering ----------------------------------------------------------- */
static void dash_print(void)
{
    int64_t now = esp_timer_get_time();
    printf("[dash] device overview (live read):\r\n");
    for (int i = 0; i < MAX_DEVICES; ++i) {
        DeviceInfo &g = g_dev[i];
        if (!g.paired) continue;
        ddev &d = D.dev[i];

        char nm[72];
        printf("  #%d %s  [%s", i + 1, dev_display_name(g, nm, sizeof(nm)),
               d.reached ? "online" : (g.has_data ? "stale" : "offline"));
        if (g.has_data) printf(", %llds ago", (long long)((now - g.last_report_us) / 1000000));
        if (g.link_valid) printf(", RSSI %d LQI %u", (int)g.rssi, (unsigned)g.lqi);
        if (!g.enumerated) printf(", not-enum");
        printf("]");
        if (g.payload[0]) printf("  payload=%s", g.payload);
        printf("\r\n");

        char vb[176]; int off = 0; vb[0] = '\0';
        if (d.temp.has)  off += snprintf(vb + off, sizeof(vb) - off, " temp=%.1fC",      d.temp.v * 0.01);
        if (d.humid.has) off += snprintf(vb + off, sizeof(vb) - off, " humidity=%.0f%%", d.humid.v * 0.01);
        if (d.co2.has)   off += snprintf(vb + off, sizeof(vb) - off, " co2=%.0fppm",     d.co2.v);
        if (d.pm25.has)  off += snprintf(vb + off, sizeof(vb) - off, " pm25=%.0fug/m3",  d.pm25.v);
        if (d.onoff.has) off += snprintf(vb + off, sizeof(vb) - off, " power=%s",        d.onoff.v != 0 ? "ON" : "OFF");
        if (d.batt.has) {
            off += snprintf(vb + off, sizeof(vb) - off, " battery=%.0f%%", d.batt.v / 2.0);
        } else if (g_batt[i].valid) {   /* live read missed it: last-known from subscription */
            off += snprintf(vb + off, sizeof(vb) - off, " battery=%.0f%% (cached %llds ago)",
                            g_batt[i].raw / 2.0, (long long)((now - g_batt[i].us) / 1000000));
        }
        if (off == 0) snprintf(vb, sizeof(vb), " (no live values%s)", d.reached ? "" : " - unreachable");
        printf("   %s\r\n", vb);

        devent &e = g_lastev[i];
        if (e.has) {
            char eb[48];
            printf("   last-press: ep%u %s  %llds ago\r\n", e.ep,
                   event_label(e.cluster, e.event, eb, sizeof(eb)),
                   (long long)((now - e.us) / 1000000));
        }
    }
}

/* ---- console dispatch ---------------------------------------------------- */
void dash_print_help(void)
{
    printf("  dash                     live one-shot overview: every device's values + status\r\n");
}

bool dash_handle_cmd(const char *line)
{
    if (strncmp(line, "dash", 4) != 0) return false;
    if (line[4] != '\0' && line[4] != ' ') return false;   /* not "dashXYZ" */
    const char *arg = line + 4; while (*arg == ' ') ++arg;
    if (!strcmp(arg, "help")) { dash_print_help(); return true; }
    if (*arg) { printf("[dash] usage: dash\r\n"); return true; }

    if (D.busy) { printf("[dash] already running - please wait\r\n"); return true; }

    memset(&D, 0, sizeof(D));
    int cnt = 0;
    for (int i = 0; i < MAX_DEVICES; ++i)
        if (g_dev[i].paired) { D.dev[i].active = true; ++cnt; }
    if (!cnt) { printf("[dash] no paired devices\r\n"); return true; }

    D.busy = true;
    printf("[dash] reading %d device(s) live (up to ~%.1fs each if asleep)...\r\n",
           cnt, DASH_DEV_TIMEOUT_MS / 1000.0);
    chip::DeviceLayer::PlatformMgr().ScheduleWork(dash_start_work, 0);
    return true;
}
