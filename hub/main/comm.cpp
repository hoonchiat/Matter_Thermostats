/* comm.cpp - see comm.h. JSON request/response protocol over native USB-Serial-JTAG. */
#include "comm.h"
#include "device_model.h"
#include "logic.h"
#include "bindings.h"
#include "schedule.h"
#include "dash.h"
#include "ble_scan.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cinttypes>
#include <ctime>
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"

#include "tlv_decode.h"
#include <esp_matter.h>
#include <esp_matter_controller_read_command.h>
#include <esp_matter_controller_write_command.h>
#include <esp_matter_controller_cluster_command.h>
#include <platform/CHIPDeviceLayer.h>

using chip::app::AttributePathParams;
using chip::app::ConcreteDataAttributePath;
using chip::app::EventPathParams;
using chip::Platform::ScopedMemoryBufferWithSize;
using chip::TLV::TLVReader;

static const char *TAG = "comm";

/* ---- write a response to the USB-SJ port in small chunks (a response can be
 * larger than the driver's TX ring; write <=256 B at a time, retrying briefly so
 * a large body drains as the host reads). Gives up if the host really isn't. */
static void comm_write(const char *s, size_t n)
{
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off; if (chunk > 256) chunk = 256;
        int tries = 0; bool wrote = false;
        while (tries < 25) {
            int w = usb_serial_jtag_write_bytes(s + off, chunk, pdMS_TO_TICKS(200));
            if (w > 0) { off += (size_t)w; wrote = true; break; }
            ++tries;
        }
        if (!wrote) break;   /* host not draining - drop the rest */
    }
}

/* Emit one NDJSON response line. Takes ownership of `result`. */
static void respond(int id, bool have_id, bool ok, cJSON *result, const char *err)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { if (result) cJSON_Delete(result); return; }
    if (have_id) cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", ok);
    if (ok) { if (result) cJSON_AddItemToObject(root, "result", result); }
    else    { cJSON_AddStringToObject(root, "error", err ? err : "error"); if (result) cJSON_Delete(result); }
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (js) { comm_write(js, strlen(js)); comm_write("\n", 1); cJSON_free(js); }
    else    { const char *e = "{\"ok\":false,\"error\":\"encode failed (low heap)\"}\n"; comm_write(e, strlen(e)); }
}

/* ---- structured device status (sensors + reachability + battery) ---- */
static cJSON *status_json(int only_slot)
{
    int64_t now = esp_timer_get_time();
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "devices");
    int nsens = logic_num_sensors();
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (!g_dev[i].paired) continue;
        if (only_slot >= 0 && i != only_slot) continue;
        DeviceInfo &d = g_dev[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", i + 1);
        char nid[24]; snprintf(nid, sizeof(nid), "0x%016" PRIx64, d.node_id);
        cJSON_AddStringToObject(o, "node_id", nid);
        cJSON_AddStringToObject(o, "name", d.name);
        cJSON_AddStringToObject(o, "label", d.label);
        cJSON_AddStringToObject(o, "payload", d.payload);
        cJSON_AddNumberToObject(o, "vid", d.vid);
        cJSON_AddNumberToObject(o, "pid", d.pid);
        cJSON_AddBoolToObject(o, "enumerated", d.enumerated);
        int age = d.has_data ? (int)((now - d.last_report_us) / 1000000) : -1;
        cJSON_AddNumberToObject(o, "age_s", age);
        cJSON_AddBoolToObject(o, "online", d.has_data && age >= 0 && age < 60);
        if (d.link_valid) { cJSON_AddNumberToObject(o, "rssi", d.rssi); cJSON_AddNumberToObject(o, "lqi", d.lqi); }
        cJSON *sv = cJSON_AddObjectToObject(o, "sensors");
        for (int s = 0; s < nsens; ++s) { float v; if (logic_reading(i, s, &v)) cJSON_AddNumberToObject(sv, logic_sensor_name(s), v); }
        int pct, bage; if (dash_get_battery(i, &pct, &bage)) { cJSON_AddNumberToObject(o, "battery", pct); cJSON_AddNumberToObject(o, "battery_age_s", bage); }
        /* OnOff: whether the device exposes it (from the enumerated tree) + last cached state. */
        bool hasoo = false;
        for (int e = 0; e < d.n_eps && !hasoo; ++e) if (d.eps[e] && ep_find_cluster(*d.eps[e], CL_ONOFF)) hasoo = true;
        cJSON_AddBoolToObject(o, "hasOnOff", hasoo);
        bool onv; if (dash_get_onoff(i, &onv)) cJSON_AddNumberToObject(o, "onoff", onv ? 1 : 0);
        /* Dimmable / colour light: expose LevelControl (0x0008) + ColorControl (0x0300)
         * presence so the portal can offer brightness + colour controls. */
        bool haslvl = false, hascol = false; uint16_t lep = 1;
        for (int e = 0; e < d.n_eps; ++e) if (d.eps[e]) {
            if (ep_find_cluster(*d.eps[e], 0x0008UL)) { haslvl = true; lep = d.eps[e]->id; }
            if (ep_find_cluster(*d.eps[e], 0x0300UL)) { hascol = true; lep = d.eps[e]->id; }
        }
        cJSON_AddBoolToObject(o, "hasLevel", haslvl);
        cJSON_AddBoolToObject(o, "hasColor", hascol);
        if (haslvl || hascol) cJSON_AddNumberToObject(o, "lightEp", lep);
        cJSON_AddItemToArray(arr, o);
    }
    if (hub_restore_pending()) cJSON_AddBoolToObject(root, "restore_pending", true);
    return root;
}

/* ---- small request accessors ---- */
static int json_int(const cJSON *o, const char *k, int def) { cJSON *v = cJSON_GetObjectItem(o, k); return (v && cJSON_IsNumber(v)) ? (int)cJSON_GetNumberValue(v) : def; }
static const char *json_str(const cJSON *o, const char *k) { return cJSON_GetStringValue(cJSON_GetObjectItem(o, k)); }

static bool parse_tz(const cJSON *req, int *mins)
{
    cJSON *v = cJSON_GetObjectItem(req, "offset");
    if (!v) return false;
    if (cJSON_IsNumber(v)) { *mins = (int)cJSON_GetNumberValue(v); return true; }
    const char *s = cJSON_GetStringValue(v);
    if (!s) return false;
    if (s[0] == 'Z' || s[0] == 'z') { *mins = 0; return true; }
    int sg = 1; if (s[0] == '+') ++s; else if (s[0] == '-') { sg = -1; ++s; }
    int h = 0, m = 0; if (sscanf(s, "%d:%d", &h, &m) < 1) return false;
    *mins = sg * (h * 60 + m); return true;
}

static uint32_t json_uint(const cJSON *o, const char *k) { cJSON *v = cJSON_GetObjectItem(o, k); return (v && cJSON_IsNumber(v)) ? (uint32_t)cJSON_GetNumberValue(v) : 0; }

/* ===================================================================== *
 *  Async device I/O (Phase 2): read / cluster / invoke / onoff / write / scan
 * ===================================================================== *
 * These issue a CHIP controller command (on the CHIP task) whose result arrives
 * via callbacks; the response is written from the callback. One in-flight op at a
 * time (a busy flag) since the read/invoke callbacks carry no per-op context. A
 * SystemLayer timer (also on the CHIP task, so no cross-thread race with the done
 * callback) bounds each op. write is fire-and-forget (its params are heap-passed). */
#define IO_TIMEOUT_MS 8000

static struct {
    volatile bool busy, responded;
    int  id; bool have_id;
    uint64_t node; uint16_t ep; uint32_t cl, attr, cmd;
    char args[128];
    cJSON *values;
} s_io;

static void io_finish(bool ok, cJSON *result, const char *err)
{
    if (s_io.responded) { if (result) cJSON_Delete(result); return; }
    s_io.responded = true;
    respond(s_io.id, s_io.have_id, ok, result, err);
    if (s_io.values) { cJSON_Delete(s_io.values); s_io.values = nullptr; }
    s_io.busy = false;
}
static void io_timeout_fire(chip::System::Layer *, void *) { io_finish(false, nullptr, "device I/O timeout"); }

/* returns true if the op was claimed (dispatch must NOT respond); false = busy (already responded). */
static bool io_begin(int id, bool have_id)
{
    if (s_io.busy) { respond(id, have_id, false, nullptr, "device I/O busy - one at a time"); return false; }
    s_io.busy = true; s_io.responded = false; s_io.id = id; s_io.have_id = have_id; s_io.values = nullptr;
    return true;
}

/* ---- read / cluster ---- */
static void io_read_attr_cb(uint64_t, const ConcreteDataAttributePath &path, TLVReader *data)
{
    if (!s_io.values) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ep", path.mEndpointId);
    cJSON_AddNumberToObject(o, "cluster", path.mClusterId);
    cJSON_AddNumberToObject(o, "attr", path.mAttributeId);
    char sval[256] = "";
    bool is_num = false; double num = 0;
    if (data) { TLVReader r; r.Init(*data); is_num = tlv_get_scalar(r, num); }
    if (data) { TLVReader r; r.Init(*data); tlv_to_str(r, sval, sizeof(sval)); }
    if (is_num) cJSON_AddNumberToObject(o, "value", num);
    else        cJSON_AddStringToObject(o, "value", sval);
    cJSON_AddStringToObject(o, "text", sval);
    cJSON_AddItemToArray(s_io.values, o);
}
static void io_read_done_cb(uint64_t, const ScopedMemoryBufferWithSize<AttributePathParams> &,
                            const ScopedMemoryBufferWithSize<EventPathParams> &)
{
    chip::DeviceLayer::SystemLayer().CancelTimer(io_timeout_fire, nullptr);
    cJSON *vals = s_io.values; s_io.values = nullptr;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "values", vals ? vals : cJSON_CreateArray());
    io_finish(true, r, nullptr);
}
static void io_read_work(intptr_t)
{
    s_io.values = cJSON_CreateArray();
    auto *c = chip::Platform::New<esp_matter::controller::read_command>(
        s_io.node, s_io.ep, s_io.cl, s_io.attr, esp_matter::controller::READ_ATTRIBUTE,
        io_read_attr_cb, io_read_done_cb, nullptr);
    if (!c) { io_finish(false, nullptr, "out of memory"); return; }
    chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(IO_TIMEOUT_MS), io_timeout_fire, nullptr);
    if (c->send_command() != ESP_OK) {
        chip::DeviceLayer::SystemLayer().CancelTimer(io_timeout_fire, nullptr);
        chip::Platform::Delete(c); io_finish(false, nullptr, "read send failed");
    }
}

/* ---- invoke / onoff ---- */
static void io_invoke_ok(void *, const chip::app::ConcreteCommandPath &path, const chip::app::StatusIB &status, TLVReader *data)
{
    chip::DeviceLayer::SystemLayer().CancelTimer(io_timeout_fire, nullptr);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "status", (unsigned)static_cast<uint8_t>(status.mStatus));
    cJSON_AddNumberToObject(r, "ep", path.mEndpointId);
    if (data) { char v[192] = ""; TLVReader rr; rr.Init(*data); tlv_to_str(rr, v, sizeof(v)); if (v[0]) cJSON_AddStringToObject(r, "resp", v); }
    io_finish(true, r, nullptr);
}
static void io_invoke_err(void *, CHIP_ERROR error)
{
    chip::DeviceLayer::SystemLayer().CancelTimer(io_timeout_fire, nullptr);
    char e[80]; snprintf(e, sizeof(e), "invoke failed: %" CHIP_ERROR_FORMAT, error.Format());
    io_finish(false, nullptr, e);
}
static void io_invoke_work(intptr_t)
{
    auto *c = chip::Platform::New<esp_matter::controller::cluster_command>(
        s_io.node, s_io.ep, s_io.cl, s_io.cmd, s_io.args[0] ? s_io.args : "{}",
        chip::NullOptional, io_invoke_ok, io_invoke_err);
    if (!c) { io_finish(false, nullptr, "out of memory"); return; }
    chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(IO_TIMEOUT_MS), io_timeout_fire, nullptr);
    if (c->send_command() != ESP_OK) {
        chip::DeviceLayer::SystemLayer().CancelTimer(io_timeout_fire, nullptr);
        chip::Platform::Delete(c); io_finish(false, nullptr, "invoke send failed");
    }
}

/* ---- write (fire-and-forget; params heap-passed so no shared-state race) ---- */
struct io_wr_t { uint64_t node; uint16_t ep; uint32_t cl, attr; char json[192]; };
static void io_write_work(intptr_t arg)
{
    io_wr_t *w = (io_wr_t *)arg;
    esp_matter::controller::send_write_attr_command(w->node, w->ep, w->cl, w->attr, w->json);
    free(w);
}

/* ---- BLE scan completion (called on the CHIP task by ble_scan_finish) ---- */
static void comm_blescan_done(const ble_scan_dev_t *devs, int n)
{
    if (n < 0) { io_finish(false, nullptr, "BLE scan could not start (radio busy / BLE off while operating)"); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "devices");
    for (int i = 0; i < n; ++i) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "discriminator", devs[i].discriminator);
        cJSON_AddNumberToObject(o, "vid", devs[i].vid);
        cJSON_AddNumberToObject(o, "pid", devs[i].pid);
        cJSON_AddItemToArray(arr, o);
    }
    io_finish(true, r, nullptr);
}

static void dispatch(const char *line)
{
    cJSON *req = cJSON_Parse(line);
    if (!req) { respond(0, false, false, nullptr, "malformed JSON"); return; }
    cJSON *idv = cJSON_GetObjectItem(req, "id");
    bool have_id = idv && cJSON_IsNumber(idv);
    int id = have_id ? (int)cJSON_GetNumberValue(idv) : 0;
    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(req, "cmd"));
    if (!cmd) { respond(id, have_id, false, nullptr, "missing 'cmd'"); cJSON_Delete(req); return; }

    char err[112] = {0};
    bool ok = false;
    hub_cmd_lock();

    /* ---------- queries (structured results) ---------- */
    if (!strcmp(cmd, "ping")) {
        cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "pong", true);
        respond(id, have_id, true, r, nullptr);
    }
    else if (!strcmp(cmd, "status") || !strcmp(cmd, "devices")) {
        int slot = -1; cJSON *dv = cJSON_GetObjectItem(req, "dev");
        if (dv && cJSON_IsNumber(dv)) slot = (int)cJSON_GetNumberValue(dv) - 1;
        else if (dv && cJSON_IsString(dv)) { const char *p = cJSON_GetStringValue(dv); for (int i = 0; i < MAX_DEVICES; ++i) if (g_dev[i].paired && !strcmp(g_dev[i].payload, p)) { slot = i; break; } }
        respond(id, have_id, true, status_json(slot), nullptr);
    }
    else if (!strcmp(cmd, "backup"))     respond(id, have_id, true, hub_backup_json(), nullptr);
    else if (!strcmp(cmd, "schedule") || !strcmp(cmd, "sched_list") || !strcmp(cmd, "calendar_list"))
                                         respond(id, have_id, true, schedule_to_json(), nullptr);
    else if (!strcmp(cmd, "logic_list")) respond(id, have_id, true, logic_list_json(), nullptr);
    else if (!strcmp(cmd, "bindings"))   respond(id, have_id, true, bindings_list_json(), nullptr);
    else if (!strcmp(cmd, "bindable"))   respond(id, have_id, true, bindings_bindable_json(), nullptr);
    else if (!strcmp(cmd, "heap")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "free", esp_get_free_heap_size());
        cJSON_AddNumberToObject(r, "min_ever", esp_get_minimum_free_heap_size());
        cJSON_AddNumberToObject(r, "largest", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        respond(id, have_id, true, r, nullptr);
    }
    /* ---------- schedule / calendar / tz actions ---------- */
    else if (!strcmp(cmd, "sched_add")) {
        int n = json_int(req, "n", 0); const char *day = json_str(req, "day");
        int di = day ? sched_day_index(day) : -1;
        int st = sched_parse_hhmm(json_str(req, "start") ?: "");
        int en = sched_parse_hhmm(json_str(req, "end") ?: "");
        if (di < 0)                 snprintf(err, sizeof(err), "bad day (mon..sun / holiday)");
        else if (st < 0 || en < 0)  snprintf(err, sizeof(err), "bad HH:MM start/end");
        else                        ok = sched_add_event(n - 1, di, st, en, err, sizeof(err));
        respond(id, have_id, ok, nullptr, err);
    }
    else if (!strcmp(cmd, "sched_rm")) {
        int n = json_int(req, "n", 0); const char *day = json_str(req, "day");
        int di = day ? sched_day_index(day) : -1;
        if (di < 0) snprintf(err, sizeof(err), "bad day");
        else        ok = sched_rm_event(n - 1, di, json_int(req, "idx", -1), err, sizeof(err));
        respond(id, have_id, ok, nullptr, err);
    }
    else if (!strcmp(cmd, "sched_cal")) {
        ok = sched_set_cal(json_int(req, "n", 0) - 1, cJSON_IsTrue(cJSON_GetObjectItem(req, "on")), err, sizeof(err));
        respond(id, have_id, ok, nullptr, err);
    }
    else if (!strcmp(cmd, "calendar_add")) {
        int dd, mm;
        if (sched_parse_ddmm(json_str(req, "date") ?: "", &dd, &mm)) ok = calendar_add(dd, mm, err, sizeof(err));
        else snprintf(err, sizeof(err), "bad date (DD/MM, ** = any)");
        respond(id, have_id, ok, nullptr, err);
    }
    else if (!strcmp(cmd, "calendar_rm")) {
        ok = calendar_rm(json_int(req, "idx", -1), err, sizeof(err));
        respond(id, have_id, ok, nullptr, err);
    }
    else if (!strcmp(cmd, "tz")) {
        int mins; if (parse_tz(req, &mins)) { schedule_set_tz(mins); ok = true; } else snprintf(err, sizeof(err), "bad tz offset");
        respond(id, have_id, ok, nullptr, err);
    }
    /* ---------- logic actions ---------- */
    else if (!strcmp(cmd, "logic_add")) {
        const char *expr = json_str(req, "expr");
        if (!expr) snprintf(err, sizeof(err), "need 'expr' (console logic syntax)");
        else       ok = logic_add_expr(expr, err, sizeof(err));
        respond(id, have_id, ok, nullptr, err);
    }
    else if (!strcmp(cmd, "logic_rm")) {
        ok = logic_rm_idx(json_int(req, "idx", -1), err, sizeof(err));
        respond(id, have_id, ok, nullptr, err);
    }
    /* ---------- bindings actions ---------- */
    else if (!strcmp(cmd, "bind")) {
        const char *act = json_str(req, "action");
        uint8_t a = (act && !strcmp(act, "off")) ? 0 : (act && !strcmp(act, "on")) ? 1 : 2;
        ok = bindings_add(json_int(req, "src", 0), json_int(req, "src_ep", 1), json_int(req, "dst", 0), json_int(req, "dst_ep", 0), a, json_str(req, "press"), err, sizeof(err));
        respond(id, have_id, ok, nullptr, err);
    }
    else if (!strcmp(cmd, "unbind")) {
        ok = bindings_unbind_idx(json_int(req, "idx", -1), err, sizeof(err));
        respond(id, have_id, ok, nullptr, err);
    }
    /* ---------- async device I/O (Phase 2): response is written from the callback ---------- */
    else if (!strcmp(cmd, "read") || !strcmp(cmd, "cluster")) {
        int dev = json_int(req, "dev", 0);
        if (dev < 1 || dev > MAX_DEVICES || !g_dev[dev - 1].paired) respond(id, have_id, false, nullptr, "no such device");
        else if (io_begin(id, have_id)) {
            s_io.node = g_dev[dev - 1].node_id;
            s_io.ep   = (uint16_t)json_uint(req, "ep");
            s_io.cl   = json_uint(req, "cluster");
            s_io.attr = !strcmp(cmd, "cluster") ? WILDCARD_ID : json_uint(req, "attr");
            chip::DeviceLayer::PlatformMgr().ScheduleWork(io_read_work, 0);
        }
    }
    else if (!strcmp(cmd, "invoke")) {
        int dev = json_int(req, "dev", 0);
        if (dev < 1 || dev > MAX_DEVICES || !g_dev[dev - 1].paired) respond(id, have_id, false, nullptr, "no such device");
        else if (io_begin(id, have_id)) {
            s_io.node = g_dev[dev - 1].node_id;
            s_io.ep   = (uint16_t)json_uint(req, "ep");
            s_io.cl   = json_uint(req, "cluster");
            s_io.cmd  = json_uint(req, "command");   /* NOT "cmd" - that's the top-level command name */
            const char *a = json_str(req, "args");
            s_io.args[0] = '\0'; if (a) { strncpy(s_io.args, a, sizeof(s_io.args) - 1); s_io.args[sizeof(s_io.args) - 1] = '\0'; }
            chip::DeviceLayer::PlatformMgr().ScheduleWork(io_invoke_work, 0);
        }
    }
    else if (!strcmp(cmd, "onoff")) {
        int dev = json_int(req, "dev", 0);
        const char *act = json_str(req, "action");
        if (dev < 1 || dev > MAX_DEVICES || !g_dev[dev - 1].paired) respond(id, have_id, false, nullptr, "no such device");
        else if (!act) respond(id, have_id, false, nullptr, "need 'action' (on/off/toggle)");
        else if (io_begin(id, have_id)) {
            s_io.node = g_dev[dev - 1].node_id;
            s_io.ep   = (uint16_t)json_int(req, "ep", 1);
            s_io.cl   = CL_ONOFF;
            s_io.cmd  = !strcmp(act, "off") ? CMD_ONOFF_OFF : !strcmp(act, "on") ? CMD_ONOFF_ON : CMD_ONOFF_TOGGLE;
            s_io.args[0] = '\0';
            chip::DeviceLayer::PlatformMgr().ScheduleWork(io_invoke_work, 0);
        }
    }
    else if (!strcmp(cmd, "write")) {
        int dev = json_int(req, "dev", 0);
        const char *val = json_str(req, "value");
        if (dev < 1 || dev > MAX_DEVICES || !g_dev[dev - 1].paired) respond(id, have_id, false, nullptr, "no such device");
        else if (!val) respond(id, have_id, false, nullptr, "need 'value' (esp-matter JSON, e.g. {\"0:U8\":1})");
        else {
            io_wr_t *w = (io_wr_t *)calloc(1, sizeof(io_wr_t));
            if (!w) respond(id, have_id, false, nullptr, "out of memory");
            else {
                w->node = g_dev[dev - 1].node_id; w->ep = (uint16_t)json_uint(req, "ep");
                w->cl = json_uint(req, "cluster"); w->attr = json_uint(req, "attr");
                strncpy(w->json, val, sizeof(w->json) - 1);
                chip::DeviceLayer::PlatformMgr().ScheduleWork(io_write_work, (intptr_t)w);
                cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "sent", true);
                respond(id, have_id, true, r, nullptr);
            }
        }
    }
    else if (!strcmp(cmd, "blescan")) {
        int secs = json_int(req, "secs", 5);
        if (io_begin(id, have_id))                       /* responds from comm_blescan_done */
            ble_scan_start_ex((uint16_t)secs, comm_blescan_done);
    }
    else if (!strcmp(cmd, "devtime")) {
        int dev = json_int(req, "dev", 0);
        time_t now = time(nullptr);
        if (dev < 1 || dev > MAX_DEVICES || !g_dev[dev - 1].paired) respond(id, have_id, false, nullptr, "no such device");
        else if (now < 946684800L) respond(id, have_id, false, nullptr, "hub clock not set (settime first)");
        else if (io_begin(id, have_id)) {
            long long matter_us = (long long)(now - 946684800LL) * 1000000LL;   /* since Matter epoch 2000-01-01 UTC */
            s_io.node = g_dev[dev - 1].node_id;
            s_io.ep   = 0;          /* TimeSynchronization is on the root endpoint      */
            s_io.cl   = 0x0038;     /* TimeSynchronization cluster                      */
            s_io.cmd  = 0x00;       /* SetUTCTime                                       */
            snprintf(s_io.args, sizeof(s_io.args), "{\"0:U64\":\"%lld\",\"1:U8\":2}", matter_us);  /* UTCTime + Granularity=Seconds */
            chip::DeviceLayer::PlatformMgr().ScheduleWork(io_invoke_work, 0);
        }
    }
    /* ---------- restore (reboots) ---------- */
    else if (!strcmp(cmd, "restore")) {
        cJSON *bk = cJSON_GetObjectItem(req, "backup");
        if (cJSON_IsTrue(cJSON_GetObjectItem(req, "cancel"))) {
            bool was = hub_restore_pending(); hub_restore_cancel();
            cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "cancelled", was);
            respond(id, have_id, true, r, nullptr);
        }
        else if (!bk) respond(id, have_id, false, nullptr, "need 'backup' object");
        else {
            cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "rebooting", true);
            respond(id, have_id, true, r, nullptr);              /* respond BEFORE reboot */
            hub_stage_restore(bk, cJSON_IsTrue(cJSON_GetObjectItem(req, "wipe")), err, sizeof(err));
        }
    }
    /* ---------- pairing lifecycle (reboots) ---------- */
    else if (!strcmp(cmd, "pair")) {
        const char *pl = json_str(req, "payload");
        if (!pl) respond(id, have_id, false, nullptr, "need 'payload'");
        else { cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "rebooting", true);
               respond(id, have_id, true, r, nullptr); hub_stage_pair(pl, err, sizeof(err)); }
    }
    else if (!strcmp(cmd, "pairlist")) {
        cJSON *pls = cJSON_GetObjectItem(req, "payloads");
        if (!cJSON_IsArray(pls)) respond(id, have_id, false, nullptr, "need 'payloads' array");
        else { int q = 0; cJSON *r = cJSON_CreateObject();
               cJSON_AddBoolToObject(r, "rebooting", true);
               respond(id, have_id, true, r, nullptr); hub_stage_pairlist(pls, &q, err, sizeof(err)); }
    }
    else if (!strcmp(cmd, "remove")) {
        cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "rebooting", true);
        respond(id, have_id, true, r, nullptr); hub_remove_device(json_int(req, "dev", 0), err, sizeof(err));
    }
    else if (!strcmp(cmd, "reset")) {
        cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "rebooting", true);
        respond(id, have_id, true, r, nullptr); hub_factory_reset();
    }
    /* ---------- non-rebooting globals (name/payload/pin/debug/settime) ---------- */
    else {
        cJSON *result = cJSON_CreateObject();
        ok = hub_action_json(cmd, req, result, err, sizeof(err));
        respond(id, have_id, ok, result, ok ? nullptr : (err[0] ? err : "unknown command"));
    }

    hub_cmd_unlock();
    cJSON_Delete(req);
}

/* ---- transport ---- */
/* Growable request-line buffer: normal commands stay at LINE_BASE, but a 'restore'
 * carries the whole config inline (several KB), so grow up to LINE_MAX for that one
 * line then shrink back - keeping the steady-state heap footprint small (the C6 is
 * heap-tight once several devices are subscribed). */
#define LINE_BASE  1536
#define LINE_MAX   12288
static void comm_task(void *)
{
    size_t cap = LINE_BASE;
    char *line = (char *)malloc(cap);
    if (!line) { ESP_LOGE(TAG, "comm line alloc failed"); vTaskDelete(nullptr); return; }
    int pos = 0;
    uint8_t buf[128];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; ++i) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                if (pos > 0) { line[pos] = '\0'; dispatch(line); }
                pos = 0;
                if (cap > LINE_BASE) {   /* shrink back after a big (restore) line */
                    char *nl = (char *)realloc(line, LINE_BASE);
                    if (nl) { line = nl; cap = LINE_BASE; }
                }
                continue;
            }
            if (pos >= (int)cap - 1) {
                if (cap >= LINE_MAX) { pos = 0; respond(0, false, false, nullptr, "request line too long"); continue; }
                size_t ncap = cap * 2; if (ncap > LINE_MAX) ncap = LINE_MAX;
                char *nl = (char *)realloc(line, ncap);
                if (!nl) { pos = 0; respond(0, false, false, nullptr, "out of memory for request"); continue; }
                line = nl; cap = ncap;
            }
            line[pos++] = c;
        }
    }
}

void comm_start(void)
{
    usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 2048, .rx_buffer_size = 512 };
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) { ESP_LOGE(TAG, "USB-Serial-JTAG driver install failed"); return; }
    if (xTaskCreate(comm_task, "comm", 4096, nullptr, 5, nullptr) != pdPASS)
        ESP_LOGE(TAG, "comm_task create failed (low heap)");
    else
        ESP_LOGI(TAG, "USB JSON protocol up on native USB (COM7)");
}
