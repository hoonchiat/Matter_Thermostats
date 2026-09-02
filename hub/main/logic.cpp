/* logic.cpp - see logic.h.
 *
 * Structure:
 *   Part 1  Sensor table + latest-value cache  (fed by the subscription)
 *   Part 2  Rule table  (two-level term/group, multi-target; NVS-persisted)
 *   Part 3  Evaluation  (per-condition hysteresis, AND/OR/XOR/NOT, drive N targets)
 *   Part 4  Console command dispatch
 *   Part 5  Config backup / restore
 *
 * A rule is:  <term> [topgate <term>]...  ->  <action>  <target..>
 *   term   = <cond> | ( <cond> [gate <cond>]... )      (one level of grouping)
 *   cond   = [not] <sensor> [avg|min|max] #src.. <op> <value> [hyst <v>]
 *          | [not] schedule <n>            (scheduler 1..4 active now; schedule.cpp)
 *          | [not] calendar                (today is a public holiday)
 *   gate   = and | or | xor    (folded left-to-right, equal precedence)
 *   action = on | off | toggle             (applied to every target device)
 * Each TERM folds its conditions with its inner gates; the rule folds the term
 * truths with the top gates. TRACK rules auto-inverse when the rule clears;
 * ONESHOT rules fire once on the rising edge.
 */
#include "logic.h"
#include "device_model.h"
#include "schedule.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cinttypes>
#include "cJSON.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <esp_matter.h>
#include <esp_matter_controller_cluster_command.h>
#include <platform/CHIPDeviceLayer.h>
#include <system/SystemClock.h>

static const char *TAG = "logic";

/* Action codes (map to OnOff command ids in device_model.h). */
enum { ACT_OFF = 0, ACT_ON = 1, ACT_TOGGLE = 2 };
/* Comparison operators. */
enum { OP_GT = 0, OP_GE = 1, OP_LT = 2, OP_LE = 3 };
/* Aggregation across multiple source devices in one condition. */
enum { AGG_SINGLE = 0, AGG_AVG = 1, AGG_MIN = 2, AGG_MAX = 3 };
/* Boolean gate joining two conditions/terms. */
enum { GATE_AND = 0, GATE_OR = 1, GATE_XOR = 2 };
/* Rule behaviour when the combined condition clears. */
enum { MODE_TRACK = 0, MODE_ONESHOT = 1 };
/* Condition kind (v1.8). */
enum { CK_SENSOR = 0, CK_SCHEDULE = 1, CK_CALENDAR = 2 };

/* ===================================================================== *
 *  Part 1 - sensor table + latest-value cache
 * ===================================================================== *
 * The one source of truth for BOTH the subscription paths (every device is
 * subscribed to each cluster below, wildcard endpoint) and the rule keyword
 * vocabulary. To support a new measurement, add a row here. `scale` normalises
 * the raw MeasuredValue to human units. */
struct sensor_def_t {
    const char *key;
    uint32_t    cluster;
    uint32_t    attr;
    float       scale;
    const char *unit;
    float       def_hyst;   /* default hysteresis deadband, in user units */
};
static const sensor_def_t SENSORS[] = {
    { "temp",     0x0402UL, 0x0000UL, 0.01f, "C",     0.5f },
    { "humidity", 0x0405UL, 0x0000UL, 0.01f, "%",     2.0f },
    { "co2",      0x040DUL, 0x0000UL, 1.0f,  "ppm",   50.0f },
    { "pm25",     0x042AUL, 0x0000UL, 1.0f,  "ug/m3", 5.0f },
    /* Add a row to support another measurement; each row = one wildcard-endpoint
     * attribute path on every device's subscription. ALPSTUGA exposes PM2.5. */
};
#define N_SENSORS ((int)(sizeof(SENSORS) / sizeof(SENSORS[0])))

/* Latest value per (device slot, sensor). Written on the CHIP task (report cb),
 * read from both the CHIP task (eval) and the console task (listing). */
struct reading_t { bool valid; float value; int64_t ts_us; uint16_t ep; };
static reading_t g_readings[MAX_DEVICES][N_SENSORS];

static int sensor_by_key(const char *k)
{
    for (int i = 0; i < N_SENSORS; ++i) if (!strcmp(k, SENSORS[i].key)) return i;
    return -1;
}
static int sensor_by_path(uint32_t cl, uint32_t at)
{
    for (int i = 0; i < N_SENSORS; ++i) if (SENSORS[i].cluster == cl && SENSORS[i].attr == at) return i;
    return -1;
}

int  logic_sensor_path_count(void) { return N_SENSORS; }
void logic_sensor_path(int i, uint32_t *cluster, uint32_t *attr)
{
    if (i < 0 || i >= N_SENSORS) { if (cluster) *cluster = 0; if (attr) *attr = 0; return; }
    if (cluster) *cluster = SENSORS[i].cluster;
    if (attr)    *attr    = SENSORS[i].attr;
}

/* Sensor-cache accessors for the USB JSON `status` builder. */
int         logic_num_sensors(void)       { return N_SENSORS; }
const char *logic_sensor_name(int i)      { return (i >= 0 && i < N_SENSORS) ? SENSORS[i].key  : ""; }
const char *logic_sensor_unit(int i)      { return (i >= 0 && i < N_SENSORS) ? SENSORS[i].unit : ""; }
bool        logic_reading(int slot, int i, float *val)
{
    if (slot < 0 || slot >= MAX_DEVICES || i < 0 || i >= N_SENSORS || !g_readings[slot][i].valid) return false;
    if (val) *val = g_readings[slot][i].value;
    return true;
}

/* ===================================================================== *
 *  Part 2 - rule table (two-level: terms of conditions)
 * ===================================================================== */
#define MAX_LOGIC  12        /* rules                                     */
#define MAX_TERM   4         /* top-level terms per rule                  */
#define MAX_TCOND  3         /* conditions per term (one parenthesised group) */
#define MAX_SRC    4         /* source devices per condition (aggregate)  */
#define MAX_TARGET 4         /* target devices driven per rule            */

/* A single condition. For CK_SCHEDULE, `sensor` holds the scheduler index (0..3);
 * for CK_CALENDAR nothing else is used. For CK_SENSOR the sensor/agg/src/op/
 * threshold/hyst fields apply (as in v1.5). Devices are u8 SLOT indexes (node id
 * = NODE_ID_BASE + slot). */
struct cond_t {
    uint8_t  kind;                   /* CK_SENSOR / CK_SCHEDULE / CK_CALENDAR   */
    uint8_t  negate;                 /* NOT this condition                     */
    uint8_t  agg;                    /* sensor: AGG_SINGLE / AVG / MIN / MAX    */
    uint8_t  n_src;                  /* sensor: number of source slots          */
    uint8_t  sensor;                 /* sensor: SENSORS[] index; schedule: idx  */
    uint8_t  op;                     /* sensor: OP_GT / GE / LT / LE            */
    uint8_t  state;                  /* sensor: last RAW (pre-negate) truth     */
    uint8_t  src_slot[MAX_SRC];      /* sensor: source device slots (0-based)   */
    float    threshold;              /* sensor: user units                     */
    float    hyst;                   /* sensor: deadband, user units           */
};
struct term_t {
    uint8_t n_cond;
    cond_t  cond[MAX_TCOND];
    uint8_t gate[MAX_TCOND - 1];     /* gate[i] joins cond[i] and cond[i+1]     */
};
struct tgt_t { uint8_t slot; uint16_t ep; };

struct logic_rule_t {
    uint8_t  used;
    uint8_t  n_term;
    term_t   term[MAX_TERM];
    uint8_t  top_gate[MAX_TERM - 1]; /* top_gate[i] joins term[i] and term[i+1] */
    uint8_t  n_tgt;
    tgt_t    tgt[MAX_TARGET];
    uint8_t  on_action;              /* ACT_* applied when the rule is TRUE     */
    uint8_t  mode;                   /* MODE_TRACK / MODE_ONESHOT               */
    uint8_t  state;                  /* runtime: last combined truth (0/1)      */
};
static logic_rule_t g_rules[MAX_LOGIC];

#define NVS_NS  "matterhub"          /* same namespace as main.cpp / bindings   */
#define K_LOGIC "logicrule"          /* own key: blob of logic_rule_t[MAX_LOGIC] */

static void rules_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, K_LOGIC, g_rules, sizeof(g_rules));
    nvs_commit(h);
    nvs_close(h);
}

void logic_load(void)
{
    memset(g_rules, 0, sizeof(g_rules));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t stored = 0;
    /* Only accept an exactly-sized blob; a size mismatch (struct changed between
     * firmware versions) is ignored rather than misread - rules are re-creatable.
     * NB: the v1.8 term/group struct differs from every earlier cut, so rules
     * persisted by an older build are dropped once and must be re-added. */
    if (nvs_get_blob(h, K_LOGIC, nullptr, &stored) == ESP_OK && stored == sizeof(g_rules)) {
        size_t len = stored;
        nvs_get_blob(h, K_LOGIC, g_rules, &len);
    }
    nvs_close(h);
    int n = 0;
    for (int i = 0; i < MAX_LOGIC; ++i)
        if (g_rules[i].used) {
            g_rules[i].state = 0;
            for (int t = 0; t < g_rules[i].n_term; ++t)
                for (int c = 0; c < g_rules[i].term[t].n_cond; ++c)
                    g_rules[i].term[t].cond[c].state = 0;
            ++n;
        }
    if (n) ESP_LOGI(TAG, "Loaded %d logic rule(s)", n);
}

/* ===================================================================== *
 *  Part 3 - evaluation
 * ===================================================================== */
static const char *op_label(uint8_t op) { return op == OP_GT ? ">" : op == OP_GE ? ">=" : op == OP_LT ? "<" : "<="; }
static const char *act_label(uint8_t a) { return a == ACT_OFF ? "off" : a == ACT_ON ? "on" : "toggle"; }
static const char *gate_label(uint8_t g) { return g == GATE_AND ? "and" : g == GATE_OR ? "or" : "xor"; }
static const char *agg_label(uint8_t a) { return a == AGG_AVG ? "avg " : a == AGG_MIN ? "min " : a == AGG_MAX ? "max " : ""; }
static inline uint8_t act_inverse(uint8_t a) { return a == ACT_ON ? ACT_OFF : a == ACT_OFF ? ACT_ON : ACT_TOGGLE; }
static inline bool gate_apply(uint8_t g, bool a, bool b) { return g == GATE_AND ? (a && b) : g == GATE_OR ? (a || b) : (a != b); }

/* Aggregate one condition's source readings into a comparable value. Returns
 * false if not one source has reported yet. */
static bool agg_value(const cond_t &c, double &out)
{
    double acc = 0, mn = 0, mx = 0; int cnt = 0;
    for (int i = 0; i < c.n_src; ++i) {
        int slot = c.src_slot[i];
        if (slot >= MAX_DEVICES || !g_dev[slot].paired) continue;
        reading_t &rd = g_readings[slot][c.sensor];
        if (!rd.valid) continue;
        double v = rd.value;
        if (cnt == 0) { acc = mn = mx = v; }
        else { acc += v; if (v < mn) mn = v; if (v > mx) mx = v; }
        ++cnt;
    }
    if (cnt == 0) return false;
    out = (c.agg == AGG_MIN) ? mn : (c.agg == AGG_MAX) ? mx : (acc / cnt);
    return true;
}

/* Evaluate one condition to a boolean. `has_data` is set false if a SENSOR
 * condition has no reading yet (schedule/calendar always have data). Returns the
 * truth AFTER applying NOT. */
static bool cond_eval(cond_t &c, bool &has_data)
{
    if (c.kind == CK_SCHEDULE) { has_data = true; bool t = schedule_on(c.sensor); return c.negate ? !t : t; }
    if (c.kind == CK_CALENDAR) { has_data = true; bool t = calendar_on();         return c.negate ? !t : t; }

    /* CK_SENSOR: threshold compare with hysteresis. */
    double v;
    if (!agg_value(c, v)) { has_data = false; return c.negate ? true : false; }
    has_data = true;
    bool above = (c.op == OP_GT || c.op == OP_GE);
    bool prev  = c.state != 0;
    bool t;
    if (above) t = prev ? (v > (double)c.threshold - c.hyst)
                        : ((c.op == OP_GT) ? v >  c.threshold : v >= c.threshold);
    else       t = prev ? (v < (double)c.threshold + c.hyst)
                        : ((c.op == OP_LT) ? v <  c.threshold : v <= c.threshold);
    c.state = t ? 1 : 0;                                 /* store RAW truth for hysteresis */
    return c.negate ? !t : t;
}

/* Fold one term's conditions with its inner gates. `has_data` false if any
 * (sensor) condition is missing its reading. */
static bool term_eval(term_t &t, bool &has_data)
{
    bool combined = false, all = true;
    for (int i = 0; i < t.n_cond; ++i) {
        bool hd = true;
        bool ci = cond_eval(t.cond[i], hd);
        if (!hd) all = false;
        combined = (i == 0) ? ci : gate_apply(t.gate[i - 1], combined, ci);
    }
    has_data = all;
    return combined;
}

/* ---- drive an OnOff target ------------------------------------------------ */
static void act_ok(void *, const chip::app::ConcreteCommandPath &path,
                   const chip::app::StatusIB &status, chip::TLV::TLVReader *)
{
    printf("[logic] drive -> ep%u OnOff cmd 0x%02lx status=0x%02x\r\n",
           path.mEndpointId, (unsigned long)path.mCommandId,
           (unsigned)static_cast<uint8_t>(status.mStatus));
}
static void act_err(void *, CHIP_ERROR error)
{
    printf("[logic] drive FAILED: %" CHIP_ERROR_FORMAT "\r\n", error.Format());
}

/* Must run on the CHIP task (all callers do: report cb / tick timers). */
static void logic_send(uint64_t node, uint16_t ep, uint8_t action)
{
    uint32_t cmd = (action == ACT_OFF) ? CMD_ONOFF_OFF
                 : (action == ACT_ON)  ? CMD_ONOFF_ON
                                       : CMD_ONOFF_TOGGLE;
    auto *c = chip::Platform::New<esp_matter::controller::cluster_command>(
        node, ep, CL_ONOFF, cmd, "{}", chip::NullOptional, act_ok, act_err);
    if (!c) { ESP_LOGE(TAG, "OOM: logic cluster_command"); return; }
    if (c->send_command() != ESP_OK) { ESP_LOGE(TAG, "logic send failed"); chip::Platform::Delete(c); }
}

static void render_rule(const logic_rule_t &r, char *buf, size_t cap);   /* fwd */

/* Evaluate one rule and drive its targets on a combined edge. The rule only acts
 * once every condition has data. `reassert` re-sends a TRACK rule's current-state
 * action so a target that drifted realigns - skipped for toggle. */
static void eval_rule(int idx, bool reassert)
{
    logic_rule_t &r = g_rules[idx];
    if (!r.used || r.n_term == 0) return;

    bool combined = false, all_data = true;
    for (int i = 0; i < r.n_term; ++i) {
        bool hd = true;
        bool ti = term_eval(r.term[i], hd);
        if (!hd) all_data = false;
        combined = (i == 0) ? ti : gate_apply(r.top_gate[i - 1], combined, ti);
    }
    if (!all_data) return;                               /* wait until all inputs known */

    bool prev = r.state != 0;
    bool edge = (combined != prev);
    r.state = combined ? 1 : 0;

    uint8_t action = ACT_OFF; bool send = false;
    if (edge) {
        if (r.mode == MODE_ONESHOT) { if (combined) { action = r.on_action; send = true; } }
        else                        { action = combined ? r.on_action : act_inverse(r.on_action); send = true; }
        if (send) {
            char desc[220]; render_rule(r, desc, sizeof(desc));
            printf("[logic] rule[%d] %s -> firing %s\r\n", idx, desc, act_label(action));
        }
    } else if (reassert && r.mode == MODE_TRACK && r.on_action != ACT_TOGGLE) {
        action = combined ? r.on_action : act_inverse(r.on_action);
        send = true;
    }
    if (send)
        for (int k = 0; k < r.n_tgt; ++k)
            logic_send(slot_node_id(r.tgt[k].slot), r.tgt[k].ep, action);
}

/* Does a rule reference any sensor `si` (to re-eval on that sensor's report)? */
static bool rule_uses_sensor(const logic_rule_t &r, int si)
{
    for (int t = 0; t < r.n_term; ++t)
        for (int c = 0; c < r.term[t].n_cond; ++c)
            if (r.term[t].cond[c].kind == CK_SENSOR && r.term[t].cond[c].sensor == si) return true;
    return false;
}
/* Does a rule reference a time source (schedule/calendar), i.e. its truth can
 * change with the clock rather than with a report? */
static bool rule_uses_time(const logic_rule_t &r)
{
    for (int t = 0; t < r.n_term; ++t)
        for (int c = 0; c < r.term[t].n_cond; ++c)
            if (r.term[t].cond[c].kind == CK_SCHEDULE || r.term[t].cond[c].kind == CK_CALENDAR) return true;
    return false;
}

void logic_on_report(uint64_t node_id, uint16_t endpoint_id,
                     uint32_t cluster_id, uint32_t attr_id, double value)
{
    int slot = dev_slot_by_node(node_id);
    if (slot < 0) return;
    int si = sensor_by_path(cluster_id, attr_id);
    if (si < 0) return;                                 /* not a sensor we track */

    reading_t &rd = g_readings[slot][si];
    rd.valid = true;
    rd.value = (float)(value * SENSORS[si].scale);
    rd.ts_us = esp_timer_get_time();
    rd.ep    = endpoint_id;

    for (int i = 0; i < MAX_LOGIC; ++i)
        if (g_rules[i].used && rule_uses_sensor(g_rules[i], si)) eval_rule(i, false);
}

/* Periodic safety re-evaluation / TRACK re-assert (all rules). */
#define LOGIC_TICK_MS 120000
static void logic_tick(chip::System::Layer *, void *)
{
    for (int i = 0; i < MAX_LOGIC; ++i)
        if (g_rules[i].used) eval_rule(i, true);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(LOGIC_TICK_MS), logic_tick, nullptr);
}

/* Faster tick for TIME-dependent rules only: catch schedule/calendar edges within
 * ~5 s. Safe to run often: eval_rule(reassert=false) only sends on an actual edge,
 * so this adds no traffic between transitions - it just re-checks schedule_on(). */
#define SCHED_TICK_MS 5000
static void sched_tick(chip::System::Layer *, void *)
{
    for (int i = 0; i < MAX_LOGIC; ++i)
        if (g_rules[i].used && rule_uses_time(g_rules[i])) eval_rule(i, false);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(SCHED_TICK_MS), sched_tick, nullptr);
}

void logic_start(void)
{
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(LOGIC_TICK_MS), logic_tick, nullptr);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(SCHED_TICK_MS), sched_tick, nullptr);
}

/* Evaluate a single rule on the CHIP task (used right after 'logic add'). */
static void logic_eval_one_work(intptr_t idx) { eval_rule((int)idx, false); }

/* ===================================================================== *
 *  Part 4 - console dispatch
 * ===================================================================== */
static bool tok_is_op(const char *t)
{
    return !strcmp(t, ">") || !strcmp(t, "<") || !strcmp(t, ">=") || !strcmp(t, "<=");
}
static bool tok_is_gate(const char *t) { return !strcmp(t, "and") || !strcmp(t, "or") || !strcmp(t, "xor"); }
static bool tok_is_action(const char *t) { return !strcmp(t, "on") || !strcmp(t, "off") || !strcmp(t, "toggle"); }
static bool tok_is_slot(const char *t) { return t[0] == '#' || (t[0] >= '0' && t[0] <= '9'); }
static uint8_t gate_from_tok(const char *t) { return !strcmp(t, "and") ? GATE_AND : !strcmp(t, "or") ? GATE_OR : GATE_XOR; }

/* Parse a '#n' or 'n' slot token into a paired device (1-based in the UI). */
static DeviceInfo *slot_from_tok(const char *t, int *slot)
{
    if (*t == '#') ++t;
    int n = atoi(t);
    if (n < 1 || n > MAX_DEVICES || !g_dev[n - 1].paired) {
        printf("[logic] no paired device #%s (see 'devices')\r\n", t);
        return nullptr;
    }
    if (slot) *slot = n - 1;
    return &g_dev[n - 1];
}

/* First endpoint on d exposing a server OnOff cluster, or -1. */
static int onoff_ep(DeviceInfo &d)
{
    for (int i = 0; i < d.n_eps; ++i)
        if (d.eps[i] && ep_find_cluster(*d.eps[i], CL_ONOFF)) return (int)d.eps[i]->id;
    return -1;
}

/* Render one condition into buf at *off. */
static void render_cond(const cond_t &c, char *buf, size_t cap, int &off)
{
    if (c.negate) off += snprintf(buf + off, cap - off, "!");
    if (c.kind == CK_SCHEDULE) { off += snprintf(buf + off, cap - off, "schedule %d", c.sensor + 1); return; }
    if (c.kind == CK_CALENDAR) { off += snprintf(buf + off, cap - off, "calendar");                return; }
    const sensor_def_t &s = SENSORS[c.sensor];
    off += snprintf(buf + off, cap - off, "%s %s", s.key, agg_label(c.agg));
    for (int k = 0; k < c.n_src; ++k) off += snprintf(buf + off, cap - off, "%s#%d", k ? "," : "", c.src_slot[k] + 1);
    off += snprintf(buf + off, cap - off, "%s%.1f%s", op_label(c.op), (double)c.threshold, s.unit);
}

/* Build a human-readable "term topgate term ... -> action targets [mode]" string. */
static void render_rule(const logic_rule_t &r, char *buf, size_t cap)
{
    int off = 0; if (cap) buf[0] = '\0';
    for (int t = 0; t < r.n_term && off < (int)cap; ++t) {
        if (t > 0) off += snprintf(buf + off, cap - off, " %s ", gate_label(r.top_gate[t - 1]));
        const term_t &tm = r.term[t];
        bool grp = (tm.n_cond > 1);
        if (grp) off += snprintf(buf + off, cap - off, "( ");
        for (int c = 0; c < tm.n_cond && off < (int)cap; ++c) {
            if (c > 0) off += snprintf(buf + off, cap - off, " %s ", gate_label(tm.gate[c - 1]));
            render_cond(tm.cond[c], buf, cap, off);
        }
        if (grp) off += snprintf(buf + off, cap - off, " )");
    }
    off += snprintf(buf + off, cap - off, " -> %s ", act_label(r.on_action));
    for (int k = 0; k < r.n_tgt; ++k) off += snprintf(buf + off, cap - off, "%s#%d", k ? "," : "", r.tgt[k].slot + 1);
    off += snprintf(buf + off, cap - off, " [%s]", r.mode == MODE_ONESHOT ? "oneshot" : "track");
}

void logic_print_help(void)
{
    printf("  logic                    list value-triggered rules + live sensor values\r\n"
           "  logic add <term> [and|or|xor <term>]... <on|off|toggle> #d [#d2..] [oneshot]\r\n"
           "        <term> = <cond>  |  ( <cond> [and|or|xor <cond>]... )   (one group level)\r\n"
           "        <cond> = [not] <sensor> [avg|min|max] #s.. <op> <val> [hyst <v>]\r\n"
           "               | [not] schedule <n>   (scheduler 1..%d on now)\r\n"
           "               | [not] calendar       (today is a public holiday)\r\n"
           "        sensors: temp humidity co2 pm25 ; op: > >= < <= ; default track\r\n"
           "        e.g. logic add temp #1 > 28 on #4\r\n"
           "             logic add temp #1 > 28 and schedule 1 on #5\r\n"
           "             logic add calendar and ( temp #2 > 25 or co2 #2 > 10000 ) on #4\r\n"
           "  logic rm <idx>           remove rule #idx (see 'logic')\r\n", SCHED_MAX);
}

static void print_rules(void)
{
    printf("[logic] value-triggered rules:\r\n");
    int n = 0;
    for (int i = 0; i < MAX_LOGIC; ++i) {
        logic_rule_t &r = g_rules[i];
        if (!r.used) continue;
        ++n;
        char desc[220]; render_rule(r, desc, sizeof(desc));
        printf("  [%d] %s  state=%s\r\n", i, desc, r.state ? "ON" : "OFF");
    }
    if (!n) printf("  (none)\r\n");

    printf("[logic] live sensor values:\r\n");
    bool anydev = false;
    for (int d = 0; d < MAX_DEVICES; ++d) {
        if (!g_dev[d].paired) continue;
        char row[224], nm[72];
        int off = snprintf(row, sizeof(row), "  #%d %s:", d + 1, dev_display_name(g_dev[d], nm, sizeof(nm)));
        bool any = false;
        for (int si = 0; si < N_SENSORS && off < (int)sizeof(row); ++si) {
            reading_t &rd = g_readings[d][si];
            if (!rd.valid) continue;
            any = true;
            off += snprintf(row + off, sizeof(row) - off, " %s=%.1f%s", SENSORS[si].key, rd.value, SENSORS[si].unit);
        }
        if (g_dev[d].payload[0] && off < (int)sizeof(row))
            off += snprintf(row + off, sizeof(row) - off, " payload=%s", g_dev[d].payload);
        if (any) { printf("%s\r\n", row); anydev = true; }
    }
    if (!anydev) printf("  (no sensor reports yet)\r\n");
}

/* Parse one condition (sensor / schedule <n> / calendar) starting at argv[*ip];
 * advance *ip past it. Returns false on a parse error (message already printed). */
static bool parse_one_cond(int argc, char **argv, int *ip, cond_t &c)
{
    int i = *ip;
    memset(&c, 0, sizeof(c));

    if (i < argc && !strcmp(argv[i], "not")) { c.negate = 1; ++i; }
    if (i >= argc) { printf("[logic] expected a condition\r\n"); return false; }

    if (!strcmp(argv[i], "schedule") || !strcmp(argv[i], "sched")) {
        ++i;
        if (i >= argc) { printf("[logic] 'schedule' needs a number 1..%d\r\n", SCHED_MAX); return false; }
        int sn = atoi(argv[i]);
        if (sn < 1 || sn > SCHED_MAX) { printf("[logic] schedule must be 1..%d\r\n", SCHED_MAX); return false; }
        c.kind = CK_SCHEDULE; c.sensor = (uint8_t)(sn - 1); ++i; *ip = i; return true;
    }
    if (!strcmp(argv[i], "calendar") || !strcmp(argv[i], "holiday") || !strcmp(argv[i], "cal")) {
        c.kind = CK_CALENDAR; ++i; *ip = i; return true;
    }

    /* sensor condition */
    c.kind = CK_SENSOR;
    int si = sensor_by_key(argv[i]);
    if (si < 0) { printf("[logic] unknown condition '%s' (sensor, 'schedule <n>', or 'calendar')\r\n", argv[i]); return false; }
    c.sensor = (uint8_t)si; ++i;

    c.agg = AGG_SINGLE;
    if (i < argc) {
        if      (!strcmp(argv[i], "avg")) { c.agg = AGG_AVG; ++i; }
        else if (!strcmp(argv[i], "min")) { c.agg = AGG_MIN; ++i; }
        else if (!strcmp(argv[i], "max")) { c.agg = AGG_MAX; ++i; }
    }

    int ns = 0;
    while (i < argc && !tok_is_op(argv[i])) {
        if (ns >= MAX_SRC) { printf("[logic] too many sources in one condition (max %d)\r\n", MAX_SRC); return false; }
        int slot; DeviceInfo *d = slot_from_tok(argv[i], &slot);
        if (!d) return false;
        c.src_slot[ns++] = (uint8_t)slot; ++i;
    }
    if (ns == 0) { printf("[logic] condition has no source device (e.g. '#1')\r\n"); return false; }
    c.n_src = (uint8_t)ns;

    if (i >= argc || !tok_is_op(argv[i])) { printf("[logic] expected an operator: > >= < <=\r\n"); return false; }
    c.op = !strcmp(argv[i], ">") ? OP_GT : !strcmp(argv[i], ">=") ? OP_GE
         : !strcmp(argv[i], "<") ? OP_LT : OP_LE;
    ++i;

    if (i >= argc) { printf("[logic] expected a threshold value\r\n"); return false; }
    c.threshold = strtof(argv[i], nullptr); ++i;

    c.hyst = SENSORS[si].def_hyst;
    if (i + 1 < argc && !strcmp(argv[i], "hyst")) { c.hyst = strtof(argv[i + 1], nullptr); i += 2; }
    if (c.hyst < 0) c.hyst = 0;

    if (ns > 1 && c.agg == AGG_SINGLE) {
        printf("[logic] multiple sources need avg/min/max, e.g. 'co2 avg #1 #2 > 1250'\r\n"); return false; }

    *ip = i;
    return true;
}

/* Parse one term: a single condition, or a parenthesised group of conditions.
 * Tokens '(' and ')' must be space-separated. */
static bool parse_term(int argc, char **argv, int *ip, term_t &t)
{
    int i = *ip;
    memset(&t, 0, sizeof(t));

    if (i < argc && !strcmp(argv[i], "(")) {
        ++i;                                            /* consume '(' */
        for (;;) {
            if (t.n_cond >= MAX_TCOND) { printf("[logic] too many conditions in a group (max %d)\r\n", MAX_TCOND); return false; }
            if (!parse_one_cond(argc, argv, &i, t.cond[t.n_cond])) return false;
            ++t.n_cond;
            if (i < argc && tok_is_gate(argv[i])) {
                if (t.n_cond >= MAX_TCOND) { printf("[logic] too many conditions in a group (max %d)\r\n", MAX_TCOND); return false; }
                t.gate[t.n_cond - 1] = gate_from_tok(argv[i]); ++i; continue;
            }
            break;
        }
        if (i >= argc || strcmp(argv[i], ")")) { printf("[logic] expected ')' to close the group\r\n"); return false; }
        ++i;                                            /* consume ')' */
    } else {
        if (!parse_one_cond(argc, argv, &i, t.cond[0])) return false;
        t.n_cond = 1;
    }
    *ip = i;
    return true;
}

static void do_add(int argc, char **argv)
{
    /* argv[0] == "add" ; terms/gates/action/targets follow. */
    term_t  terms[MAX_TERM];
    uint8_t topg[MAX_TERM - 1];
    int nt = 0;
    int i = 1;

    for (;;) {
        if (nt >= MAX_TERM) { printf("[logic] too many terms (max %d)\r\n", MAX_TERM); return; }
        if (!parse_term(argc, argv, &i, terms[nt])) return;
        ++nt;
        if (i < argc && tok_is_gate(argv[i])) {
            if (nt >= MAX_TERM) { printf("[logic] too many terms (max %d)\r\n", MAX_TERM); return; }
            topg[nt - 1] = gate_from_tok(argv[i]); ++i; continue;
        }
        break;
    }

    if (i >= argc || !tok_is_action(argv[i])) { printf("[logic] expected an action: on / off / toggle\r\n"); return; }
    uint8_t action = !strcmp(argv[i], "on") ? ACT_ON : !strcmp(argv[i], "off") ? ACT_OFF : ACT_TOGGLE;
    ++i;

    tgt_t tgts[MAX_TARGET]; int ntg = 0;
    while (i < argc && tok_is_slot(argv[i])) {
        if (ntg >= MAX_TARGET) { printf("[logic] too many targets (max %d)\r\n", MAX_TARGET); return; }
        int ds; DeviceInfo *dd = slot_from_tok(argv[i], &ds);
        if (!dd) return;
        if (!dd->enumerated) { printf("[logic] target #%d not enumerated yet - 'scan %d' first\r\n", ds + 1, ds + 1); return; }
        int dep = onoff_ep(*dd);
        if (dep < 0) { printf("[logic] target #%d has no OnOff endpoint (try 'bindable %d')\r\n", ds + 1, ds + 1); return; }
        tgts[ntg].slot = (uint8_t)ds; tgts[ntg].ep = (uint16_t)dep; ++ntg; ++i;
    }
    if (ntg == 0) { printf("[logic] no target device given (e.g. '#4')\r\n"); return; }

    uint8_t mode = MODE_TRACK;
    while (i < argc) {
        if      (!strcmp(argv[i], "oneshot")) { mode = MODE_ONESHOT; ++i; }
        else if (!strcmp(argv[i], "track"))   { mode = MODE_TRACK;   ++i; }
        else { printf("[logic] unexpected token '%s' ('logic help')\r\n", argv[i]); return; }
    }

    int idx = -1;
    for (int k = 0; k < MAX_LOGIC; ++k) if (!g_rules[k].used) { idx = k; break; }
    if (idx < 0) { printf("[logic] rule table full (%d) - 'logic rm <idx>' first\r\n", MAX_LOGIC); return; }

    logic_rule_t &r = g_rules[idx];
    memset(&r, 0, sizeof(r));
    r.used = 1; r.n_term = (uint8_t)nt;
    for (int k = 0; k < nt; ++k)     r.term[k] = terms[k];
    for (int k = 0; k < nt - 1; ++k) r.top_gate[k] = topg[k];
    r.n_tgt = (uint8_t)ntg;
    for (int k = 0; k < ntg; ++k)    r.tgt[k] = tgts[k];
    r.on_action = action; r.mode = mode; r.state = 0;
    rules_save();

    char desc[220]; render_rule(r, desc, sizeof(desc));
    printf("[logic] added rule [%d]: %s\r\n", idx, desc);

    /* Act immediately if the sensors have already reported (schedule on CHIP task). */
    chip::DeviceLayer::PlatformMgr().ScheduleWork(logic_eval_one_work, (intptr_t)idx);
}

bool logic_handle_cmd(const char *line)
{
    char buf[224];
    strncpy(buf, line, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
    char *argv[40]; int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 40; t = strtok(nullptr, " ")) argv[argc++] = t;
    if (argc == 0) return false;
    if (strcmp(argv[0], "logic")) return false;         /* not ours */

    if (argc == 1 || !strcmp(argv[1], "list")) { print_rules(); return true; }
    if (!strcmp(argv[1], "help")) { logic_print_help(); return true; }

    if (!strcmp(argv[1], "rm") && argc >= 3) {
        int idx = atoi(argv[2]);
        if (idx < 0 || idx >= MAX_LOGIC || !g_rules[idx].used) {
            printf("[logic] no rule [%s] (see 'logic')\r\n", argv[2]); return true; }
        g_rules[idx].used = 0; rules_save();
        printf("[logic] removed rule [%d]\r\n", idx);
        return true;
    }

    if (!strcmp(argv[1], "add")) { do_add(argc - 1, argv + 1); return true; }

    printf("[logic] unknown subcommand '%s' ('logic help')\r\n", argv[1]);
    return true;
}

/* --- structured entry points (shared by the console + the JSON protocol) ---- */
bool logic_add_expr(const char *expr, char *err, size_t cap)
{
    int before = 0; for (int i = 0; i < MAX_LOGIC; ++i) if (g_rules[i].used) ++before;
    char buf[224]; snprintf(buf, sizeof(buf), "add %s", expr ? expr : "");
    char *argv[40]; int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 40; t = strtok(nullptr, " ")) argv[argc++] = t;
    do_add(argc, argv);   /* prints any parse error to the console */
    int after = 0; for (int i = 0; i < MAX_LOGIC; ++i) if (g_rules[i].used) ++after;
    if (after > before) return true;
    if (err) snprintf(err, cap, "invalid logic rule (see console output for detail)");
    return false;
}

bool logic_rm_idx(int idx, char *err, size_t cap)
{
    if (idx < 0 || idx >= MAX_LOGIC || !g_rules[idx].used) { if (err) snprintf(err, cap, "no rule [%d]", idx); return false; }
    g_rules[idx].used = 0; rules_save();
    return true;
}

cJSON *logic_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return nullptr;
    for (int i = 0; i < MAX_LOGIC; ++i) {
        if (!g_rules[i].used) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "idx", i);
        char d[220]; render_rule(g_rules[i], d, sizeof(d));
        cJSON_AddStringToObject(o, "text", d);
        cJSON_AddBoolToObject(o, "state", g_rules[i].state != 0);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* ===================================================================== *
 *  Part 5 - config backup / restore (JSON, device-index keyed)
 * ===================================================================== */
static const char *agg_key(uint8_t a)  { return a == AGG_AVG ? "avg" : a == AGG_MIN ? "min" : a == AGG_MAX ? "max" : "single"; }
static const char *gate_key(uint8_t g) { return g == GATE_AND ? "and" : g == GATE_OR ? "or" : "xor"; }
static uint8_t agg_from(const char *s)  { return (s && !strcmp(s,"avg")) ? AGG_AVG : (s && !strcmp(s,"min")) ? AGG_MIN : (s && !strcmp(s,"max")) ? AGG_MAX : AGG_SINGLE; }
static uint8_t gate_from(const char *s) { return (s && !strcmp(s,"or")) ? GATE_OR : (s && !strcmp(s,"xor")) ? GATE_XOR : GATE_AND; }
static uint8_t op_from(const char *s)   { return (s && !strcmp(s,">=")) ? OP_GE : (s && !strcmp(s,"<")) ? OP_LT : (s && !strcmp(s,"<=")) ? OP_LE : OP_GT; }
static uint8_t act_from(const char *s)  { return (s && !strcmp(s,"off")) ? ACT_OFF : (s && !strcmp(s,"toggle")) ? ACT_TOGGLE : ACT_ON; }

static cJSON *cond_to_json(const cond_t &cd, const int *slot2idx)
{
    cJSON *co = cJSON_CreateObject();
    cJSON_AddBoolToObject(co, "not", cd.negate != 0);
    if (cd.kind == CK_SCHEDULE) {
        cJSON_AddStringToObject(co, "kind", "schedule");
        cJSON_AddNumberToObject(co, "n", cd.sensor + 1);
    } else if (cd.kind == CK_CALENDAR) {
        cJSON_AddStringToObject(co, "kind", "calendar");
    } else {
        cJSON_AddStringToObject(co, "kind", "sensor");
        cJSON_AddStringToObject(co, "sensor", SENSORS[cd.sensor].key);
        cJSON_AddStringToObject(co, "agg", agg_key(cd.agg));
        cJSON *srcs = cJSON_AddArrayToObject(co, "srcs");
        for (int k = 0; k < cd.n_src; ++k)
            cJSON_AddItemToArray(srcs, cJSON_CreateNumber(slot2idx[cd.src_slot[k]]));
        cJSON_AddStringToObject(co, "op", op_label(cd.op));
        cJSON_AddNumberToObject(co, "thr", cd.threshold);
        cJSON_AddNumberToObject(co, "hyst", cd.hyst);
    }
    return co;
}

cJSON *logic_to_json(const int *slot2idx)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return nullptr;
    for (int i = 0; i < MAX_LOGIC; ++i) {
        logic_rule_t &r = g_rules[i];
        if (!r.used) continue;
        cJSON *ro = cJSON_CreateObject();
        cJSON *terms = cJSON_AddArrayToObject(ro, "terms");
        for (int t = 0; t < r.n_term; ++t) {
            cJSON *to = cJSON_CreateObject();
            cJSON *conds = cJSON_AddArrayToObject(to, "conds");
            for (int c = 0; c < r.term[t].n_cond; ++c)
                cJSON_AddItemToArray(conds, cond_to_json(r.term[t].cond[c], slot2idx));
            cJSON *gates = cJSON_AddArrayToObject(to, "gates");
            for (int g = 0; g < r.term[t].n_cond - 1; ++g)
                cJSON_AddItemToArray(gates, cJSON_CreateString(gate_key(r.term[t].gate[g])));
            cJSON_AddItemToArray(terms, to);
        }
        cJSON *tg = cJSON_AddArrayToObject(ro, "top_gates");
        for (int g = 0; g < r.n_term - 1; ++g)
            cJSON_AddItemToArray(tg, cJSON_CreateString(gate_key(r.top_gate[g])));
        cJSON_AddStringToObject(ro, "action", act_label(r.on_action));
        cJSON_AddStringToObject(ro, "mode", r.mode == MODE_ONESHOT ? "oneshot" : "track");
        cJSON *tgts = cJSON_AddArrayToObject(ro, "targets");
        for (int t = 0; t < r.n_tgt; ++t) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "dev", slot2idx[r.tgt[t].slot]);
            cJSON_AddNumberToObject(o, "ep", r.tgt[t].ep);
            cJSON_AddItemToArray(tgts, o);
        }
        cJSON_AddItemToArray(arr, ro);
    }
    return arr;
}

/* Resolve a backup device index to a current slot; on failure record it for the
 * skip report. Returns -1 if unresolved. */
static int resolve_dev(const restore_map *m, const cJSON *num, int *missing_idx)
{
    int di = (int)cJSON_GetNumberValue(num);
    if (di < 0 || di >= m->n_dev || m->idx2slot[di] < 0) { *missing_idx = di; return -1; }
    return m->idx2slot[di];
}

/* Parse one condition object into cd; returns false + sets *miss on an unresolved
 * device reference (or on a malformed sensor). */
static bool cond_from_json(const cJSON *co, const restore_map *m, cond_t &cd, int *miss)
{
    memset(&cd, 0, sizeof(cd));
    cd.negate = cJSON_IsTrue(cJSON_GetObjectItem(co, "not")) ? 1 : 0;
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(co, "kind"));
    if (kind && !strcmp(kind, "schedule")) {
        int sn = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(co, "n"));
        if (sn < 1 || sn > SCHED_MAX) return false;
        cd.kind = CK_SCHEDULE; cd.sensor = (uint8_t)(sn - 1);
        return true;
    }
    if (kind && !strcmp(kind, "calendar")) { cd.kind = CK_CALENDAR; return true; }

    cd.kind = CK_SENSOR;
    int si = sensor_by_key(cJSON_GetStringValue(cJSON_GetObjectItem(co, "sensor")) ?: "");
    if (si < 0) return false;
    cd.sensor = (uint8_t)si;
    cd.agg = agg_from(cJSON_GetStringValue(cJSON_GetObjectItem(co, "agg")));
    const cJSON *srcs = cJSON_GetObjectItem(co, "srcs"), *sv;
    cJSON_ArrayForEach(sv, srcs) {
        if (cd.n_src >= MAX_SRC) return false;
        int slot = resolve_dev(m, sv, miss);
        if (slot < 0) return false;
        cd.src_slot[cd.n_src++] = (uint8_t)slot;
    }
    cd.op = op_from(cJSON_GetStringValue(cJSON_GetObjectItem(co, "op")));
    cd.threshold = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(co, "thr"));
    cd.hyst = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(co, "hyst"));
    return true;
}

void logic_apply_json(const cJSON *arr, const restore_map *m, int *applied, int *skipped)
{
    int ap = 0, sk = 0, out = 0;
    memset(g_rules, 0, sizeof(g_rules));
    const cJSON *ro;
    cJSON_ArrayForEach(ro, arr) {
        if (out >= MAX_LOGIC) { ++sk; continue; }
        logic_rule_t r; memset(&r, 0, sizeof(r));
        bool ok = true; int miss = -1; int total_cond = 0;

        const cJSON *terms = cJSON_GetObjectItem(ro, "terms"), *to;
        cJSON_ArrayForEach(to, terms) {
            if (r.n_term >= MAX_TERM) { ok = false; break; }
            term_t &tm = r.term[r.n_term];
            const cJSON *conds = cJSON_GetObjectItem(to, "conds"), *co;
            cJSON_ArrayForEach(co, conds) {
                if (tm.n_cond >= MAX_TCOND) { ok = false; break; }
                if (!cond_from_json(co, m, tm.cond[tm.n_cond], &miss)) { ok = false; break; }
                ++tm.n_cond; ++total_cond;
            }
            if (!ok) break;
            const cJSON *gates = cJSON_GetObjectItem(to, "gates"), *gv; int gi = 0;
            cJSON_ArrayForEach(gv, gates) { if (gi < MAX_TCOND - 1) tm.gate[gi++] = gate_from(cJSON_GetStringValue(gv)); }
            ++r.n_term;
        }
        if (ok) {
            const cJSON *tg = cJSON_GetObjectItem(ro, "top_gates"), *gv; int gi = 0;
            cJSON_ArrayForEach(gv, tg) { if (gi < MAX_TERM - 1) r.top_gate[gi++] = gate_from(cJSON_GetStringValue(gv)); }
        }
        r.on_action = act_from(cJSON_GetStringValue(cJSON_GetObjectItem(ro, "action")));
        const char *md = cJSON_GetStringValue(cJSON_GetObjectItem(ro, "mode"));
        r.mode = (md && !strcmp(md, "oneshot")) ? MODE_ONESHOT : MODE_TRACK;

        const cJSON *tgts = cJSON_GetObjectItem(ro, "targets"), *tv;
        if (ok) cJSON_ArrayForEach(tv, tgts) {
            if (r.n_tgt >= MAX_TARGET) { ok = false; break; }
            int slot = resolve_dev(m, cJSON_GetObjectItem(tv, "dev"), &miss);
            if (slot < 0) { ok = false; break; }
            r.tgt[r.n_tgt].slot = (uint8_t)slot;
            r.tgt[r.n_tgt].ep = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(tv, "ep"));
            ++r.n_tgt;
        }

        if (ok && r.n_term > 0 && total_cond > 0 && r.n_tgt > 0) {
            r.used = 1; g_rules[out++] = r; ++ap;
        } else {
            ++sk;
            if (miss >= 0)
                printf("  SKIPPED logic rule: references device \"%s\" (not paired)\r\n",
                       (miss < m->n_dev && m->idx_name[miss]) ? m->idx_name[miss] : "?");
            else
                printf("  SKIPPED logic rule: malformed / too complex\r\n");
        }
    }
    rules_save();
    if (applied) *applied = ap;
    if (skipped) *skipped = sk;
}
