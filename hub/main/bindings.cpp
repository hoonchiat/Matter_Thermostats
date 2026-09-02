/* bindings.cpp - see bindings.h.
 *
 * Structure:
 *   Part 1  Hub-mediated rule table  (event -> action relay; NVS-persisted)
 *   Part 2  Native Matter binding    (Binding table + ACL read-modify-write)
 *   Part 3  bindable classification
 *   Part 4  Console command dispatch
 */
#include "bindings.h"
#include "device_model.h"
#include "matter_names.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cinttypes>
#include "cJSON.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

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
using chip::TLV::TLVType;

static const char *TAG = "bindings";

/* Well-known ids (see also device_model.h / matter_names.h). */
#define CL_BINDING       0x001EUL
#define CL_ACCESSCONTROL 0x001FUL
#define CL_SWITCH        0x003BUL
#define A_BINDING_LIST   0x0000UL   /* Binding.Binding          (list[TargetStruct])   */
#define A_ACL_LIST       0x0000UL   /* AccessControl.ACL        (list[AclEntryStruct]) */
#define EV_SWITCH_SHORTRELEASE 0x03UL

/* ACL privilege / auth-mode enums (Matter core). */
#define ACL_PRIV_OPERATE 3
#define ACL_AUTH_CASE    2

/* Action codes for a hub rule (map to OnOff command ids). */
enum { ACT_OFF = 0, ACT_ON = 1, ACT_TOGGLE = 2 };

/* Press-type triggers, encoded INTO hub_rule_t.trigger_event so the persisted
 * struct size is unchanged (existing bindings keep working across this upgrade):
 * low byte = Matter Switch event id, high byte = MultiPressComplete count to match
 * (0 = any). A momentary button emits: single = MultiPressComplete{count:1},
 * double = {count:2}, long = LongPress. */
#define EV_SWITCH_LONGPRESS  0x02UL
#define EV_SWITCH_MULTIPRESS 0x06UL
#define TRIG_SINGLE  (EV_SWITCH_MULTIPRESS | (1u << 8))   /* 0x0106 */
#define TRIG_DOUBLE  (EV_SWITCH_MULTIPRESS | (2u << 8))   /* 0x0206 */
#define TRIG_LONG    (EV_SWITCH_LONGPRESS)                /* 0x0002 */

static const char *trig_name(uint32_t trig)
{
    uint32_t ev = trig & 0xFFu; uint8_t cnt = (uint8_t)((trig >> 8) & 0xFFu);
    if (ev == EV_SWITCH_LONGPRESS)  return "long";
    if (ev == EV_SWITCH_MULTIPRESS) return (cnt == 2) ? "double" : "single";
    return "press";   /* legacy ShortRelease = fires on any press */
}

/* ===================================================================== *
 *  Part 1 - Hub-mediated rule table
 * ===================================================================== */

#define MAX_RULES 12

struct hub_rule_t {
    uint8_t  used;
    uint64_t src_node;
    uint16_t src_ep;
    uint32_t trigger_event;   /* Switch event id that fires this rule (ShortRelease) */
    uint64_t dst_node;
    uint16_t dst_ep;
    uint8_t  action;          /* ACT_OFF / ACT_ON / ACT_TOGGLE */
};
static hub_rule_t g_rules[MAX_RULES];

#define NVS_NS   "matterhub"      /* same namespace as main.cpp                       */
#define K_BINDS  "hubbind"        /* own key: blob of hub_rule_t[MAX_RULES]           */

static void rules_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, K_BINDS, g_rules, sizeof(g_rules));
    nvs_commit(h);
    nvs_close(h);
}

void bindings_load(void)
{
    memset(g_rules, 0, sizeof(g_rules));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t stored = 0;
    /* Only accept an exactly-sized blob; a size mismatch (struct changed) is
     * ignored rather than misread - hub rules are re-creatable, unlike pairings. */
    if (nvs_get_blob(h, K_BINDS, nullptr, &stored) == ESP_OK && stored == sizeof(g_rules)) {
        size_t len = stored;
        nvs_get_blob(h, K_BINDS, g_rules, &len);
    }
    nvs_close(h);
    int n = 0;
    for (int i = 0; i < MAX_RULES; ++i) if (g_rules[i].used) ++n;
    if (n) ESP_LOGI(TAG, "Loaded %d hub-mediated binding rule(s)", n);
}

static int rules_find_free(void) { for (int i = 0; i < MAX_RULES; ++i) if (!g_rules[i].used) return i; return -1; }

/* ---- hub-rule dispatch (invoke the target's OnOff cluster) ---------------- */
static void act_invoke_ok(void *, const chip::app::ConcreteCommandPath &path,
                          const chip::app::StatusIB &status, TLVReader *)
{
    printf("[hub] binding relay -> ep%u OnOff cmd 0x%02lx status=0x%02x\r\n",
           path.mEndpointId, (unsigned long)path.mCommandId,
           (unsigned)static_cast<uint8_t>(status.mStatus));
}
static void act_invoke_err(void *, CHIP_ERROR error)
{
    printf("[hub] binding relay FAILED: %" CHIP_ERROR_FORMAT "\r\n", error.Format());
}

/* Called on the CHIP task (from the event-report callback). Safe to originate a
 * command here. */
void bindings_on_event(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id, uint32_t event_id, uint8_t count)
{
    if (cluster_id != CL_SWITCH) return;   /* only Generic Switch events drive rules */
    for (int i = 0; i < MAX_RULES; ++i) {
        hub_rule_t &r = g_rules[i];
        if (!r.used) continue;
        if (r.src_node != node_id || r.src_ep != endpoint_id) continue;
        uint32_t want_ev  = r.trigger_event & 0xFFu;
        uint8_t  want_cnt = (uint8_t)((r.trigger_event >> 8) & 0xFFu);
        if (want_ev != event_id) continue;
        if (want_cnt != 0 && want_cnt != count) continue;   /* distinguish single vs double */

        uint32_t cmd = (r.action == ACT_OFF) ? CMD_ONOFF_OFF
                     : (r.action == ACT_ON)  ? CMD_ONOFF_ON
                                             : CMD_ONOFF_TOGGLE;
        printf("[hub] binding: #%d ep%u %s -> %s plug node=0x%016" PRIx64 " ep%u\r\n",
               dev_slot_by_node(node_id) + 1, endpoint_id, trig_name(r.trigger_event),
               (r.action == ACT_OFF) ? "off" : (r.action == ACT_ON) ? "on" : "toggle",
               r.dst_node, r.dst_ep);

        auto *c = chip::Platform::New<esp_matter::controller::cluster_command>(
            r.dst_node, r.dst_ep, CL_ONOFF, cmd, "{}", chip::NullOptional,
            act_invoke_ok, act_invoke_err);
        if (!c) { ESP_LOGE(TAG, "OOM: relay cluster_command"); continue; }
        if (c->send_command() != ESP_OK) { ESP_LOGE(TAG, "relay send failed"); chip::Platform::Delete(c); }
    }
}

/* ===================================================================== *
 *  Part 2 - Native Matter binding (Binding table + ACL)
 * ===================================================================== *
 *
 * A native bind is a chained, CHIP-task state machine:
 *   read target ACL  -> (append Operate entry, preserving existing) write ACL
 *   read source Bind -> (append target entry, preserving existing)  write Bind
 *   read both back   -> report.
 * ACL decode is defensive: if any existing entry is a shape we cannot faithfully
 * re-encode, we ABORT rather than risk clobbering the admin entry.
 */

#define NB_MAX_ACL   8
#define NB_MAX_SUBJ  4
#define NB_MAX_TGT   4
#define NB_MAX_BIND  10
#define NB_JSON_CAP  1200

struct acl_target_t { bool has_cl; uint32_t cl; bool has_ep; uint16_t ep; bool has_dt; uint32_t dt; };
struct acl_entry_t {
    uint8_t  privilege, authmode;
    bool     subj_null; uint8_t n_subj; uint64_t subj[NB_MAX_SUBJ];
    bool     tgt_null;  uint8_t n_tgt;  acl_target_t tgt[NB_MAX_TGT];
};
struct bind_target_t { bool has_node; uint64_t node; bool has_group; uint16_t group;
                       bool has_ep; uint16_t ep; bool has_cl; uint32_t cl; };

/* Heap-allocated for the duration of ONE native bind/unbind (v1.6 RAM diet:
 * this ctx is ~2.3 KB and a native bind is rare - it was permanent .bss).
 * Non-null pointer == an operation is in progress (replaces the busy flag).
 * Allocated in native_bind_begin, freed in nb_finish; every async callback
 * funnels through nb_finish or guards on the pointer. */
struct native_ctx_t {
    bool     remove;               /* false=bind, true=unbind                  */
    uint64_t src_node; uint16_t src_ep;
    uint64_t dst_node; uint16_t dst_ep; uint32_t cluster;
    int      src_slot, dst_slot;

    bool     acl_ok; uint8_t n_acl; acl_entry_t acl[NB_MAX_ACL];
    bool     bind_ok; uint8_t n_bind; bind_target_t binds[NB_MAX_BIND];
    char     json[NB_JSON_CAP];
};
static native_ctx_t *s_nb = nullptr;

static void nb_finish(const char *msg)
{
    if (msg) printf("[hub] %s\r\n", msg);
    free(s_nb);
    s_nb = nullptr;
}

/* ---- defensive decoders -------------------------------------------------- */
static bool decode_acl_list(TLVReader &r, acl_entry_t *out, uint8_t &n, uint8_t maxn)
{
    n = 0;
    TLVType outer;
    if (r.EnterContainer(outer) != CHIP_NO_ERROR) return false;
    while (r.Next() == CHIP_NO_ERROR) {
        if (n >= maxn) return false;                    /* too many -> abort safe */
        acl_entry_t &e = out[n];
        memset(&e, 0, sizeof(e));
        e.subj_null = e.tgt_null = true;                /* default: not present */
        TLVType est;
        if (r.EnterContainer(est) != CHIP_NO_ERROR) return false;
        while (r.Next() == CHIP_NO_ERROR) {
            if (!chip::TLV::IsContextTag(r.GetTag())) continue;
            uint8_t t = (uint8_t)chip::TLV::TagNumFromTag(r.GetTag());
            if (t == 1) { uint64_t v = 0; if (r.Get(v) != CHIP_NO_ERROR) return false; e.privilege = (uint8_t)v; }
            else if (t == 2) { uint64_t v = 0; if (r.Get(v) != CHIP_NO_ERROR) return false; e.authmode = (uint8_t)v; }
            else if (t == 3) {                          /* Subjects: null | list[u64] */
                if (r.GetType() == chip::TLV::kTLVType_Null) { e.subj_null = true; }
                else if (r.GetType() == chip::TLV::kTLVType_Array) {
                    e.subj_null = false; TLVType sc;
                    if (r.EnterContainer(sc) != CHIP_NO_ERROR) return false;
                    while (r.Next() == CHIP_NO_ERROR) {
                        if (e.n_subj >= NB_MAX_SUBJ) return false;
                        uint64_t v = 0; if (r.Get(v) != CHIP_NO_ERROR) return false;
                        e.subj[e.n_subj++] = v;
                    }
                    r.ExitContainer(sc);
                } else return false;
            }
            else if (t == 4) {                          /* Targets: null | list[struct] */
                if (r.GetType() == chip::TLV::kTLVType_Null) { e.tgt_null = true; }
                else if (r.GetType() == chip::TLV::kTLVType_Array) {
                    e.tgt_null = false; TLVType tc;
                    if (r.EnterContainer(tc) != CHIP_NO_ERROR) return false;
                    while (r.Next() == CHIP_NO_ERROR) {
                        if (e.n_tgt >= NB_MAX_TGT) return false;
                        acl_target_t &tg = e.tgt[e.n_tgt];
                        memset(&tg, 0, sizeof(tg));
                        TLVType gc;
                        if (r.EnterContainer(gc) != CHIP_NO_ERROR) return false;
                        while (r.Next() == CHIP_NO_ERROR) {
                            if (!chip::TLV::IsContextTag(r.GetTag())) continue;
                            uint8_t gt = (uint8_t)chip::TLV::TagNumFromTag(r.GetTag());
                            if (r.GetType() == chip::TLV::kTLVType_Null) continue;
                            uint64_t v = 0; if (r.Get(v) != CHIP_NO_ERROR) return false;
                            if (gt == 1) { tg.has_cl = true; tg.cl = (uint32_t)v; }
                            else if (gt == 2) { tg.has_ep = true; tg.ep = (uint16_t)v; }
                            else if (gt == 3) { tg.has_dt = true; tg.dt = (uint32_t)v; }
                        }
                        r.ExitContainer(gc);
                        e.n_tgt++;
                    }
                    r.ExitContainer(tc);
                } else return false;
            }
            /* tag 254 (FabricIndex) and any others: ignored - server re-fills it. */
        }
        r.ExitContainer(est);
        n++;
    }
    r.ExitContainer(outer);
    return true;
}

static bool decode_bind_list(TLVReader &r, bind_target_t *out, uint8_t &n, uint8_t maxn)
{
    n = 0;
    TLVType outer;
    if (r.EnterContainer(outer) != CHIP_NO_ERROR) return false;
    while (r.Next() == CHIP_NO_ERROR) {
        if (n >= maxn) return false;
        bind_target_t &b = out[n];
        memset(&b, 0, sizeof(b));
        TLVType st;
        if (r.EnterContainer(st) != CHIP_NO_ERROR) return false;
        while (r.Next() == CHIP_NO_ERROR) {
            if (!chip::TLV::IsContextTag(r.GetTag())) continue;
            uint8_t t = (uint8_t)chip::TLV::TagNumFromTag(r.GetTag());
            uint64_t v = 0; if (r.Get(v) != CHIP_NO_ERROR) return false;
            if (t == 1) { b.has_node = true; b.node = v; }
            else if (t == 2) { b.has_group = true; b.group = (uint16_t)v; }
            else if (t == 3) { b.has_ep = true; b.ep = (uint16_t)v; }
            else if (t == 4) { b.has_cl = true; b.cl = (uint32_t)v; }
        }
        r.ExitContainer(st);
        n++;
    }
    r.ExitContainer(outer);
    return true;
}

/* ---- JSON encoders (esp-matter json_to_tlv grammar: tag:type, ARR-SUB) ---- */
static int enc_acl_entry(char *p, size_t cap, const acl_entry_t &e)
{
    int k = snprintf(p, cap, "{\"1:U8\":%u,\"2:U8\":%u", e.privilege, e.authmode);
    if (e.subj_null) k += snprintf(p + k, cap - k, ",\"3:NULL\":null");
    else {
        k += snprintf(p + k, cap - k, ",\"3:ARR-U64\":[");
        for (int i = 0; i < e.n_subj; ++i)
            k += snprintf(p + k, cap - k, "%s\"%" PRIu64 "\"", i ? "," : "", e.subj[i]);
        k += snprintf(p + k, cap - k, "]");
    }
    if (e.tgt_null) k += snprintf(p + k, cap - k, ",\"4:NULL\":null");
    else {
        k += snprintf(p + k, cap - k, ",\"4:ARR-OBJ\":[");
        for (int i = 0; i < e.n_tgt; ++i) {
            const acl_target_t &t = e.tgt[i];
            k += snprintf(p + k, cap - k, "%s{", i ? "," : "");
            bool first = true;
            if (t.has_cl) { k += snprintf(p + k, cap - k, "\"1:U32\":%lu", (unsigned long)t.cl); first = false; }
            if (t.has_ep) { k += snprintf(p + k, cap - k, "%s\"2:U16\":%u", first ? "" : ",", t.ep); first = false; }
            if (t.has_dt) { k += snprintf(p + k, cap - k, "%s\"3:U32\":%lu", first ? "" : ",", (unsigned long)t.dt); }
            k += snprintf(p + k, cap - k, "}");
        }
        k += snprintf(p + k, cap - k, "]");
    }
    k += snprintf(p + k, cap - k, "}");
    return k;
}

static bool build_acl_json(void)
{
    char *p = s_nb->json; size_t cap = NB_JSON_CAP;
    int k = snprintf(p, cap, "{\"0:ARR-OBJ\":[");
    for (int i = 0; i < s_nb->n_acl; ++i) {
        if ((size_t)k > cap - 200) return false;
        k += snprintf(p + k, cap - k, "%s", i ? "," : "");
        k += enc_acl_entry(p + k, cap - k, s_nb->acl[i]);
    }
    if ((size_t)k > cap - 200) return false;
    k += snprintf(p + k, cap - k, "]}");
    return (size_t)k < cap;
}

static bool build_bind_json(void)
{
    char *p = s_nb->json; size_t cap = NB_JSON_CAP;
    int k = snprintf(p, cap, "{\"0:ARR-OBJ\":[");
    for (int i = 0; i < s_nb->n_bind; ++i) {
        const bind_target_t &b = s_nb->binds[i];
        if ((size_t)k > cap - 120) return false;
        k += snprintf(p + k, cap - k, "%s{", i ? "," : "");
        bool first = true;
        if (b.has_node) { k += snprintf(p + k, cap - k, "\"1:U64\":\"%" PRIu64 "\"", b.node); first = false; }
        if (b.has_group){ k += snprintf(p + k, cap - k, "%s\"2:U16\":%u", first ? "" : ",", b.group); first = false; }
        if (b.has_ep)   { k += snprintf(p + k, cap - k, "%s\"3:U16\":%u", first ? "" : ",", b.ep); first = false; }
        if (b.has_cl)   { k += snprintf(p + k, cap - k, "%s\"4:U32\":%lu", first ? "" : ",", (unsigned long)b.cl); }
        k += snprintf(p + k, cap - k, "}");
    }
    k += snprintf(p + k, cap - k, "]}");
    return (size_t)k < cap;
}

/* ---- state machine phases ------------------------------------------------ */
static void nb_read_binding(void);   /* fwd */

static bool acl_has_subject(const acl_entry_t &e, uint64_t node)
{
    if (e.subj_null) return false;
    for (int i = 0; i < e.n_subj; ++i) if (e.subj[i] == node) return true;
    return false;
}

/* Write the (already built) source Binding table, then confirm. */
static void nb_write_binding(void)
{
    if (!build_bind_json()) { nb_finish("bind: Binding JSON too large - aborted"); return; }
    printf("[hub] writing Binding table -> src node=0x%016" PRIx64 " ep%u (%d entr%s)\r\n",
           s_nb->src_node, s_nb->src_ep, s_nb->n_bind, s_nb->n_bind == 1 ? "y" : "ies");
    esp_err_t e = esp_matter::controller::send_write_attr_command(
        s_nb->src_node, s_nb->src_ep, CL_BINDING, A_BINDING_LIST, s_nb->json);
    nb_finish(e == ESP_OK
        ? "native bind sent (ACL + Binding). Watch log for the devices' write status; "
          "use 'bindings <src#>' to read the table back."
        : "bind: Binding write FAILED to send");
}

static void bind_read_cb(uint64_t, const ConcreteDataAttributePath &path, TLVReader *data)
{
    if (!s_nb) return;
    if (path.mClusterId != CL_BINDING || path.mAttributeId != A_BINDING_LIST || !data) return;
    TLVReader r; r.Init(*data);
    s_nb->bind_ok = decode_bind_list(r, s_nb->binds, s_nb->n_bind, NB_MAX_BIND);
}

static void bind_read_done(uint64_t, const ScopedMemoryBufferWithSize<AttributePathParams> &,
                           const ScopedMemoryBufferWithSize<EventPathParams> &)
{
    if (!s_nb) return;
    if (!s_nb->bind_ok) { nb_finish("bind: could not read source Binding table (too complex/unreachable) - aborted"); return; }

    /* Locate an existing entry to the same target (node+ep+cluster). */
    int found = -1;
    for (int i = 0; i < s_nb->n_bind; ++i) {
        bind_target_t &b = s_nb->binds[i];
        if (b.has_node && b.node == s_nb->dst_node && b.has_ep && b.ep == s_nb->dst_ep &&
            b.has_cl && b.cl == s_nb->cluster) { found = i; break; }
    }
    if (s_nb->remove) {
        if (found < 0) { nb_finish("unbind: no matching Binding entry on source (nothing to do)"); return; }
        for (int i = found; i < s_nb->n_bind - 1; ++i) s_nb->binds[i] = s_nb->binds[i + 1];
        s_nb->n_bind--;
    } else {
        if (found >= 0) { nb_finish("bind: source already bound to this target (Binding unchanged); ACL ensured"); return; }
        if (s_nb->n_bind >= NB_MAX_BIND) { nb_finish("bind: source Binding table full - aborted"); return; }
        bind_target_t &b = s_nb->binds[s_nb->n_bind++];
        memset(&b, 0, sizeof(b));
        b.has_node = true; b.node = s_nb->dst_node;
        b.has_ep = true;   b.ep = s_nb->dst_ep;
        b.has_cl = true;   b.cl = s_nb->cluster;
    }
    nb_write_binding();
}

static void nb_read_binding(void)
{
    s_nb->bind_ok = false; s_nb->n_bind = 0;
    auto *cmd = chip::Platform::New<esp_matter::controller::read_command>(
        s_nb->src_node, s_nb->src_ep, (uint32_t)CL_BINDING, (uint32_t)A_BINDING_LIST,
        esp_matter::controller::READ_ATTRIBUTE, bind_read_cb, bind_read_done, nullptr);
    if (!cmd) { nb_finish("bind: OOM (Binding read)"); return; }
    if (cmd->send_command() != ESP_OK) { chip::Platform::Delete(cmd); nb_finish("bind: Binding read failed to send"); }
}

/* ACL written (fire-and-forget status in log); move on to the Binding table. */
static void acl_read_cb(uint64_t, const ConcreteDataAttributePath &path, TLVReader *data)
{
    if (!s_nb) return;
    if (path.mClusterId != CL_ACCESSCONTROL || path.mAttributeId != A_ACL_LIST || !data) return;
    TLVReader r; r.Init(*data);
    s_nb->acl_ok = decode_acl_list(r, s_nb->acl, s_nb->n_acl, NB_MAX_ACL);
}

static void acl_read_done(uint64_t, const ScopedMemoryBufferWithSize<AttributePathParams> &,
                          const ScopedMemoryBufferWithSize<EventPathParams> &)
{
    if (!s_nb) return;
    if (!s_nb->acl_ok) {
        nb_finish("bind: target ACL is a shape I can't safely round-trip - ABORTED (ACL not touched). "
                  "Add an Operate ACL entry for the source manually, or use a hub-mediated bind.");
        return;
    }

    /* Find our Operate entry (subject == src node) if present. */
    int found = -1;
    for (int i = 0; i < s_nb->n_acl; ++i)
        if (s_nb->acl[i].privilege == ACL_PRIV_OPERATE && acl_has_subject(s_nb->acl[i], s_nb->src_node)) { found = i; break; }

    if (s_nb->remove) {
        if (found >= 0) {
            for (int i = found; i < s_nb->n_acl - 1; ++i) s_nb->acl[i] = s_nb->acl[i + 1];
            s_nb->n_acl--;
        }
        /* If not found, leave ACL as-is; still proceed to remove the Binding entry. */
    } else if (found < 0) {
        if (s_nb->n_acl >= NB_MAX_ACL) { nb_finish("bind: target ACL full - aborted"); return; }
        acl_entry_t &e = s_nb->acl[s_nb->n_acl++];
        memset(&e, 0, sizeof(e));
        e.privilege = ACL_PRIV_OPERATE; e.authmode = ACL_AUTH_CASE;
        e.subj_null = false; e.n_subj = 1; e.subj[0] = s_nb->src_node;
        /* Least privilege: restrict to the bound cluster on the target endpoint. */
        e.tgt_null = false; e.n_tgt = 1;
        e.tgt[0] = { true, s_nb->cluster, true, s_nb->dst_ep, false, 0 };
    } else {
        /* Already granted - skip ACL write, just ensure the Binding entry. */
        nb_read_binding();
        return;
    }

    if (!build_acl_json()) { nb_finish("bind: ACL JSON too large - aborted (ACL not written)"); return; }
    printf("[hub] writing ACL -> target node=0x%016" PRIx64 " (%d entr%s, admin preserved)\r\n",
           s_nb->dst_node, s_nb->n_acl, s_nb->n_acl == 1 ? "y" : "ies");
    esp_err_t e = esp_matter::controller::send_write_attr_command(
        s_nb->dst_node, 0 /*root ep*/, CL_ACCESSCONTROL, A_ACL_LIST, s_nb->json);
    if (e != ESP_OK) { nb_finish("bind: ACL write FAILED to send - aborted before Binding"); return; }
    /* Proceed to the source Binding table. */
    nb_read_binding();
}

static void nb_start_work(intptr_t)
{
    if (!s_nb) return;
    s_nb->acl_ok = false; s_nb->n_acl = 0;
    auto *cmd = chip::Platform::New<esp_matter::controller::read_command>(
        s_nb->dst_node, (uint16_t)0, (uint32_t)CL_ACCESSCONTROL, (uint32_t)A_ACL_LIST,
        esp_matter::controller::READ_ATTRIBUTE, acl_read_cb, acl_read_done, nullptr);
    if (!cmd) { nb_finish("bind: OOM (ACL read)"); return; }
    if (cmd->send_command() != ESP_OK) { chip::Platform::Delete(cmd); nb_finish("bind: ACL read failed to send"); }
}

/* Kick off a native bind/unbind (called from console task). */
static void native_bind_begin(bool remove, int src_slot, uint16_t src_ep,
                              int dst_slot, uint16_t dst_ep, uint32_t cluster)
{
    if (s_nb) { printf("[hub] a native bind is already in progress - try again in a moment\r\n"); return; }
    s_nb = (native_ctx_t *)calloc(1, sizeof(native_ctx_t));
    if (!s_nb) { printf("[hub] bind: out of memory for the bind context\r\n"); return; }
    s_nb->remove = remove;
    s_nb->src_slot = src_slot; s_nb->dst_slot = dst_slot;
    s_nb->src_node = g_dev[src_slot].node_id; s_nb->src_ep = src_ep;
    s_nb->dst_node = g_dev[dst_slot].node_id; s_nb->dst_ep = dst_ep;
    s_nb->cluster  = cluster;
    printf("[hub] native %s: #%d ep%u  <->  #%d ep%u  cluster 0x%04lx ...\r\n",
           remove ? "unbind" : "bind", src_slot + 1, src_ep, dst_slot + 1, dst_ep, (unsigned long)cluster);
    chip::DeviceLayer::PlatformMgr().ScheduleWork(nb_start_work, 0);
}

/* Live-read + print a device's native Binding table (for `bindings <n>`). */
static void bl_read_cb(uint64_t node, const ConcreteDataAttributePath &path, TLVReader *data)
{
    if (path.mClusterId != CL_BINDING || !data) return;
    bind_target_t bs[NB_MAX_BIND]; uint8_t n = 0;
    TLVReader r; r.Init(*data);
    if (!decode_bind_list(r, bs, n, NB_MAX_BIND)) { printf("  (Binding table too complex to decode)\r\n"); return; }
    int slot = dev_slot_by_node(node);
    printf("[hub] #%d ep%u native Binding table: %u entr%s\r\n", slot + 1, path.mEndpointId, n, n == 1 ? "y" : "ies");
    for (int i = 0; i < n; ++i) {
        bind_target_t &b = bs[i];
        char clb[40];
        printf("    -> node=0x%016" PRIx64 " ep%u %s\r\n",
               b.has_node ? b.node : 0, b.has_ep ? b.ep : 0,
               b.has_cl ? cluster_label(b.cl, clb, sizeof(clb)) : "(any)");
    }
}
static void bl_read_done(uint64_t, const ScopedMemoryBufferWithSize<AttributePathParams> &,
                         const ScopedMemoryBufferWithSize<EventPathParams> &) {}

static struct { uint64_t node; uint16_t ep; } s_bl;
static void bl_work(intptr_t)
{
    auto *cmd = chip::Platform::New<esp_matter::controller::read_command>(
        s_bl.node, s_bl.ep, (uint32_t)CL_BINDING, (uint32_t)A_BINDING_LIST,
        esp_matter::controller::READ_ATTRIBUTE, bl_read_cb, bl_read_done, nullptr);
    if (!cmd) { ESP_LOGE(TAG, "OOM: binding read"); return; }
    if (cmd->send_command() != ESP_OK) { chip::Platform::Delete(cmd); ESP_LOGE(TAG, "binding read failed"); }
}

/* ===================================================================== *
 *  Part 3 - classification helpers
 * ===================================================================== */
static bool ep_has_server(DeviceInfo &d, uint16_t ep, uint32_t cl)
{
    EndpointInfo *e = dev_find_ep(d, ep);
    return e && ep_find_cluster(*e, cl);
}
/* First endpoint on d that has server cluster cl, or -1. */
static int ep_with_server(DeviceInfo &d, uint32_t cl)
{
    for (int i = 0; i < d.n_eps; ++i)
        if (d.eps[i] && ep_find_cluster(*d.eps[i], cl)) return (int)d.eps[i]->id;
    return -1;
}

/* ===================================================================== *
 *  Part 4 - console dispatch
 * ===================================================================== */
static uint32_t parse_id(const char *s) { return (uint32_t)strtoul(s, nullptr, 0); }

static DeviceInfo *slot_arg(const char *s, int *slot_out)
{
    int n = atoi(s);
    if (n < 1 || n > MAX_DEVICES || !g_dev[n - 1].paired) {
        printf("[hub] no paired device #%s (see 'devices')\r\n", s);
        return nullptr;
    }
    if (slot_out) *slot_out = n - 1;
    return &g_dev[n - 1];
}

void bindings_print_help(void)
{
    printf("  bindable <n>              show how device #n can bind (source/target roles)\r\n"
           "  bind <s> <sep> <d> [on|off|toggle] [single|double|long]   src ep -> dest (auto native/hub;\r\n"
           "                            press-type applies to Switch/button sources, default single)\r\n"
           "  bindings [n]             list hub rules (and #n's native table if given)\r\n"
           "  unbind <idx>             remove hub rule #idx (see 'bindings')\r\n"
           "  unbind <s> <sep> <d>     remove a native binding\r\n");
}

static void print_bindable(DeviceInfo &d, int slot)
{
    char nm[72];
    printf("[hub] #%d \"%s\" binding roles:\r\n", slot + 1, dev_display_name(d, nm, sizeof(nm)));
    if (!d.enumerated) { printf("  (not enumerated yet - 'scan %d')\r\n", slot + 1); return; }
    bool any = false;
    for (int i = 0; i < d.n_eps; ++i) {
        EndpointInfo &e = *d.eps[i];
        bool nativeSrc = ep_find_cluster(e, CL_BINDING);
        bool eventSrc  = ep_find_cluster(e, CL_SWITCH);
        bool onoffTgt  = ep_find_cluster(e, CL_ONOFF);
        if (!nativeSrc && !eventSrc && !onoffTgt) continue;
        any = true;
        printf("  ep%u:", e.id);
        if (nativeSrc) printf("  SOURCE(native binding)");
        else if (eventSrc) printf("  SOURCE(hub-mediated; Generic Switch)");
        if (onoffTgt) printf("  TARGET(OnOff)");
        printf("\r\n");
    }
    if (!any) printf("  (no bindable endpoints - needs Binding/Switch source or OnOff target)\r\n");
}

static void print_hub_rules(void)
{
    int n = 0;
    printf("[hub] Hub-mediated binding rules:\r\n");
    for (int i = 0; i < MAX_RULES; ++i) {
        hub_rule_t &r = g_rules[i];
        if (!r.used) continue;
        n++;
        int ss = dev_slot_by_node(r.src_node), ds = dev_slot_by_node(r.dst_node);
        printf("  [%d] #%d ep%u %s -> %s  #%d ep%u (OnOff)\r\n", i,
               ss + 1, r.src_ep, trig_name(r.trigger_event),
               (r.action == ACT_OFF) ? "off" : (r.action == ACT_ON) ? "on" : "toggle",
               ds + 1, r.dst_ep);
    }
    if (!n) printf("  (none)\r\n");
}

bool bindings_handle_cmd(const char *line)
{
    char buf[192];
    strncpy(buf, line, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
    char *argv[10]; int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 10; t = strtok(nullptr, " ")) argv[argc++] = t;
    if (argc == 0) return false;
    const char *cmd = argv[0];

    if (!strcmp(cmd, "bindable") && argc >= 2) {
        int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
        if (d) print_bindable(*d, slot);
        return true;
    }

    if (!strcmp(cmd, "bindings")) {
        print_hub_rules();
        if (argc >= 2) {                       /* also live-read a device's native table */
            int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
            if (d) {
                int bep = ep_with_server(*d, CL_BINDING);
                if (bep < 0) printf("[hub] #%d has no native Binding cluster (hub-mediated only)\r\n", slot + 1);
                else { s_bl.node = d->node_id; s_bl.ep = (uint16_t)bep;
                       chip::DeviceLayer::PlatformMgr().ScheduleWork(bl_work, 0); }
            }
        }
        return true;
    }

    /* bind <src> <srcEp> <dst> [dstEp] [on|off|toggle] */
    if (!strcmp(cmd, "bind") && argc >= 4) {
        int ss, ds; DeviceInfo *sd = slot_arg(argv[1], &ss); if (!sd) return true;
        uint16_t sep = (uint16_t)parse_id(argv[2]);
        DeviceInfo *dd = slot_arg(argv[3], &ds); if (!dd) return true;

        /* optional action word (on|off|toggle); optional press-type
         * (single|double|long, default single); optional explicit dst ep (numeric) */
        uint8_t action = ACT_TOGGLE; int dep = -1; uint32_t trig = TRIG_SINGLE;
        for (int i = 4; i < argc; ++i) {
            if (!strcmp(argv[i], "on")) action = ACT_ON;
            else if (!strcmp(argv[i], "off")) action = ACT_OFF;
            else if (!strcmp(argv[i], "toggle")) action = ACT_TOGGLE;
            else if (!strcmp(argv[i], "single")) trig = TRIG_SINGLE;
            else if (!strcmp(argv[i], "double")) trig = TRIG_DOUBLE;
            else if (!strcmp(argv[i], "long"))   trig = TRIG_LONG;
            else dep = (int)parse_id(argv[i]);          /* numeric = explicit dst endpoint */
        }
        if (dep < 0) dep = ep_with_server(*dd, CL_ONOFF);
        if (dep < 0) { printf("[hub] target #%d has no OnOff endpoint (try 'bindable %d')\r\n", ds + 1, ds + 1); return true; }
        if (!ep_has_server(*dd, (uint16_t)dep, CL_ONOFF)) {
            printf("[hub] target #%d ep%d has no OnOff cluster\r\n", ds + 1, dep); return true; }

        if (!sd->enumerated) { printf("[hub] source #%d not enumerated yet - 'scan %d'\r\n", ss + 1, ss + 1); return true; }

        if (ep_has_server(*sd, sep, CL_BINDING)) {
            /* Native binding: the source can command the target directly. */
            native_bind_begin(false, ss, sep, ds, (uint16_t)dep, CL_ONOFF);
        } else if (ep_has_server(*sd, sep, CL_SWITCH)) {
            /* Hub-mediated: store a rule; hub relays button presses. */
            int idx = rules_find_free();
            if (idx < 0) { printf("[hub] rule table full (%d) - 'unbind <idx>' first\r\n", MAX_RULES); return true; }
            /* Replace an identical existing mapping rather than duplicate. */
            for (int i = 0; i < MAX_RULES; ++i)
                if (g_rules[i].used && g_rules[i].src_node == sd->node_id && g_rules[i].src_ep == sep &&
                    g_rules[i].dst_node == dd->node_id) { idx = i; break; }
            hub_rule_t &r = g_rules[idx];
            r.used = 1; r.src_node = sd->node_id; r.src_ep = sep;
            r.trigger_event = trig;
            r.dst_node = dd->node_id; r.dst_ep = (uint16_t)dep; r.action = action;
            rules_save();
            printf("[hub] hub-mediated bind [%d]: #%d ep%u %s-press -> %s #%d ep%d.\r\n"
                   "      Do that press to test (the hub relays it; watch the target's state).\r\n",
                   idx, ss + 1, sep, trig_name(trig), (action == ACT_OFF) ? "off" : (action == ACT_ON) ? "on" : "toggle",
                   ds + 1, dep);
        } else {
            printf("[hub] source #%d ep%u is neither a Binding (native) nor a Switch (hub) source. "
                   "'bindable %d' to see roles.\r\n", ss + 1, sep, ss + 1);
        }
        return true;
    }

    if (!strcmp(cmd, "unbind") && argc >= 2) {
        /* unbind <idx> (hub rule) OR unbind <src> <srcEp> <dst> (native) */
        if (argc == 2) {
            int idx = atoi(argv[1]);
            if (idx < 0 || idx >= MAX_RULES || !g_rules[idx].used) {
                printf("[hub] no hub rule [%s] (see 'bindings')\r\n", argv[1]); return true; }
            g_rules[idx].used = 0; rules_save();
            printf("[hub] removed hub rule [%d]\r\n", idx);
            return true;
        }
        if (argc >= 4) {
            int ss, ds; DeviceInfo *sd = slot_arg(argv[1], &ss); if (!sd) return true;
            uint16_t sep = (uint16_t)parse_id(argv[2]);
            DeviceInfo *dd = slot_arg(argv[3], &ds); if (!dd) return true;
            int dep = ep_with_server(*dd, CL_ONOFF);
            if (dep < 0) { printf("[hub] target #%d has no OnOff endpoint\r\n", ds + 1); return true; }
            native_bind_begin(true, ss, sep, ds, (uint16_t)dep, CL_ONOFF);
            return true;
        }
        printf("[hub] Usage: unbind <idx>   OR   unbind <src> <srcEp> <dst>\r\n");
        return true;
    }

    return false;
}

/* --- structured entry points (shared by the console + the JSON protocol) ---- */
bool bindings_add(int src, int src_ep, int dst, int dst_ep, uint8_t action, const char *press, char *err, size_t cap)
{
    uint32_t trig = TRIG_SINGLE;   /* press-type for Switch/button sources; default single */
    if (press) { if (!strcmp(press, "double")) trig = TRIG_DOUBLE; else if (!strcmp(press, "long")) trig = TRIG_LONG; }
    if (src < 1 || src > MAX_DEVICES || !g_dev[src-1].paired) { if (err) snprintf(err, cap, "no source device #%d", src); return false; }
    if (dst < 1 || dst > MAX_DEVICES || !g_dev[dst-1].paired) { if (err) snprintf(err, cap, "no target device #%d", dst); return false; }
    DeviceInfo *sd = &g_dev[src-1], *dd = &g_dev[dst-1];
    int ss = src-1, ds = dst-1;
    int dep = (dst_ep > 0) ? dst_ep : ep_with_server(*dd, CL_ONOFF);
    if (dep < 0) { if (err) snprintf(err, cap, "target #%d has no OnOff endpoint", dst); return false; }
    if (!ep_has_server(*dd, (uint16_t)dep, CL_ONOFF)) { if (err) snprintf(err, cap, "target #%d ep%d has no OnOff cluster", dst, dep); return false; }
    if (!sd->enumerated) { if (err) snprintf(err, cap, "source #%d not enumerated yet (scan it first)", src); return false; }

    if (ep_has_server(*sd, (uint16_t)src_ep, CL_BINDING)) {
        native_bind_begin(false, ss, (uint16_t)src_ep, ds, (uint16_t)dep, CL_ONOFF);
        return true;                                    /* native: async, sent */
    }
    if (ep_has_server(*sd, (uint16_t)src_ep, CL_SWITCH)) {
        int idx = rules_find_free();
        if (idx < 0) { if (err) snprintf(err, cap, "hub rule table full (%d)", MAX_RULES); return false; }
        for (int i = 0; i < MAX_RULES; ++i)             /* replace an identical mapping */
            if (g_rules[i].used && g_rules[i].src_node == sd->node_id && g_rules[i].src_ep == src_ep &&
                g_rules[i].dst_node == dd->node_id) { idx = i; break; }
        hub_rule_t &r = g_rules[idx];
        r.used = 1; r.src_node = sd->node_id; r.src_ep = (uint16_t)src_ep;
        r.trigger_event = trig;
        r.dst_node = dd->node_id; r.dst_ep = (uint16_t)dep; r.action = action;
        rules_save();
        return true;
    }
    if (err) snprintf(err, cap, "source #%d ep%d is neither a Binding nor a Switch source", src, src_ep);
    return false;
}

bool bindings_unbind_idx(int idx, char *err, size_t cap)
{
    if (idx < 0 || idx >= MAX_RULES || !g_rules[idx].used) { if (err) snprintf(err, cap, "no hub rule [%d]", idx); return false; }
    g_rules[idx].used = 0; rules_save();
    return true;
}

cJSON *bindings_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return nullptr;
    for (int i = 0; i < MAX_RULES; ++i) {
        hub_rule_t &r = g_rules[i];
        if (!r.used) continue;
        int ss = dev_slot_by_node(r.src_node), ds = dev_slot_by_node(r.dst_node);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "idx", i);
        cJSON_AddNumberToObject(o, "src", ss + 1);
        cJSON_AddNumberToObject(o, "src_ep", r.src_ep);
        cJSON_AddStringToObject(o, "press", trig_name(r.trigger_event));   /* single/double/long/press */
        cJSON_AddNumberToObject(o, "dst", ds + 1);
        cJSON_AddNumberToObject(o, "dst_ep", r.dst_ep);
        cJSON_AddStringToObject(o, "action", r.action == ACT_OFF ? "off" : r.action == ACT_ON ? "on" : "toggle");
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* Per-device bindable endpoints (mirrors print_bindable) so a UI can offer named
 * source/target endpoints instead of raw numbers. */
cJSON *bindings_bindable_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return nullptr;
    char nm[72];
    for (int s = 0; s < MAX_DEVICES; ++s) {
        DeviceInfo &d = g_dev[s];
        if (!d.paired) continue;
        cJSON *eps = cJSON_CreateArray();
        for (int i = 0; i < d.n_eps; ++i) {
            if (!d.eps[i]) continue;
            EndpointInfo &e = *d.eps[i];
            bool nativeSrc = ep_find_cluster(e, CL_BINDING);
            bool eventSrc  = ep_find_cluster(e, CL_SWITCH);
            bool onoffTgt  = ep_find_cluster(e, CL_ONOFF);
            if (!nativeSrc && !eventSrc && !onoffTgt) continue;
            cJSON *eo = cJSON_CreateObject();
            cJSON_AddNumberToObject(eo, "ep", e.id);
            if (nativeSrc)     cJSON_AddStringToObject(eo, "src", "native");
            else if (eventSrc) cJSON_AddStringToObject(eo, "src", "switch");
            if (onoffTgt)      cJSON_AddBoolToObject(eo, "target", true);
            cJSON_AddItemToArray(eps, eo);
        }
        if (cJSON_GetArraySize(eps) == 0) { cJSON_Delete(eps); continue; }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", s + 1);
        cJSON_AddStringToObject(o, "name", dev_display_name(d, nm, sizeof(nm)));
        cJSON_AddBoolToObject(o, "enumerated", d.enumerated);
        cJSON_AddItemToObject(o, "eps", eps);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* ===================================================================== *
 *  Part 5 - config backup / restore (JSON, device-index keyed)
 * ===================================================================== */
cJSON *bindings_to_json(const int *slot2idx)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return nullptr;
    for (int i = 0; i < MAX_RULES; ++i) {
        hub_rule_t &r = g_rules[i];
        if (!r.used) continue;
        int ss = dev_slot_by_node(r.src_node), ds = dev_slot_by_node(r.dst_node);
        if (ss < 0 || ds < 0 || slot2idx[ss] < 0 || slot2idx[ds] < 0) continue;  /* stale ref */
        cJSON *o = cJSON_CreateObject();
        /* backup/restore is device-index keyed (src_dev/dst_dev) and carries the raw
         * trigger_event, which already encodes the press-type - restore reads it back. */
        cJSON_AddNumberToObject(o, "src_dev", slot2idx[ss]);
        cJSON_AddNumberToObject(o, "src_ep",  r.src_ep);
        cJSON_AddNumberToObject(o, "event",   r.trigger_event);
        cJSON_AddNumberToObject(o, "dst_dev", slot2idx[ds]);
        cJSON_AddNumberToObject(o, "dst_ep",  r.dst_ep);
        cJSON_AddStringToObject(o, "action",  r.action == ACT_OFF ? "off" : r.action == ACT_ON ? "on" : "toggle");
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static int bind_resolve(const restore_map *m, const cJSON *num, int *miss)
{
    int di = (int)cJSON_GetNumberValue(num);
    if (di < 0 || di >= m->n_dev || m->idx2slot[di] < 0) { *miss = di; return -1; }
    return m->idx2slot[di];
}

void bindings_apply_json(const cJSON *arr, const restore_map *m, int *applied, int *skipped)
{
    int ap = 0, sk = 0, out = 0;
    memset(g_rules, 0, sizeof(g_rules));
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (out >= MAX_RULES) { ++sk; continue; }
        int miss = -1;
        int ss = bind_resolve(m, cJSON_GetObjectItem(o, "src_dev"), &miss);
        int ds = bind_resolve(m, cJSON_GetObjectItem(o, "dst_dev"), &miss);
        if (ss < 0 || ds < 0) {
            ++sk;
            printf("  SKIPPED binding rule: references device \"%s\" (not paired)\r\n",
                   (miss >= 0 && miss < m->n_dev && m->idx_name[miss]) ? m->idx_name[miss] : "?");
            continue;
        }
        const char *act = cJSON_GetStringValue(cJSON_GetObjectItem(o, "action"));
        hub_rule_t &r = g_rules[out++];
        r.used = 1;
        r.src_node = slot_node_id(ss);
        r.src_ep = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "src_ep"));
        r.trigger_event = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "event"));
        r.dst_node = slot_node_id(ds);
        r.dst_ep = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "dst_ep"));
        r.action = (act && !strcmp(act, "off")) ? ACT_OFF : (act && !strcmp(act, "toggle")) ? ACT_TOGGLE : ACT_ON;
        ++ap;
    }
    rules_save();
    if (applied) *applied = ap;
    if (skipped) *skipped = sk;
}
