/*
 * Matter Hub V1.0 - ESP32-C6 Matter controller + Thread Border Router.
 *
 * A GENERALIZED Matter hub distilled from the ALPSTUGA project (see
 * FOUNDATION.md). It commissions Matter-over-Thread end devices, runs as the
 * Thread BR/Leader, keeps a CASE session per device alive, and - additively -
 * discovers each device's full structure (VendorID/ProductID/endpoints/
 * clusters/attributes) for the console inspector (see inspector.cpp). No
 * product-specific decoding: the device model is discovered at runtime.
 *
 * CARDINAL RULE (FOUNDATION §11): the commissioning path is sacred. The
 * inspector is strictly additive and never touches it.
 *
 * Console (115200 baud):
 *   payload <MT:...|code>   set onboarding payload for the NEXT device
 *   pin <PIN> <disc>        set manual credentials (disc 0-4095)
 *   pair                    add a device (reboots into commissioning)
 *   remove <n>              decommission device slot <n> (unpair + reboots)
 *   reset                   factory reset (erase NVS) + restart
 *   status                  show current state
 *   settime YYYY-MM-DD HH:MM:SS   set hub UTC clock (CASE cert validity)
 *   debug on|off            verbose log toggle
 *   ...plus inspector commands: devices, tree, ep, cluster, read, write, invoke
 *
 * Radio (FOUNDATION §4): one 2.4 GHz radio. Commission in an isolated session
 * (BLE on, no subscriptions); operate with BLE off. Mode switch = reboot.
 * Time comes from the host (settime) - no Wi-Fi (avoids Wi-Fi/Thread conflict).
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cinttypes>
#include <ctime>
#include <optional>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_netif.h"

/* Matter controller */
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_subscribe_command.h>
#include <esp_matter_controller_read_command.h>
#include <platform/CHIPDeviceLayer.h>
#include <app/InteractionModelEngine.h>            /* ShutdownSubscriptions (decommission) */
#include <lib/support/logging/TextOnlyLogging.h>   /* chip::Logging::SetLogFilter */
#include <platform/ESP32/OpenthreadLauncher.h>
#include <lib/dnssd/Advertiser.h>                  /* ServiceAdvertiser - mDNS restart */
#include <lib/core/DataModelTypes.h>               /* chip::FabricIndex */
#include <inet/IPAddress.h>                        /* chip::Inet::IPAddress */

#include "device_model.h"
#include "inspector.h"
#include "status_led.h"
#include "ble_scan.h"
#include "matter_names.h"   /* cluster_label / event_label for event reports */
#include "tlv_decode.h"     /* generic TLV -> text for event payloads        */
#include "bindings.h"       /* device-to-device binding (native + hub-mediated) */
#include "logic.h"          /* value-triggered automation rules (v1.5)          */
#include "schedule.h"       /* weekly/holiday schedulers + calendar (v1.8)      */
#include "comm.h"           /* USB-Serial-JTAG JSON request/response protocol   */
#include "freertos/semphr.h"
#include "dash.h"           /* live one-shot device overview (v1.5)             */
#include "cJSON.h"          /* config backup/restore JSON                       */

/* Forward decl (defined in Resolver_ImplMinimalMdns.cpp): register a known
 * Thread MLEID so ResolveNodeId can bypass mDNS multicast for Sleepy End
 * Devices that don't receive ff02::fb until they data-poll (FOUNDATION §5). */
namespace chip { namespace Dnssd {
    void RegisterThreadPeerHint(chip::NodeId nodeId, chip::FabricIndex fabricIndex,
                                const chip::Inet::IPAddress & addr, uint16_t port);
} }

/* OpenThread */
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <esp_openthread_types.h>
#include <openthread/dataset.h>
#include <openthread/dataset_ftd.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>
#include <openthread/ip6.h>
#include <openthread/srp_server.h>

/* ===================================================================== */
/*  Constants                                                              */
/* ===================================================================== */
#define TAG                 "hub"
#define BUTTON_PIN          9
#define LONG_PRESS_MS       5000

/* Subscription intervals - a per-device CASE keepalive/liveness subscription.
 *
 * SUB_MIN_S is the MinIntervalFloor: a publisher may not send a second report
 * until it elapses. It MUST be 0 here. With the 5 s inherited from ALPSTUGA a
 * button press delivered InitialPress immediately and then stalled ~5 s before
 * batching ShortRelease + MultiPressComplete together - the switch felt broken
 * even though the device had reported instantly. 0 = report as events happen.
 * This does not cost battery: it removes a throttle, it does not force polling
 * (the device still only transmits when something actually changes). */
#define SUB_MIN_S           0
#define SUB_MAX_S           30

/* Build-time fallback so the hub can be provisioned without the console. */
#define DEFAULT_PAYLOAD     ""

/* NVS */
#define NVS_NS              "matterhub"
#define K_PIN               "pin_code"
#define K_DISC              "discrim"
#define K_PAYLOAD           "payload"
#define K_DEVTAB            "devtab"       /* blob: dev_nvs_t[MAX_DEVICES]      */
#define K_DEVID             "devid"        /* blob: dev_id_nvs_t[MAX_DEVICES]   */
#define K_DEVCAP            "devcap"       /* blob: uint8_t[MAX_DEVICES] cap mask */
#define K_CAPVER            "capver"       /* u8: cap-mask scheme version (see CAP_SCHEME_VER) */
#define K_DEVLABEL          "devlabel"     /* blob: char[MAX_DEVICES][32] user labels */
#define K_DEBUG             "dbg"          /* u8: persisted verbose-log setting  */
#define K_RESTOREJ          "restorej"     /* blob: pending restore JSON (identity->slot after re-pair) */
#define K_CMODE             "cmode"        /* u8: one-shot enter-commissioning  */

/* Event bits */
#define EVT_SHORT           (1u << 0)
#define EVT_LONG            (1u << 1)
#define EVT_COMMISSIONED    (1u << 2)
#define EVT_THREAD_READY    (1u << 3)

/* ===================================================================== */
/*  Globals                                                                */
/* ===================================================================== */
static EventGroupHandle_t s_evt;

DeviceInfo g_dev[MAX_DEVICES];      /* definition (declared extern in device_model.h) */
int        g_dev_count = 0;

/* Per-device subscription-capability mask (persisted, K_DEVCAP). bit7=CAP_KNOWN
 * (enumerated at least once); bits0-4 = device HAS {temp,humidity,co2,pm25,
 * battery}; bit5 = device HAS OnOff (0x0006). Until known, subscriptions carry the
 * full set (safe); once known, start_subscription_for scopes the extra paths so e.g.
 * an air-quality sensor carries no OnOff path and a plug carries no sensor paths. */
static uint8_t g_devcap[MAX_DEVICES];
#define CAP_KNOWN 0x80
/* Bump whenever cap_bit_for()/hub_update_devcap() gain a NEW cluster bit. A stored
 * mask written by an older scheme has CAP_KNOWN set but lacks the new bit, so it would
 * wrongly scope the new path OUT until a second reboot. On a version mismatch, nvs_load
 * drops CAP_KNOWN for every device so each re-derives its full mask on this boot's
 * enumeration (full path set used until then - the safe default). v2 = added OnOff. */
#define CAP_SCHEME_VER 2

static bool     g_commission_mode = false;  /* one-shot NVS flag: boot into BLE   */
static bool     g_auto_commission = false;  /* 'pair'-triggered: auto-fire commission */
static int      g_staging_slot    = -1;     /* slot the next commission fills      */
static uint64_t g_node_id         = NODE_ID_BASE;
static uint32_t g_pin             = 20202021;
static uint16_t g_disc            = 3840;
static bool     g_paired          = false;  /* operating mode (>=1 paired, not commissioning) */
static char     g_payload[72]     = {};
static bool     g_debug_verbose   = false;
static bool     g_thread_paused_for_ble = false;

/* ---- batch pairing (v1.7): a persistent payload list drives the existing
 * reboot-per-device commissioning loop. The list is RETAINED with a per-entry
 * outcome + a cursor (not popped) so a final report can say which paired and which
 * could not. `cur` = entry being commissioned now; `attempts` = boots spent on it. */
#define PAIRQ_MAX          7
#define PAIRQ_MAX_ATTEMPTS 2
enum { PQ_PENDING = 0, PQ_PAIRED = 1, PQ_FAILED = 2 };
struct pairq_entry_t { char code[72]; uint8_t status; uint8_t slot; };
struct pairq_t { uint8_t n; uint8_t cur; uint8_t attempts; pairq_entry_t e[PAIRQ_MAX]; };
static pairq_t g_pairq;
#define K_PAIRQ "pairq"     /* blob: pairq_t */

/* A batch is in progress iff there are entries still to try. */
static inline bool batch_active(void) { return g_pairq.n > 0 && g_pairq.cur < g_pairq.n; }

/* Persisted (NVS blob) subset of DeviceInfo.
 * NOTE: do NOT add fields here - nvs_load() requires the stored blob to match
 * sizeof(tab) exactly, so growing this struct silently discards every existing
 * pairing on the next boot. New persisted data goes in its own key (see below). */
struct dev_nvs_t {
    uint8_t  paired;
    uint8_t  eui64_known;
    uint8_t  eui64[8];
    uint64_t node_id;
    char     payload[72];
};

/* Last-known identity, kept in a SEPARATE key so dev_nvs_t stays size-stable.
 * The endpoint tree itself is deliberately RAM-only (heap, FOUNDATION section 7)
 * and re-enumerated each boot - but vid/pid/name are tiny and there is no reason
 * to forget what a device IS just because it is currently asleep. Without this a
 * sleepy battery device (e.g. the BILRESA button) shows up as "?" / VID=0x0000
 * in `devices` for most of its life, which is useless. */
struct dev_id_nvs_t {
    uint16_t vid;
    uint16_t pid;
    char     name[32];
};

/* ===================================================================== */
/*  NVS                                                                    */
/* ===================================================================== */
static void nvs_load(void)
{
    for (int i = 0; i < MAX_DEVICES; ++i) {
        memset(&g_dev[i], 0, sizeof(g_dev[i]));
        g_dev[i].node_id = slot_node_id(i);
        g_dev[i].paired  = false;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    /* The blob is MAX_DEVICES * sizeof(dev_nvs_t), so raising MAX_DEVICES changes
     * its size. Accept a SMALLER stored blob (written by an older, fewer-slot
     * build) and migrate it into the low slots instead of discarding every
     * pairing: nvs_get_blob fills only the bytes present, the rest stay zeroed
     * (= unpaired). A LARGER stored blob (someone shrank MAX_DEVICES) is refused
     * rather than truncated, so we never silently drop a paired device. */
    dev_nvs_t tab[MAX_DEVICES];
    memset(tab, 0, sizeof(tab));
    size_t stored = 0;
    bool   have_tab = false;
    if (nvs_get_blob(h, K_DEVTAB, nullptr, &stored) == ESP_OK &&
        stored > 0 && (stored % sizeof(dev_nvs_t)) == 0) {
        int n = (int)(stored / sizeof(dev_nvs_t));
        if (stored <= sizeof(tab)) {
            /* Same-size or SMALLER stored blob (older, fewer-slot build): read
             * directly; slots [n..MAX_DEVICES) stay zeroed (= unpaired). */
            size_t len = stored;
            if (nvs_get_blob(h, K_DEVTAB, tab, &len) == ESP_OK) {
                if (n != MAX_DEVICES)
                    ESP_LOGW(TAG, "Migrating device table: %d stored slots -> %d", n, MAX_DEVICES);
                have_tab = true;
            }
        } else {
            /* LARGER stored blob (MAX_DEVICES was shrunk). Load the first
             * MAX_DEVICES slots, but ONLY if no PAIRED device lives in a slot we
             * would drop - otherwise refuse rather than silently orphan it. */
            dev_nvs_t *big = (dev_nvs_t *)malloc(stored);
            size_t len = stored;
            if (big && nvs_get_blob(h, K_DEVTAB, big, &len) == ESP_OK) {
                bool drop_paired = false;
                for (int i = MAX_DEVICES; i < n; ++i) if (big[i].paired) drop_paired = true;
                if (drop_paired) {
                    ESP_LOGE(TAG, "Stored device table (%d slots) > MAX_DEVICES=%d and a paired device "
                             "sits in a dropped slot - refusing to truncate; raise MAX_DEVICES or "
                             "'remove' the high-slot device first", n, MAX_DEVICES);
                } else {
                    ESP_LOGW(TAG, "Migrating device table (shrink): %d stored slots -> %d (dropped "
                             "slots empty)", n, MAX_DEVICES);
                    memcpy(tab, big, sizeof(tab));
                    have_tab = true;
                }
            }
            free(big);
        }
    }
    if (have_tab) {
        for (int i = 0; i < MAX_DEVICES; ++i) {
            g_dev[i].paired      = tab[i].paired != 0;
            g_dev[i].eui64_known = tab[i].eui64_known != 0;
            memcpy(g_dev[i].eui64.m8, tab[i].eui64, 8);
            g_dev[i].node_id     = tab[i].node_id ? tab[i].node_id : slot_node_id(i);
            tab[i].payload[sizeof(tab[i].payload) - 1] = '\0';
            strncpy(g_dev[i].payload, tab[i].payload, sizeof(g_dev[i].payload) - 1);
        }
    }

    /* Last-known identity (optional key; absent on older firmware -> stays 0/"?").
     * Same smaller-blob migration as the device table above. */
    dev_id_nvs_t ids[MAX_DEVICES];
    memset(ids, 0, sizeof(ids));
    size_t id_stored = 0;
    if (nvs_get_blob(h, K_DEVID, nullptr, &id_stored) == ESP_OK &&
        id_stored > 0 && id_stored <= sizeof(ids) && (id_stored % sizeof(dev_id_nvs_t)) == 0) {
        size_t len = id_stored;
        if (nvs_get_blob(h, K_DEVID, ids, &len) == ESP_OK) {
            int n = (int)(id_stored / sizeof(dev_id_nvs_t));
            for (int i = 0; i < n && i < MAX_DEVICES; ++i) {
                g_dev[i].vid = ids[i].vid;
                g_dev[i].pid = ids[i].pid;
                ids[i].name[sizeof(ids[i].name) - 1] = '\0';
                strncpy(g_dev[i].name, ids[i].name, sizeof(g_dev[i].name) - 1);
            }
        }
    }

    /* Capability masks (optional key). Smaller stored blob (fewer slots) migrates
     * into the low slots; the rest stay 0 (= unknown -> full paths, safe). A
     * larger stored blob (MAX_DEVICES shrunk) is ignored -> all unknown; caps are
     * re-derived on the next enumeration, so nothing breaks. */
    memset(g_devcap, 0, sizeof(g_devcap));
    size_t cap_stored = 0;
    if (nvs_get_blob(h, K_DEVCAP, nullptr, &cap_stored) == ESP_OK &&
        cap_stored > 0 && cap_stored <= sizeof(g_devcap)) {
        size_t len = cap_stored;
        nvs_get_blob(h, K_DEVCAP, g_devcap, &len);
    }
    /* Cap-scheme migration (see CAP_SCHEME_VER): a mask from an older scheme has
     * CAP_KNOWN set but lacks any newly-added bit (e.g. OnOff in v2), which would
     * scope the new path OUT for already-paired devices until a second reboot. Drop
     * CAP_KNOWN so each device re-derives its full mask on this boot's enumeration;
     * hub_update_devcap() + nvs_save() then rewrite the mask and stamp the new version. */
    uint8_t capver = 0;
    nvs_get_u8(h, K_CAPVER, &capver);   /* absent on older firmware -> 0 -> migrate */
    if (capver != CAP_SCHEME_VER)
        for (int i = 0; i < MAX_DEVICES; ++i) g_devcap[i] &= (uint8_t)~CAP_KNOWN;

    /* User labels (optional key). Read into a temp then copy per slot so a shorter
     * stored blob (fewer slots) migrates into the low slots; the rest stay "". */
    char labels[MAX_DEVICES][32];
    memset(labels, 0, sizeof(labels));
    size_t lbl_stored = 0;
    if (nvs_get_blob(h, K_DEVLABEL, nullptr, &lbl_stored) == ESP_OK &&
        lbl_stored > 0 && lbl_stored <= sizeof(labels) && (lbl_stored % 32) == 0) {
        size_t len = lbl_stored;
        if (nvs_get_blob(h, K_DEVLABEL, labels, &len) == ESP_OK) {
            int n = (int)(lbl_stored / 32);
            for (int i = 0; i < n && i < MAX_DEVICES; ++i) {
                labels[i][31] = '\0';
                strncpy(g_dev[i].label, labels[i], sizeof(g_dev[i].label) - 1);
            }
        }
    }

    /* Batch-pairing list (own key; exact-size blob - a size mismatch from a struct
     * change is ignored, the batch is just abandoned, which is safe). */
    memset(&g_pairq, 0, sizeof(g_pairq));
    size_t pq_stored = 0;
    if (nvs_get_blob(h, K_PAIRQ, nullptr, &pq_stored) == ESP_OK && pq_stored == sizeof(g_pairq)) {
        size_t len = pq_stored;
        nvs_get_blob(h, K_PAIRQ, &g_pairq, &len);
    }

    nvs_get_u32(h, K_PIN,  &g_pin);
    uint16_t d = g_disc; nvs_get_u16(h, K_DISC, &d); g_disc = d;
    uint8_t cm = 0; nvs_get_u8(h, K_CMODE, &cm); g_commission_mode = cm != 0;
    uint8_t dbg = 0; nvs_get_u8(h, K_DEBUG, &dbg); g_debug_verbose = dbg != 0;   /* restored at boot */
    size_t pld_len = sizeof(g_payload);
    nvs_get_str(h, K_PAYLOAD, g_payload, &pld_len);
    nvs_close(h);

    g_dev_count = dev_count_paired();
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    dev_nvs_t tab[MAX_DEVICES];
    memset(tab, 0, sizeof(tab));
    for (int i = 0; i < MAX_DEVICES; ++i) {
        tab[i].paired      = g_dev[i].paired ? 1 : 0;
        tab[i].eui64_known = g_dev[i].eui64_known ? 1 : 0;
        memcpy(tab[i].eui64, g_dev[i].eui64.m8, 8);
        tab[i].node_id     = g_dev[i].node_id;
        strncpy(tab[i].payload, g_dev[i].payload, sizeof(tab[i].payload) - 1);
    }
    nvs_set_blob(h, K_DEVTAB, tab, sizeof(tab));

    dev_id_nvs_t ids[MAX_DEVICES];
    memset(ids, 0, sizeof(ids));
    for (int i = 0; i < MAX_DEVICES; ++i) {
        ids[i].vid = g_dev[i].vid;
        ids[i].pid = g_dev[i].pid;
        strncpy(ids[i].name, g_dev[i].name, sizeof(ids[i].name) - 1);
    }
    nvs_set_blob(h, K_DEVID, ids, sizeof(ids));
    nvs_set_blob(h, K_DEVCAP, g_devcap, sizeof(g_devcap));
    nvs_set_u8  (h, K_CAPVER, CAP_SCHEME_VER);   /* stamp the scheme the mask was written under */

    char labels[MAX_DEVICES][32];
    memset(labels, 0, sizeof(labels));
    for (int i = 0; i < MAX_DEVICES; ++i)
        strncpy(labels[i], g_dev[i].label, 31);
    nvs_set_blob(h, K_DEVLABEL, labels, sizeof(labels));
    nvs_set_blob(h, K_PAIRQ,  &g_pairq, sizeof(g_pairq));

    nvs_set_u8 (h, K_CMODE,   g_commission_mode ? 1 : 0);
    nvs_set_u32(h, K_PIN,     g_pin);
    nvs_set_u16(h, K_DISC,    g_disc);
    nvs_set_str(h, K_PAYLOAD, g_payload);
    nvs_commit(h);
    nvs_close(h);

    g_dev_count = dev_count_paired();
}

/* Exposed to inspector.cpp (declared in device_model.h) so a freshly enumerated
 * identity is remembered across reboots. */
void hub_persist_devices(void) { nvs_save(); }

/* ---- subscription-capability scoping ------------------------------------- *
 * The "extra" attribute paths a subscription may carry beyond the Basic Info
 * liveness path: the logic sensors + the dash battery. Each maps to a cap bit. */
static int cap_bit_for(uint32_t cluster)
{
    switch (cluster) {
    case 0x0402UL: return 0;   /* TemperatureMeasurement        */
    case 0x0405UL: return 1;   /* RelativeHumidityMeasurement   */
    case 0x040DUL: return 2;   /* CarbonDioxideConcentration    */
    case 0x042AUL: return 3;   /* Pm25ConcentrationMeasurement  */
    case 0x002FUL: return 4;   /* PowerSource (battery)         */
    case 0x0006UL: return 5;   /* OnOff (plug state)            */
    default:       return -1;
    }
}

/* Recompute a device's cap mask from its enumerated tree (see device_model.h). */
void hub_update_devcap(int slot)
{
    if (slot < 0 || slot >= MAX_DEVICES) return;
    DeviceInfo &d = g_dev[slot];
    static const uint32_t kExtra[] = { 0x0402UL, 0x0405UL, 0x040DUL, 0x042AUL, 0x002FUL, 0x0006UL };
    uint8_t cap = CAP_KNOWN;
    for (unsigned k = 0; k < sizeof(kExtra) / sizeof(kExtra[0]); ++k)
        for (int i = 0; i < d.n_eps; ++i)
            if (d.eps[i] && ep_find_cluster(*d.eps[i], kExtra[k])) { cap |= (uint8_t)(1u << cap_bit_for(kExtra[k])); break; }
    g_devcap[slot] = cap;
}

/* Should this device's subscription include the extra path for `cluster`? Yes if
 * the device is not yet enumerated (unknown caps -> include, so data still flows),
 * or if its cap mask says it has the cluster. */
static bool devcap_wants(int slot, uint32_t cluster)
{
    if (slot < 0 || slot >= MAX_DEVICES) return true;
    uint8_t cap = g_devcap[slot];
    if (!(cap & CAP_KNOWN)) return true;
    int b = cap_bit_for(cluster);
    if (b < 0) return true;
    return (cap >> b) & 1;
}

/* ===================================================================== */
/*  Debug log level                                                        */
/* ===================================================================== */
/* Task handles for the `heap` command's stack high-water report (Phase 3 of the
 * v1.6 RAM work): measurements first, stack tuning only ever after. */
static TaskHandle_t s_serial_task_h = nullptr;
static TaskHandle_t s_sub_task_h    = nullptr;

static void apply_debug_level(bool verbose)
{
    esp_log_level_t lvl = verbose ? ESP_LOG_INFO : ESP_LOG_WARN;
    esp_log_level_set("CHIP",       lvl);
    esp_log_level_set("NimBLE",     lvl);
    /* Our own + esp-matter component INFO chatter (subscribe retries, per-device
     * enumeration progress, "Subscription established"/"read done") is the bulk of
     * what fills the console with 'debug off'. Silence it to WARN unless verbose;
     * user-facing state still prints via printf ("[hub] ..."), and warnings/errors
     * (partial enum, CASE timeouts) still surface. */
    esp_log_level_set("hub",          lvl);
    esp_log_level_set("inspector",    lvl);
    esp_log_level_set("read_command", lvl);
    esp_log_level_set("logic",        lvl);
    esp_log_level_set("dash",         lvl);
    /* OpenThread is noisy at WARN: repeated SRP "Name conflict"/"Duplicated"
     * spam when a device re-registers a service name still held by a stale lease,
     * plus transient MLE "Link Accept" warnings. None affect operation - CASE
     * succeeds via link-local peer hints - so keep OT at ERROR unless verbose to
     * keep the console readable. 'debug on' restores full OpenThread logs. */
    esp_log_level_set("OPENTHREAD", verbose ? ESP_LOG_INFO : ESP_LOG_ERROR);
    chip::Logging::SetLogFilter(verbose ? chip::Logging::kLogCategory_Detail
                                        : chip::Logging::kLogCategory_Error);
}

/* ===================================================================== */
/*  Thread dataset + peer-hint machinery (generic; FOUNDATION §5)          */
/* ===================================================================== */
static int thread_dataset_bin(uint8_t *buf, size_t max_len)
{
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ot = esp_openthread_get_instance();
    int len = -1;
    if (ot) {
        otOperationalDatasetTlvs tlvs; memset(&tlvs, 0, sizeof(tlvs));
        if (otDatasetGetActiveTlvs(ot, &tlvs) == OT_ERROR_NONE &&
            tlvs.mLength > 0 && tlvs.mLength <= max_len) {
            memcpy(buf, tlvs.mTlvs, tlvs.mLength);
            len = (int)tlvs.mLength;
        }
    }
    esp_openthread_lock_release();
    return len;
}

#define HLOG(...)  do { if (g_debug_verbose) ESP_LOGI(TAG, __VA_ARGS__); } while (0)

static int dev_slot_by_eui(const uint8_t *ext)
{
    for (int i = 0; i < MAX_DEVICES; ++i)
        if (g_dev[i].paired && g_dev[i].eui64_known &&
            memcmp(g_dev[i].eui64.m8, ext, OT_EXT_ADDRESS_SIZE) == 0) return i;
    return -1;
}

static int dev_learn_slot(void)
{
    if (g_staging_slot >= 0)
        return g_dev[g_staging_slot].eui64_known ? -1 : g_staging_slot;
    for (int i = 0; i < MAX_DEVICES; ++i)
        if (g_dev[i].paired && !g_dev[i].eui64_known) return i;
    return -1;
}

/* Register the link-local (fe80::) peer hint for node_id from its EUI-64.
 * Must be called with the OpenThread lock held; drops+reacquires it around the
 * CHIP call. (See FOUNDATION §5 mitigation.) */
static void register_ll_hint(uint64_t node_id, const uint8_t ext[8])
{
    otIp6Address ll; memset(&ll, 0, sizeof(ll));
    ll.mFields.m8[0] = 0xfe; ll.mFields.m8[1] = 0x80;
    memcpy(&ll.mFields.m8[8], ext, 8);
    ll.mFields.m8[8] ^= 0x02;
    chip::Inet::IPAddress a; memcpy(a.Addr, ll.mFields.m8, 16);
    esp_openthread_lock_release();
    chip::Dnssd::RegisterThreadPeerHint(node_id, 1, a, 5540);
    esp_openthread_lock_acquire(portMAX_DELAY);
}

static void register_rloc_hint(otInstance *ot, uint64_t node_id, uint16_t rloc16)
{
    if (rloc16 == 0 || !ot) return;
    const otIp6Address *myEid = otThreadGetMeshLocalEid(ot);
    if (!myEid) return;
    otIp6Address r; memset(&r, 0, sizeof(r));
    memcpy(r.mFields.m8, myEid->mFields.m8, 8);
    r.mFields.m8[11] = 0xff; r.mFields.m8[12] = 0xfe;
    r.mFields.m8[14] = (rloc16 >> 8) & 0xff;
    r.mFields.m8[15] = rloc16 & 0xff;
    chip::Inet::IPAddress a; memcpy(a.Addr, r.mFields.m8, 16);
    esp_openthread_lock_release();
    chip::Dnssd::RegisterThreadPeerHint(node_id, 1, a, 5540);
    esp_openthread_lock_acquire(portMAX_DELAY);
}

/* Register CASE peer hints from the hub's own SRP SERVER.
 *
 * The child/neighbor-table hints above only cover devices that are a DIRECT
 * Thread neighbor of the hub (a fe80:: link-local address is one-hop-only). A
 * device that attaches to a *different* router (e.g. one of the paired sensors,
 * which become Thread routers) is not in our tables, gets no hint, and then
 * "operational discovery failed: 32" because mDNS/SRP resolution times out.
 *
 * But every commissioned device registers its operational service with the SRP
 * server that runs ON THIS HUB - including a ROUTABLE address (mesh-local /
 * global). So we can read that address straight out of the SRP server and hint
 * it, giving CASE a working address for a device ANYWHERE on the mesh. This is
 * the fix for the recurring post-PASE operational-discovery stall.
 *
 * Called with the OT lock held; collects first (lock held, safe iteration) then
 * registers (dropping/reacquiring the lock around each CHIP call, as above). */
static void register_srp_peer_hints(otInstance *ot)
{
    if (!ot) return;
    struct { uint64_t node; otIp6Address addr; } found[2 * MAX_DEVICES];
    int nf = 0, nhosts = 0;
    const int kMaxFound = (int)(sizeof(found) / sizeof(found[0]));

    const otSrpServerHost *host = nullptr;
    while ((host = otSrpServerGetNextHost(ot, host)) != nullptr && nf < kMaxFound) {
        ++nhosts;
        if (otSrpServerHostIsDeleted(host)) continue;
        uint8_t naddr = 0;
        const otIp6Address *addrs = otSrpServerHostGetAddresses(host, &naddr);
        if (!addrs || naddr == 0) continue;
        /* Prefer a routable (non-link-local) address; fall back to the first. */
        const otIp6Address *best = &addrs[0];
        for (uint8_t i = 0; i < naddr; ++i) {
            bool ll = (addrs[i].mFields.m8[0] == 0xfe && (addrs[i].mFields.m8[1] & 0xc0) == 0x80);
            if (!ll) { best = &addrs[i]; break; }
        }
        const otSrpServerService *svc = nullptr;
        while ((svc = otSrpServerHostGetNextService(host, svc)) != nullptr && nf < kMaxFound) {
            if (otSrpServerServiceIsDeleted(svc)) continue;
            const char *sname = otSrpServerServiceGetServiceName(svc);
            if (!sname || !strstr(sname, "_matter._tcp")) continue;       /* operational service only */
            const char *label = otSrpServerServiceGetInstanceLabel(svc);  /* "<fabric>-<node>" */
            const char *dash  = label ? strrchr(label, '-') : nullptr;
            if (!dash) continue;
            uint64_t node = strtoull(dash + 1, nullptr, 16);
            if (node == 0) continue;
            found[nf].node = node;
            found[nf].addr = *best;
            ++nf;
        }
    }

    /* Diagnostic (verbose only): tells us whether the SRP server actually has the
     * device's routable address when CASE needs it. Silenced with 'debug off'. */
    if (g_debug_verbose)
        ESP_LOGW(TAG, "SRP hint scan: %d host(s), %d matter service(s) hinted", nhosts, nf);

    for (int i = 0; i < nf; ++i) {
        chip::Inet::IPAddress a; memcpy(a.Addr, found[i].addr.mFields.m8, 16);
        esp_openthread_lock_release();
        chip::Dnssd::RegisterThreadPeerHint(found[i].node, 1, a, 5540);
        esp_openthread_lock_acquire(portMAX_DELAY);
        if (g_debug_verbose)
            ESP_LOGW(TAG, "SRP peer hint: node 0x%016" PRIx64 " <- routable addr from SRP server",
                     found[i].node);
    }
}

/* Scan the Thread child/neighbor tables, learn each device's EUI-64/RLOC16,
 * sample link signal, and register CASE peer hints. Auto-reschedules every 60 s
 * while paired (appState>=100). */
static void scan_thread_peers(chip::System::Layer *, void *appState)
{
    /* Verbose-only: proves whether the peer-hint scanner actually runs during a
     * commissioning session (it is scheduled from pase_callback). 'debug off' hides it. */
    if (g_debug_verbose)
        ESP_LOGW(TAG, "peer scan pass (appState=%u)", (unsigned)(uintptr_t)appState);
    HLOG("HEAP free=%u min=%u largest=%u",
         (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ot = esp_openthread_get_instance();
    if (ot) {
        bool seen[MAX_DEVICES] = { false };

        uint16_t idx = 0; otChildInfo ci;
        while (otThreadGetChildInfoByIndex(ot, idx, &ci) == OT_ERROR_NONE) {
            /* rxOnIdle=0 marks a sleepy end device. Logged because "is the device
             * actually attached to us as a Thread child?" is the first question
             * when CASE keeps failing - a link-local peer hint is only reachable
             * for a DIRECT neighbour. */
            HLOG("Thread child[%u] rloc16=0x%04x rxOnIdle=%d ftd=%d eui=%02x%02x%02x%02x%02x%02x%02x%02x",
                 (unsigned)idx, (unsigned)ci.mRloc16, (int)ci.mRxOnWhenIdle, (int)ci.mFullThreadDevice,
                 ci.mExtAddress.m8[0], ci.mExtAddress.m8[1], ci.mExtAddress.m8[2], ci.mExtAddress.m8[3],
                 ci.mExtAddress.m8[4], ci.mExtAddress.m8[5], ci.mExtAddress.m8[6], ci.mExtAddress.m8[7]);
            int slot = dev_slot_by_eui(ci.mExtAddress.m8);
            if (slot < 0) {
                int L = dev_learn_slot();
                if (L >= 0) { memcpy(g_dev[L].eui64.m8, ci.mExtAddress.m8, OT_EXT_ADDRESS_SIZE);
                              g_dev[L].eui64_known = true; slot = L; }
            }
            if (slot >= 0) {
                g_dev[slot].rloc16 = ci.mRloc16; g_dev[slot].rssi = ci.mAverageRssi;
                g_dev[slot].lqi = ci.mLinkQualityIn; g_dev[slot].link_valid = true;
                register_ll_hint(g_dev[slot].node_id, ci.mExtAddress.m8);
                seen[slot] = true;
            }
            ++idx;
        }

        otNeighborInfoIterator nbrIt = OT_NEIGHBOR_INFO_ITERATOR_INIT; otNeighborInfo nbr;
        while (otThreadGetNextNeighborInfo(ot, &nbrIt, &nbr) == OT_ERROR_NONE) {
            HLOG("Thread nbr rloc16=0x%04x rxOnIdle=%d rssi=%d eui=%02x%02x%02x%02x%02x%02x%02x%02x",
                 (unsigned)nbr.mRloc16, (int)nbr.mRxOnWhenIdle, (int)nbr.mAverageRssi,
                 nbr.mExtAddress.m8[0], nbr.mExtAddress.m8[1], nbr.mExtAddress.m8[2], nbr.mExtAddress.m8[3],
                 nbr.mExtAddress.m8[4], nbr.mExtAddress.m8[5], nbr.mExtAddress.m8[6], nbr.mExtAddress.m8[7]);
            int slot = dev_slot_by_eui(nbr.mExtAddress.m8);
            if (slot < 0) {
                int L = dev_learn_slot();
                if (L >= 0) { memcpy(g_dev[L].eui64.m8, nbr.mExtAddress.m8, OT_EXT_ADDRESS_SIZE);
                              g_dev[L].eui64_known = true; slot = L; }
            }
            if (slot >= 0) {
                g_dev[slot].rloc16 = nbr.mRloc16; g_dev[slot].rssi = nbr.mAverageRssi;
                g_dev[slot].lqi = nbr.mLinkQualityIn; g_dev[slot].link_valid = true;
                if (!seen[slot]) { register_ll_hint(g_dev[slot].node_id, nbr.mExtAddress.m8); seen[slot] = true; }
            }
        }

        for (int i = 0; i < MAX_DEVICES; ++i) {
            bool active = g_dev[i].paired || (i == g_staging_slot);
            if (!active) continue;
            /* Which devices do we actually have a reachable address for? A device
             * with eui64_known=0 has never been seen in our tables, so we have no
             * peer hint at all and CASE must fall back to mDNS/SRP. */
            HLOG("dev#%d eui64_known=%d seen_now=%d rloc16=0x%04x reporting=%d",
                 i + 1, (int)g_dev[i].eui64_known, (int)seen[i],
                 (unsigned)g_dev[i].rloc16, (int)g_dev[i].has_data);
            if (!g_dev[i].eui64_known) continue;
            /* Register the permanent link-local hint even when the device was not
             * in our tables this pass. This is FOUNDATION section 5's mitigation for
             * "operational discovery failed: 32" and is what lets CASE reach a
             * sleepy end device that does not receive ff02::fb until it data-polls.
             * (Tried gating this on seen[] - it does NOT help: mDNS resolution
             * times out on its own, so withholding the hint only removes the one
             * path that can work.) */
            if (!seen[i]) register_ll_hint(g_dev[i].node_id, g_dev[i].eui64.m8);
            register_rloc_hint(ot, g_dev[i].node_id, g_dev[i].rloc16);
        }

        /* The real fix: hint every device the SRP server knows about, using its
         * routable registered address - covers devices that are NOT our direct
         * neighbor (which the link-local hints above miss). */
        register_srp_peer_hints(ot);
    }
    esp_openthread_lock_release();

    if (g_paired && (uintptr_t)appState >= 100)
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(60000), scan_thread_peers,
            (void*)((uintptr_t)appState + 1));
}

/* Restart minimal-mDNS server + join ff02::fb in OpenThread once the Thread
 * interface is up, so operational discovery works (FOUNDATION §5). */
static void restart_mdns_after_thread_resume(chip::System::Layer *, void *)
{
    ESP_LOGI(TAG, "Restarting minimal-mDNS server for Thread interface");
    CHIP_ERROR err = chip::Dnssd::ServiceAdvertiser::Instance().Init(
        chip::DeviceLayer::UDPEndPointManager());
    if (err != CHIP_NO_ERROR)
        ESP_LOGE(TAG, "mDNS restart failed: %" CHIP_ERROR_FORMAT, err.Format());

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ot = esp_openthread_get_instance();
    if (ot) {
        static const otIp6Address kMdns = {
            .mFields = { .m8 = {0xFF,0x02,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0xFB} } };
        otError oe = otIp6SubscribeMulticastAddress(ot, &kMdns);
        if (oe != OT_ERROR_NONE && oe != OT_ERROR_ALREADY)
            ESP_LOGW(TAG, "subscribe ff02::fb: error %d", (int)oe);
    }
    esp_openthread_lock_release();
}

static void maybe_resume_thread(void)
{
    /* Thread is intentionally left running during commissioning (BLE/802.15.4
     * coexist for PASE on the C6), so this is normally a no-op. Kept as a safety
     * net if a future change pauses Thread for BLE. */
    if (!g_thread_paused_for_ble) return;
    g_thread_paused_for_ble = false;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ot = esp_openthread_get_instance();
    if (ot) {
        if (!otIp6IsEnabled(ot)) otIp6SetEnabled(ot, true);
        if (otThreadGetDeviceRole(ot) == OT_DEVICE_ROLE_DISABLED) otThreadSetEnabled(ot, true);
    }
    esp_openthread_lock_release();
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(1000), restart_mdns_after_thread_resume, nullptr);
}

/* ===================================================================== */
/*  Per-device CASE keepalive subscription (+ enumeration trigger)         */
/* ===================================================================== */
static void subscribe_work(intptr_t);   /* fwd decl */

/* Liveness: any report advances the device's clock. We subscribe to a stable,
 * universally-present attribute (Basic Information / DataModelRevision) purely
 * to keep the CASE session warm and detect loss of comms. */
static void liveness_attribute_cb(uint64_t node_id, const chip::app::ConcreteDataAttributePath &path,
                                  chip::TLV::TLVReader *data)
{
    int slot = dev_slot_by_node(node_id);
    if (slot < 0) return;
    g_dev[slot].has_data = true;
    g_dev[slot].last_report_us = esp_timer_get_time();

    /* Feed the logic engine any numeric measurement report (temp/CO2/...). The
     * Basic Info liveness attribute won't match its sensor table -> no-op. We are
     * on the CHIP task, so a rule firing here may originate its OnOff safely. */
    if (data) {
        chip::TLV::TLVReader r; r.Init(*data);
        double v;
        if (tlv_get_scalar(r, v)) {
            logic_on_report(node_id, path.mEndpointId, path.mClusterId, path.mAttributeId, v);
            dash_on_report(node_id, path.mEndpointId, path.mClusterId, path.mAttributeId, v);
        }
    }
    /* First report proves CASE is up (the report arrived over CASE). Trigger
     * enumeration here - more reliable than the subscribe done_cb, which this
     * esp-matter build does not fire dependably. inspector_on_case_up() is
     * guarded to run exactly once per device. */
    inspector_on_case_up(node_id);
}

static void subscribe_done_cb(uint64_t node_id, uint32_t sub_id)
{
    int slot = dev_slot_by_node(node_id);
    ESP_LOGI(TAG, "CASE up / subscription established node=0x%016" PRIx64 " (sub_id=%" PRIu32 ")",
             node_id, sub_id);
    if (slot >= 0) {
        g_dev[slot].has_data = true;
        g_dev[slot].sub_id   = sub_id;
    }
    /* Additive: discover the device's structure now that CASE is up. Never
     * inline in the commissioning callback (FOUNDATION §5/§11). */
    inspector_on_case_up(node_id);
}

static void subscribe_failure_cb(void *) { ESP_LOGE(TAG, "Subscription failed - will auto-retry"); }

/* MultiPressComplete carries the press count in context tag 1
 * (TotalNumberOfPressesCounted): 1 = single, 2 = double. Pull it so a binding can
 * distinguish single- vs double-press. Returns 0 if not found. */
static uint8_t switch_multipress_count(chip::TLV::TLVReader *data)
{
    if (!data) return 0;
    chip::TLV::TLVReader r; r.Init(*data);
    if (r.GetType() != chip::TLV::kTLVType_Structure) return 0;
    chip::TLV::TLVType outer;
    if (r.EnterContainer(outer) != CHIP_NO_ERROR) return 0;
    uint8_t count = 0;
    while (r.Next() == CHIP_NO_ERROR) {
        if (chip::TLV::IsContextTag(r.GetTag()) && chip::TLV::TagNumFromTag(r.GetTag()) == 1) {
            uint16_t v = 0; if (r.Get(v) == CHIP_NO_ERROR) count = (uint8_t)v;
        }
    }
    r.ExitContainer(outer);
    return count;
}

/* Event reports. This is how a Generic Switch (e.g. IKEA BILRESA) tells us a
 * button was pressed - it has no commands and no "pressed" attribute, only
 * events (InitialPress / ShortRelease / LongPress / MultiPressComplete). Also
 * carries StartUp, fault changes, etc. Payload is decoded generically. */
static void event_report_cb(uint64_t node_id, const chip::app::EventHeader &header,
                            chip::TLV::TLVReader *data)
{
    /* TimeSynchronization spams TimeFailure (~1/s) on any device whose clock was
     * never set - it would bury everything else. Not actionable for the hub, so
     * drop it unless verbose. (Root fix: give the device a clock by invoking
     * SetUTCTime on its TimeSynchronization cluster.) */
    if (header.mPath.mClusterId == 0x0038 && !g_debug_verbose) return;

    int slot = dev_slot_by_node(node_id);
    /* An event arriving PROVES the CASE session + subscription are alive, just as
     * much as an attribute report does. Mark liveness here too, otherwise a device
     * that reports events but whose attribute report is slow looks "never heard
     * from" and the retry loop below re-subscribes it - piling up duplicate
     * subscriptions that then die with liveness timeouts. */
    if (slot >= 0) {
        g_dev[slot].has_data       = true;
        g_dev[slot].last_report_us = esp_timer_get_time();
    }
    char val[192] = "";
    if (data) { chip::TLV::TLVReader r; r.Init(*data); tlv_to_str(r, val, sizeof(val)); }
    char clb[40], evb[48];
    printf("[hub] EVENT #%d ep%u %s %s %s\r\n",
           slot + 1, header.mPath.mEndpointId,
           cluster_label(header.mPath.mClusterId, clb, sizeof(clb)),
           event_label(header.mPath.mClusterId, header.mPath.mEventId, evb, sizeof(evb)),
           val);

    /* Hub-mediated binding: relay a Generic Switch button event to a bound
     * target. Runs on the CHIP task (same as this callback), so it may originate
     * the outgoing command directly. No-op unless a rule matches. */
    uint8_t press_count = 0;
    if (header.mPath.mClusterId == 0x003BUL && header.mPath.mEventId == 0x06UL)  /* Switch MultiPressComplete */
        press_count = switch_multipress_count(data);
    bindings_on_event(node_id, header.mPath.mEndpointId, header.mPath.mClusterId, header.mPath.mEventId, press_count);
    /* Cache the last button press so `dash` can surface it. Cheap, no-op for
     * non-Switch events. */
    dash_on_event(node_id, header.mPath.mEndpointId, header.mPath.mClusterId, header.mPath.mEventId);
}

static void start_subscription_for(uint64_t node_id)
{
    using chip::app::AttributePathParams;
    using chip::app::EventPathParams;
    using chip::Platform::ScopedMemoryBufferWithSize;

    int bslot = dev_slot_by_node(node_id);
    if (bslot >= 0) {
        if (!g_dev[bslot].has_data) g_dev[bslot].last_report_us = esp_timer_get_time();
        g_dev[bslot].last_sub_attempt_us = esp_timer_get_time();   /* paces the retry loop */
    }

    ScopedMemoryBufferWithSize<AttributePathParams> attr_paths;
    ScopedMemoryBufferWithSize<EventPathParams>     event_paths;
    /* Path 0 = liveness (Basic Info). Paths 1..N = the logic engine's fixed set
     * of measurement attributes (wildcard endpoint), so sensor values stream in
     * on this one subscription and value-triggered rules work without ever
     * re-subscribing. A device lacking a cluster simply never reports it - the
     * extra wildcard-endpoint paths cost nothing on a plug (FOUNDATION §4/§11:
     * additive, one subscription per device). */
    /* Collect the extra (sensor + battery) paths this device actually needs. Once
     * a device has been enumerated its cap mask is known, so a plug carries none
     * of these (just liveness + events) - lighter subscription, less heap during
     * the reboot enumeration burst. Before the first enumeration the caps are
     * unknown and the full set is used, so sensor data always flows. */
    int  slot0 = dev_slot_by_node(node_id);
    uint32_t xcl[16], xat[16]; int nx = 0;
    for (int i = 0, n = logic_sensor_path_count(); i < n; ++i) {
        uint32_t cl = 0, at = 0; logic_sensor_path(i, &cl, &at);
        if (devcap_wants(slot0, cl)) { xcl[nx] = cl; xat[nx] = at; ++nx; }
    }
    for (int j = 0, n = dash_sub_path_count(); j < n; ++j) {   /* battery, for the dash fallback */
        uint32_t cl = 0, at = 0; dash_sub_path(j, &cl, &at);
        if (devcap_wants(slot0, cl)) { xcl[nx] = cl; xat[nx] = at; ++nx; }
    }
    if (!attr_paths.Alloc(1 + nx)) { ESP_LOGE(TAG, "OOM: sub path"); return; }
    attr_paths[0] = AttributePathParams((uint16_t)0, (uint32_t)CL_BASIC_INFO, (uint32_t)0x0000);
    for (int k = 0; k < nx; ++k) attr_paths[1 + k] = AttributePathParams(xcl[k], xat[k]);   /* wildcard endpoint */
    /* Wildcard event path (a default-constructed EventPathParams is all-wildcard):
     * catches Generic Switch button presses and any other device events. Carried
     * on the SAME subscription as the liveness attribute - one subscription per
     * device, which is what the Matter IM is designed for. */
    if (!event_paths.Alloc(1)) { ESP_LOGE(TAG, "OOM: event path"); return; }
    event_paths[0] = EventPathParams();
    /* URGENT is essential, not cosmetic: a non-urgent event just sits in the
     * device's event log until its next scheduled report. A sleepy ICD (e.g. the
     * BILRESA button) negotiates very long intervals to save battery, so a button
     * press could take minutes to surface - or never. Marking the path urgent
     * tells the device to wake into active mode and push the report immediately,
     * which is exactly what a button press needs. */
    event_paths[0].mIsUrgentEvent = true;

    auto *cmd = chip::Platform::New<esp_matter::controller::subscribe_command>(
        node_id, std::move(attr_paths), std::move(event_paths),
        (uint16_t)SUB_MIN_S, (uint16_t)SUB_MAX_S, /*auto_resubscribe*/ true,
        liveness_attribute_cb, event_report_cb, subscribe_done_cb, subscribe_failure_cb,
        /*keep_subscription*/ false);
    if (!cmd) { ESP_LOGE(TAG, "OOM: subscribe_command"); return; }
    if (cmd->send_command() != ESP_OK) { ESP_LOGE(TAG, "subscribe send failed"); chip::Platform::Delete(cmd); }
    else ESP_LOGI(TAG, "Subscription sent to node 0x%016" PRIx64, node_id);
}

static void start_all_subscriptions(void)
{
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (!g_dev[i].paired) continue;
        ESP_LOGI(TAG, "Subscribing #%d node=0x%016" PRIx64 " (heap free=%u)",
                 i + 1, g_dev[i].node_id, (unsigned)esp_get_free_heap_size());
        start_subscription_for(g_dev[i].node_id);
    }
}

static void subscribe_work(intptr_t context)
{
    if (context == 0) { start_all_subscriptions(); return; }
    int slot = (int)context - 1;
    if (slot >= 0 && slot < MAX_DEVICES) start_subscription_for(g_dev[slot].node_id);
}

/* ICD check-in -> re-subscribe. THIS is what makes a battery device survive a
 * hub reboot / power cut with NO user action.
 *
 * A LIT-ICD (measured on the IKEA BILRESA: IdleModeDuration=900 s,
 * ActiveModeDuration=1 s) is unreachable ~99.9% of the time, and its own
 * UserActiveModeTriggerInstruction says the wake gesture is the RESET button -
 * so neither polling nor pressing the normal buttons can bring it back. Expecting
 * an end user to poke a reset button after every power cut is not a product.
 *
 * Matter's answer is the Check-In Protocol: the device registers us as a check-in
 * client at commissioning (esp-matter does this automatically - the device's
 * RegisteredClients contains our node id) and then contacts US when it wakes.
 * That check-in is the one reachable moment, so we re-subscribe here.
 * esp-matter's stock DefaultCheckInDelegate only LOGS it, which is why a sleepy
 * device never came back; we subclass it. ActiveModeThreshold (5 s on the
 * BILRESA) keeps the device awake once we start talking, so the subscribe lands. */
class HubCheckInDelegate : public chip::app::DefaultCheckInDelegate {
public:
    void OnCheckInComplete(const chip::app::ICDClientInfo &clientInfo) override
    {
        chip::app::DefaultCheckInDelegate::OnCheckInComplete(clientInfo);   /* keep upstream logging */
        uint64_t node = clientInfo.peer_node.GetNodeId();
        int slot = dev_slot_by_node(node);
        if (slot < 0) return;
        if (g_dev[slot].has_data) return;      /* already subscribed and reporting */
        ESP_LOGW(TAG, "ICD check-in from #%d node=0x%016" PRIx64 " - re-subscribing inside its "
                 "active window", slot + 1, node);
        start_subscription_for(node);          /* already on the CHIP task here */
    }
};
static HubCheckInDelegate s_check_in_delegate;

/* Retry the INITIAL subscribe for any paired device that has never reported.
 *
 * Needed for sleepy / ICD devices (e.g. an IKEA BILRESA button): they keep the
 * radio off and only wake on a button press or a slow internal poll, so the
 * subscribe attempted at boot just times out. esp-matter's auto_resubscribe
 * does NOT cover this - it only re-subscribes a subscription that was already
 * established; a failed INITIAL CASE connection is reported once and dropped.
 * Without this the device would stay unreachable forever and its button events
 * would never be delivered.
 *
 * Cadence is deliberately close to how long a CASE attempt takes to fail, so an
 * attempt is almost always in flight - that way a user's button press (which
 * wakes the device for only a few seconds) lands inside an attempt's
 * retransmission window instead of falling between tries. */
/* Cadence: deliberately SLOW.
 *
 * A fast retry was tried (5 s, to try to catch a sleepy device's brief wake
 * window) and it was a mistake on both counts: CHIP coalesces concurrent session
 * attempts to the same peer so the extra tries do nothing, and for a device that
 * IS alive but slow to report it stacks duplicate subscriptions which then die
 * with "Subscription Liveness timeout". This loop exists only as a safety net to
 * recover a device that missed its one initial attempt - not as a way to chase a
 * sleeping radio (that needs ICD check-in registration, not polling). */
#define SUB_RETRY_MS      60000
#define SUB_ATTEMPT_GAP_US (55LL * 1000 * 1000)
static void subscribe_retry_timer(chip::System::Layer *, void *)
{
    for (int i = 0; i < MAX_DEVICES; ++i) {
        DeviceInfo &d = g_dev[i];
        if (!d.paired || d.has_data) continue;    /* has_data == it reported == subscribed */
        int64_t now = esp_timer_get_time();
        if (d.last_sub_attempt_us && (now - d.last_sub_attempt_us) < SUB_ATTEMPT_GAP_US) continue;
        ESP_LOGI(TAG, "Retry subscribe #%d node=0x%016" PRIx64 " (asleep/unreachable?) - "
                 "press its button to wake it", i + 1, d.node_id);
        start_subscription_for(d.node_id);   /* records last_sub_attempt_us */
    }
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(SUB_RETRY_MS), subscribe_retry_timer, nullptr);
}

/* ===================================================================== */
/*  Commissioning (sacred - ported verbatim from ALPSTUGA, generalized)    */
/* ===================================================================== */
static void reboot_to_operating(chip::System::Layer *, void *)
{
    ESP_LOGW(TAG, "Rebooting into operating mode (BLE->Thread)");
    esp_restart();
}

/* Commissioning watchdog (FOUNDATION §5): a commissioning attempt can hang with
 * NO success/failure callback and then block every future attempt with
 * ESP_ERR_INVALID_STATE - there is no cancel/stop API, only unpair_device. So we
 * arm a deadline when a commission starts; if nothing lands in time, reboot to
 * clear the commissioner and never get wedged in commissioning mode. Sized well
 * above a worst-case legitimate commission (~20 s typical; a slow sleepy-device
 * operational discovery can take a couple of minutes) so it never aborts a good
 * pairing that is merely slow. Tunable. */
#define COMMISSION_WATCHDOG_MS 240000
static void commission_watchdog_fire(chip::System::Layer *, void *)
{
    ESP_LOGE(TAG, "Commissioning watchdog: no result in %d s - rebooting to clear "
                  "the commissioner", COMMISSION_WATCHDOG_MS / 1000);
    esp_restart();
}

/* Batch-only fast skip: a device that never advertises never reaches PASE and never
 * fires the failure callback, so it would otherwise burn the full 240 s watchdog per
 * attempt. In a batch, if PASE hasn't landed within this deadline, reboot early so
 * the next boot retries/skips. A present, advertising device reaches PASE in seconds,
 * so this never aborts a good pairing. Not armed for the single-device flow. */
#define PAIRQ_PASE_DEADLINE_MS 60000
static volatile bool s_batch_pase_seen = false;
static void batch_pase_deadline_fire(chip::System::Layer *, void *)
{
    if (s_batch_pase_seen) return;   /* PASE landed - let the normal flow continue */
    ESP_LOGW(TAG, "Batch: no PASE within %d s - device not found, skipping (reboot)",
             PAIRQ_PASE_DEADLINE_MS / 1000);
    esp_restart();
}

static void on_commissioning_success(chip::ScopedNodeId peer_id)
{
    /* esp_matter passes ScopedNodeId(fabricIndex, nodeId); GetNodeId() returns
     * the fabric index, not the device node id. g_node_id holds the value we
     * passed to pairing_code_thread - use that. */
    ESP_LOGI(TAG, "Commissioned! (cb fabric=%u; keeping node=0x%016" PRIx64 ")",
             (unsigned)peer_id.GetFabricIndex(), g_node_id);

    /* A result landed - disarm the watchdog (we reboot to operating below anyway). */
    chip::DeviceLayer::SystemLayer().CancelTimer(commission_watchdog_fire, nullptr);
    status_led_set_state(LED_OPERATING);   /* paired OK -> green feedback */

    int slot = (g_staging_slot >= 0) ? g_staging_slot : dev_first_free_slot();
    if (slot < 0) slot = 0;
    g_dev[slot].paired  = true;
    g_dev[slot].node_id = g_node_id;
    strncpy(g_dev[slot].payload, g_payload, sizeof(g_dev[slot].payload) - 1);
    g_dev[slot].payload[sizeof(g_dev[slot].payload) - 1] = '\0';
    g_commission_mode = false;

    /* Batch pairing: record THIS entry as paired and advance the cursor. The queue
     * (still non-empty) makes the next boot re-enter commissioning for the next
     * entry; when it is exhausted the boot block prints the report and goes
     * operating. No-op for the single-device flow (empty queue). */
    if (batch_active()) {
        g_pairq.e[g_pairq.cur].status = PQ_PAIRED;
        g_pairq.e[g_pairq.cur].slot   = (uint8_t)slot;
        g_pairq.cur++; g_pairq.attempts = 0;
    }

    nvs_save();
    xEventGroupSetBits(s_evt, EVT_COMMISSIONED);

    /* Subscribe immediately to establish CASE before the device's post-commission
     * StayActive window expires (FOUNDATION §5.6). */
    chip::DeviceLayer::PlatformMgr().ScheduleWork(subscribe_work, (intptr_t)(slot + 1));

    ESP_LOGI(TAG, "Device #%d paired - rebooting into operating mode in 30s", slot + 1);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(30000), reboot_to_operating, nullptr);
}

static void on_commissioning_failure(chip::ScopedNodeId, CHIP_ERROR error,
                                     chip::Controller::CommissioningStage stage,
                                     std::optional<chip::Credentials::AttestationVerificationResult>)
{
    maybe_resume_thread();
    ESP_LOGE(TAG, "Commissioning failed: %" CHIP_ERROR_FORMAT " at stage %d", error.Format(), (int)stage);
    /* Batch pairing: a failure is terminal for this attempt (the commissioner may be
     * wedged - reboot is the established recovery). Reboot soon so boot retries this
     * entry (attempts++) or skips it, instead of waiting the 240 s watchdog. The
     * watchdog stays armed as the backstop for a true callback-less hang. No effect
     * on the single-device flow (batch_active() false). */
    if (batch_active())
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(10000), reboot_to_operating, nullptr);
}

static void commission_work(intptr_t)
{
    uint8_t ds[OT_OPERATIONAL_DATASET_MAX_LENGTH] = {};
    int ds_len = thread_dataset_bin(ds, sizeof(ds));
    if (ds_len <= 0) { ESP_LOGE(TAG, "No Thread dataset - cannot commission"); return; }

    esp_matter::controller::pairing_command_callbacks_t cbs = {
        .pase_callback = [](CHIP_ERROR err) {
            if (err == CHIP_NO_ERROR) {
                s_batch_pase_seen = true;   /* disarms the batch PASE deadline */
                ESP_LOGI(TAG, "PASE established - device will join Thread");
                /* Peer-hint polls covering the Thread-join + operational-discovery
                 * window (unique appState so CHIP doesn't dedup the timers). */
                static const uint32_t kDelays[] = {15000,30000,45000,60000,90000,120000,180000,240000,300000};
                for (uintptr_t i = 0; i < sizeof(kDelays)/sizeof(kDelays[0]); ++i)
                    chip::DeviceLayer::SystemLayer().StartTimer(
                        chip::System::Clock::Milliseconds32(kDelays[i]), scan_thread_peers, (void *)(i + 1));
            } else {
                ESP_LOGE(TAG, "PASE failed: %" CHIP_ERROR_FORMAT, err.Format());
            }
        },
        .commissioning_success_callback = on_commissioning_success,
        .commissioning_failure_callback = on_commissioning_failure,
    };
    esp_matter::controller::pairing_command::get_instance().set_callbacks(cbs);

    /* Arm the commissioning watchdog before we kick off - it covers a callback-less
     * hang (and a re-fired 'pair' just resets the deadline via StartTimer dedup). */
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(COMMISSION_WATCHDOG_MS),
        commission_watchdog_fire, nullptr);

    /* Batch only: a shorter PASE deadline so an absent device skips in ~1 min
     * instead of burning the 240 s watchdog (it never reaches PASE, so no failure
     * callback fires). Disarmed the moment PASE lands. */
    if (batch_active()) {
        s_batch_pase_seen = false;
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(PAIRQ_PASE_DEADLINE_MS),
            batch_pase_deadline_fire, nullptr);
    }

    esp_err_t err;
    if (g_payload[0] != '\0') {
        ESP_LOGI(TAG, "Commissioning node=0x%016" PRIx64 " payload=%s", g_node_id, g_payload);
        err = esp_matter::controller::pairing_command::pairing_code_thread(
            g_node_id, g_payload, ds, (uint8_t)ds_len);
        if (err != ESP_OK) ESP_LOGE(TAG, "pairing_code_thread failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Commissioning node=0x%016" PRIx64 " PIN=%" PRIu32 " disc=%u", g_node_id, g_pin, g_disc);
        err = esp_matter::controller::pairing_command::pairing_ble_thread(
            g_node_id, g_pin, g_disc, ds, (uint8_t)ds_len);
        if (err != ESP_OK) ESP_LOGE(TAG, "pairing_ble_thread failed: %s", esp_err_to_name(err));
    }
}

static void do_commission(void)
{
    if (g_paired) {
        printf("[hub] In operating mode (BLE off). Type 'pair' to reboot into "
               "commissioning and add a device.\r\n");
        return;
    }
    int slot = dev_first_free_slot();
    if (slot < 0) { printf("[hub] All %d slots in use - 'remove <n>' first.\r\n", MAX_DEVICES); return; }
    g_staging_slot = slot;
    g_node_id      = slot_node_id(slot);
    if (!(xEventGroupGetBits(s_evt) & EVT_THREAD_READY)) {
        ESP_LOGW(TAG, "Thread not ready yet - wait a few seconds and retry");
        return;
    }
    printf("[hub] Commissioning into slot #%d (node=0x%016" PRIx64 ")...\r\n", slot + 1, g_node_id);
    CHIP_ERROR ce = chip::DeviceLayer::PlatformMgr().ScheduleWork(commission_work, 0);
    if (ce != CHIP_NO_ERROR) ESP_LOGE(TAG, "ScheduleWork(commission) failed: %" CHIP_ERROR_FORMAT, ce.Format());
}

/* ===================================================================== */
/*  Decommission (proper removal - FOUNDATION §6)                          */
/* ===================================================================== */
static uint64_t s_decommission_node = 0;
static void decommission_work(intptr_t)
{
    uint64_t node = s_decommission_node;
    if (!node) return;
    ESP_LOGW(TAG, "Decommissioning node=0x%016" PRIx64 " (RemoveFabric + ShutdownSubscriptions)", node);
    esp_matter::controller::unpair_device(node);
    chip::app::InteractionModelEngine::GetInstance()->ShutdownSubscriptions((chip::FabricIndex)1, node);
}

/* ===================================================================== */
/*  Button                                                                 */
/* ===================================================================== */
static TickType_t s_press_tick = 0;
static void IRAM_ATTR btn_isr(void *)
{
    BaseType_t woken = pdFALSE;
    bool pressed = (gpio_get_level((gpio_num_t)BUTTON_PIN) == 0);
    if (pressed) s_press_tick = xTaskGetTickCountFromISR();
    else {
        TickType_t held = xTaskGetTickCountFromISR() - s_press_tick;
        EventBits_t bit = (held >= pdMS_TO_TICKS(LONG_PRESS_MS)) ? EVT_LONG : EVT_SHORT;
        xEventGroupSetBitsFromISR(s_evt, bit, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* ===================================================================== */
/*  Time (host-provided; CASE cert validity - FOUNDATION §8)               */
/* ===================================================================== */
static time_t utc_to_unix_ts(int Y, int Mo, int D, int H, int Mi, int S)
{
    static const int kDIM[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t days = 0;
    for (int y = 1970; y < Y; ++y) days += (y%4==0 && (y%100!=0||y%400==0)) ? 366 : 365;
    for (int m = 1; m < Mo; ++m) days += kDIM[m-1] + (m==2 && (Y%4==0 && (Y%100!=0||Y%400==0)) ? 1 : 0);
    days += D - 1;
    return (time_t)(days * 86400LL + H * 3600LL + Mi * 60LL + S);
}

/* ===================================================================== */
/*  Batch pairing helpers                                                  */
/* ===================================================================== */
/* Printed as the hub settles back to operating after the last queued device. */
static void pairq_print_report(void)
{
    int paired = 0, failed = 0;
    for (int i = 0; i < g_pairq.n; ++i) {
        if (g_pairq.e[i].status == PQ_PAIRED)      ++paired;
        else if (g_pairq.e[i].status == PQ_FAILED) ++failed;
    }
    printf("[hub] === Batch pairing complete: %d paired, %d could not pair ===\r\n", paired, failed);
    for (int i = 0; i < g_pairq.n; ++i) {
        pairq_entry_t &e = g_pairq.e[i];
        if (e.status == PQ_PAIRED) {
            char nm[72];
            printf("  PAIRED  #%d  %s  (%s)\r\n", e.slot + 1, e.code,
                   dev_display_name(g_dev[e.slot], nm, sizeof(nm)));
        } else {
            printf("  FAILED  --  %s   (not found / commissioning failed after %d tries)\r\n",
                   e.code, PAIRQ_MAX_ATTEMPTS);
        }
    }
}

static void pairq_print_status(void)
{
    printf("[hub] pair-queue: %u entr%s\r\n", g_pairq.n, g_pairq.n == 1 ? "y" : "ies");
    for (int i = 0; i < g_pairq.n; ++i) {
        const char *st = g_pairq.e[i].status == PQ_PAIRED ? "PAIRED"
                       : g_pairq.e[i].status == PQ_FAILED ? "FAILED"
                       : (i == g_pairq.cur ? "current" : "pending");
        printf("  [%d] %-8s %s\r\n", i, st, g_pairq.e[i].code);
    }
    if (!g_pairq.n) printf("  (empty - use 'pairlist <code1> <code2> ...')\r\n");
}

/* ===================================================================== */
/*  Config backup / restore  (JSON over the console)                       */
/* ===================================================================== */
/* Is a device with this onboarding code currently paired? (identity match) */
static int slot_of_payload(const char *payload)
{
    if (!payload || !payload[0]) return -1;
    for (int i = 0; i < MAX_DEVICES; ++i)
        if (g_dev[i].paired && !strcmp(g_dev[i].payload, payload)) return i;
    return -1;
}

/* `backup` - print the whole hub config as readable JSON. Devices are listed in
 * order (index = identity); bindings/logic reference those indices. */
/* Build the whole config as a cJSON object (devices/bindings/logic/schedule).
 * Shared by the console `backup` and the USB JSON protocol. Caller owns it. */
cJSON *hub_backup_json(void)
{
    int slot2idx[MAX_DEVICES];
    for (int i = 0; i < MAX_DEVICES; ++i) slot2idx[i] = -1;

    cJSON *root = cJSON_CreateObject();
    if (!root) return nullptr;
    cJSON_AddNumberToObject(root, "mh_backup", 1);
    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    int idx = 0;
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (!g_dev[i].paired) continue;
        slot2idx[i] = idx++;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "payload", g_dev[i].payload);
        cJSON_AddNumberToObject(d, "vid", g_dev[i].vid);
        cJSON_AddNumberToObject(d, "pid", g_dev[i].pid);
        cJSON_AddStringToObject(d, "name", g_dev[i].name);
        cJSON_AddStringToObject(d, "label", g_dev[i].label);
        cJSON_AddItemToArray(devs, d);
    }
    cJSON_AddItemToObject(root, "bindings", bindings_to_json(slot2idx));
    cJSON_AddItemToObject(root, "logic",    logic_to_json(slot2idx));
    cJSON_AddItemToObject(root, "schedule", schedule_to_json());   /* schedulers + calendar + tz */
    return root;
}

static void cmd_backup(void)
{
    cJSON *root = hub_backup_json();
    if (!root) { printf("[hub] backup: out of memory\r\n"); return; }
    char *js = cJSON_PrintUnformatted(root);   /* compact single line - easy to copy/paste */
    cJSON_Delete(root);
    if (!js) { printf("[hub] backup: out of memory\r\n"); return; }
    printf("[hub] --- BEGIN backup (copy the single line between the markers) ---\r\n");
    printf("%s\r\n", js);
    printf("[hub] --- END backup ---  (restore with 'restore', paste, then a line '.')\r\n");
    cJSON_free(js);
}

static bool restore_pending(void)
{
    nvs_handle_t h; if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = 0; bool present = (nvs_get_blob(h, K_RESTOREJ, nullptr, &sz) == ESP_OK && sz > 0);
    nvs_close(h);
    return present;
}
static void restore_clear(void)
{
    nvs_handle_t h; if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, K_RESTOREJ); nvs_commit(h); nvs_close(h);
}

/* Phase 2: after any needed re-pairing, resolve backup device indices to current
 * slots by payload and apply labels/bindings/logic; report what could not be
 * restored (a rule whose device did not re-pair). Runs from the boot block. */
static void restore_finalize(void)
{
    nvs_handle_t h; if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = 0;
    if (nvs_get_blob(h, K_RESTOREJ, nullptr, &sz) != ESP_OK || sz == 0) { nvs_close(h); return; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { nvs_close(h); return; }
    nvs_get_blob(h, K_RESTOREJ, buf, &sz); buf[sz] = '\0';
    nvs_close(h);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGE(TAG, "restore: stored JSON unparseable - discarding"); restore_clear(); return; }

    cJSON *devs = cJSON_GetObjectItem(root, "devices");
    int n_dev = cJSON_GetArraySize(devs);
    if (n_dev > MAX_DEVICES) n_dev = MAX_DEVICES;

    int         idx2slot[MAX_DEVICES];
    const char *idx_name[MAX_DEVICES];
    int resolved = 0, lbl = 0, i = 0;
    cJSON *d;
    cJSON_ArrayForEach(d, devs) {
        if (i >= MAX_DEVICES) break;
        const char *pl = cJSON_GetStringValue(cJSON_GetObjectItem(d, "payload"));
        const char *lb = cJSON_GetStringValue(cJSON_GetObjectItem(d, "label"));
        const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(d, "name"));
        idx_name[i] = (lb && lb[0]) ? lb : (nm && nm[0]) ? nm : (pl ? pl : "?");
        int slot = slot_of_payload(pl);
        idx2slot[i] = slot;
        if (slot >= 0) {
            ++resolved;
            strncpy(g_dev[slot].label, lb ? lb : "", sizeof(g_dev[slot].label) - 1);
            g_dev[slot].label[sizeof(g_dev[slot].label) - 1] = '\0';
            if (lb && lb[0]) ++lbl;
        }
        ++i;
    }
    n_dev = i;

    restore_map m = { idx2slot, idx_name, n_dev };
    printf("[hub] === Restore: applying config (%d/%d backup devices present) ===\r\n", resolved, n_dev);
    int b_ap = 0, b_sk = 0, l_ap = 0, l_sk = 0;
    bindings_apply_json(cJSON_GetObjectItem(root, "bindings"), &m, &b_ap, &b_sk);
    logic_apply_json(cJSON_GetObjectItem(root, "logic"),    &m, &l_ap, &l_sk);
    schedule_apply_json(cJSON_GetObjectItem(root, "schedule"));   /* device-independent */
    nvs_save();   /* persist restored labels */

    /* If any device-referencing binding/logic rule was skipped (its device isn't
     * paired yet), keep the backup STASHED instead of discarding it. restore_finalize
     * re-runs on every boot (and each commission reboots the hub), so those rules are
     * re-applied automatically as the missing devices re-commission - nothing is lost.
     * Cleared only when everything resolves, or via 'restore cancel'. */
    bool pending = (b_sk > 0 || l_sk > 0);
    printf("[hub] Restore %s: labels %d, bindings %d applied / %d skipped, "
           "logic %d applied / %d skipped.%s\r\n",
           pending ? "PARTIAL" : "complete",
           lbl, b_ap, b_sk, l_ap, l_sk,
           pending ? "  (kept PENDING: rules re-apply as the missing device(s) re-commission; "
                     "'restore cancel' - or the portal - to drop it)"
                   : "");

    cJSON_Delete(root);
    if (!pending) restore_clear();
}

/* Public wrappers for the console/USB-JSON layer. */
bool hub_restore_pending(void) { return restore_pending(); }
void hub_restore_cancel(void)  { restore_clear(); }

/* `restore` - read pasted JSON from the console, stash it, and queue re-pairing of
 * any devices not currently paired; the deferred restore_finalize() (boot block)
 * applies the config once they are back.
 *
 * `wipe` = true ('restore wipe'): first factory-erase NVS (all devices, bindings,
 * logic, labels AND the Matter fabric), then re-pair EVERY backup device from
 * scratch and apply only the backup's config - so the hub ends up matching the
 * JSON exactly, with no orphaned devices/rules. The stashed JSON + seeded queue
 * are written back AFTER the erase so they survive it. */
/* Stage a restore from an ALREADY-PARSED backup object (used by the USB JSON
 * protocol; the console `restore` has its own paste loop). Serialises the object
 * for K_RESTOREJ, seeds the re-pair queue, optionally factory-wipes, then reboots.
 * Does not take ownership of `root`. Reboots on success (does not return). */
bool hub_stage_restore(const cJSON *root, bool wipe, char *err, size_t cap)
{
    if (!root || !cJSON_GetObjectItem(root, "mh_backup")) { if (err) snprintf(err, cap, "not a valid backup"); return false; }
    char *js = cJSON_PrintUnformatted((cJSON *)root);
    if (!js) { if (err) snprintf(err, cap, "out of memory"); return false; }
    size_t len = strlen(js);

    memset(&g_pairq, 0, sizeof(g_pairq));
    const cJSON *devs = cJSON_GetObjectItem(root, "devices"), *d;
    int need = 0;
    cJSON_ArrayForEach(d, devs) {
        const char *pl = cJSON_GetStringValue(cJSON_GetObjectItem(d, "payload"));
        if (!pl || !pl[0]) continue;
        if (!wipe && slot_of_payload(pl) >= 0) continue;
        if (g_pairq.n < PAIRQ_MAX && strlen(pl) < sizeof(g_pairq.e[0].code)) {
            strncpy(g_pairq.e[g_pairq.n].code, pl, sizeof(g_pairq.e[0].code) - 1);
            g_pairq.e[g_pairq.n].status = PQ_PENDING; ++g_pairq.n; ++need;
        }
    }

    if (wipe) {
        printf("[hub] WIPE restore (USB): erasing all config + Matter fabric, re-pairing %d device(s). Rebooting...\r\n", need);
        fflush(stdout); vTaskDelay(pdMS_TO_TICKS(200));
        nvs_flash_deinit(); nvs_flash_erase(); nvs_flash_init();
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, K_RESTOREJ, js, len + 1);
            nvs_set_blob(h, K_PAIRQ, &g_pairq, sizeof(g_pairq));
            nvs_commit(h); nvs_close(h);
        }
        esp_restart();
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) { nvs_set_blob(h, K_RESTOREJ, js, len + 1); nvs_commit(h); nvs_close(h); }
    nvs_save();
    cJSON_free(js);
    printf("[hub] Restore staged (USB): %d device(s) to re-pair. Rebooting...\r\n", need);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return true;
}

static void cmd_restore(bool wipe)
{
    printf("[hub] Paste the backup JSON, then a line containing only '.' to apply "
           "(or 'x' to cancel):\r\n");
    size_t cap = 2048, len = 0, line_start = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { printf("[hub] restore: out of memory\r\n"); return; }
    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (c == '\r') continue;
        if (len + 2 >= cap) {
            if (cap >= 16384) { printf("[hub] restore: input too large - aborted\r\n"); free(buf); return; }
            size_t nc = cap * 2; char *nb = (char *)realloc(buf, nc);
            if (!nb) { printf("[hub] restore: OOM\r\n"); free(buf); return; }
            buf = nb; cap = nc;
        }
        if (c == '\n') {
            if (len - line_start == 1 && (buf[line_start] == '.' || buf[line_start] == 'x')) {
                if (buf[line_start] == 'x') { printf("[hub] restore cancelled\r\n"); free(buf); return; }
                len = line_start; break;   /* '.' = end of paste */
            }
            buf[len++] = '\n'; line_start = len;
        } else {
            buf[len++] = (char)c;
        }
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root || !cJSON_GetObjectItem(root, "mh_backup")) {
        printf("[hub] restore: not a valid backup (missing/invalid JSON) - nothing changed\r\n");
        if (root) cJSON_Delete(root);
        free(buf);
        return;
    }

    /* Seed the re-pair queue. Normal restore: only devices not already paired.
     * Wipe restore: EVERY backup device (the fabric is about to be erased, so none
     * will be paired after the wipe). */
    memset(&g_pairq, 0, sizeof(g_pairq));
    cJSON *devs = cJSON_GetObjectItem(root, "devices"), *d;
    int need = 0, already = 0;
    cJSON_ArrayForEach(d, devs) {
        const char *pl = cJSON_GetStringValue(cJSON_GetObjectItem(d, "payload"));
        if (!pl || !pl[0]) continue;
        if (!wipe && slot_of_payload(pl) >= 0) { ++already; continue; }   /* same device: already paired */
        if (g_pairq.n < PAIRQ_MAX && strlen(pl) < sizeof(g_pairq.e[0].code)) {
            strncpy(g_pairq.e[g_pairq.n].code, pl, sizeof(g_pairq.e[0].code) - 1);
            g_pairq.e[g_pairq.n].status = PQ_PENDING; ++g_pairq.n; ++need;
        }
    }
    cJSON_Delete(root);

    if (wipe) {
        /* Factory-erase, then write the stashed JSON + seeded queue back into the
         * fresh NVS (so they survive the wipe). We deliberately do NOT nvs_save()
         * here - that would resurrect the old device table from RAM. On the next
         * boot nvs_load() sees a blank device table, the queue drives re-pairing,
         * and restore_finalize() applies only the backup's config. */
        printf("[hub] WIPE restore: erasing all devices, bindings, logic, labels and the "
               "Matter fabric, then re-pairing %d device(s) from the backup.\r\n", need);
        printf("[hub] (Each device must be in pairing mode to re-commission.) Rebooting...\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));   /* drain the message while NVS is still valid */
        /* deinit before erase (IDF requirement): a bare erase leaves the partition
         * marked initialised, so a following nvs_flash_init() would keep stale cached
         * state instead of re-reading the blanked flash. Then reboot immediately to
         * keep the post-erase window (CHIP handles now dangling) as short as possible. */
        nvs_flash_deinit();
        nvs_flash_erase();                /* full factory wipe: hub keys + CHIP fabric */
        nvs_flash_init();                 /* fresh, blank NVS re-read from flash */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, K_RESTOREJ, buf, len + 1);   /* survives the wipe */
            nvs_set_blob(h, K_PAIRQ, &g_pairq, sizeof(g_pairq));
            nvs_commit(h); nvs_close(h);
        }
        esp_restart();
    }

    /* Non-wipe: stash the raw JSON + seeded queue via the normal save path. */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, K_RESTOREJ, buf, len + 1); nvs_commit(h); nvs_close(h);
    }
    nvs_save();   /* persist the seeded pair-queue */
    free(buf);

    printf("[hub] Restore staged: %d device(s) already paired, %d to re-pair. Rebooting%s...\r\n",
           already, need, need ? " into commissioning" : " to apply config");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ===================================================================== */
/*  Console command dispatch                                               */
/* ===================================================================== */
static void handle_cmd(const char *line)
{
    /* Inspector commands first (devices/tree/ep/cluster/read/write/invoke/scan). */
    if (inspector_handle_cmd(line)) return;
    /* Binding commands (bind/bindings/unbind/bindable). */
    if (bindings_handle_cmd(line)) return;
    /* Value-triggered logic rules (logic add/rm/list). */
    if (logic_handle_cmd(line)) return;
    /* Live device overview (dash). */
    if (dash_handle_cmd(line)) return;
    /* Schedulers + shared calendar + timezone (sched/calendar/tz). */
    if (schedule_handle_cmd(line)) return;

    if (strcmp(line, "backup") == 0)       { cmd_backup();       return; }
    if (strcmp(line, "restore") == 0)      { cmd_restore(false); return; }
    if (strcmp(line, "restore wipe") == 0) { cmd_restore(true);  return; }
    if (strcmp(line, "restore cancel") == 0) {
        if (restore_pending()) { restore_clear(); printf("[hub] Pending restore cancelled.\r\n"); }
        else printf("[hub] No pending restore.\r\n");
        return;
    }

    uint32_t pin = 0; uint16_t disc = 0;

    if (strncmp(line, "payload ", 8) == 0) {
        const char *p = line + 8; while (*p == ' ') ++p;
        if (*p == '\0') { printf("[hub] Usage: payload <MT:...|pairing-code>\r\n"); return; }
        if (strlen(p) >= sizeof(g_payload)) { printf("[hub] payload too long\r\n"); return; }
        strncpy(g_payload, p, sizeof(g_payload) - 1); g_payload[sizeof(g_payload) - 1] = '\0';
        nvs_save();
        printf("[hub] Payload saved: %s  - type 'pair' to commission.\r\n", g_payload);

    } else if (strncmp(line, "name ", 5) == 0) {
        /* name <n> [label...] - set a user label to tell identical devices apart
         * (rest of line = label, spaces allowed); no label clears it. */
        const char *p = line + 5; while (*p == ' ') ++p;
        int n = atoi(p);
        if (n < 1 || n > MAX_DEVICES || !g_dev[n - 1].paired) {
            printf("[hub] Usage: name <n> [label]  (n = paired slot; empty label clears)\r\n"); return;
        }
        int slot = n - 1;
        while (*p && *p != ' ') ++p;          /* skip the slot number */
        while (*p == ' ') ++p;                /* to the label */
        if (*p == '\0') {
            g_dev[slot].label[0] = '\0';
            printf("[hub] #%d label cleared (shows device name)\r\n", n);
        } else {
            strncpy(g_dev[slot].label, p, sizeof(g_dev[slot].label) - 1);
            g_dev[slot].label[sizeof(g_dev[slot].label) - 1] = '\0';
            printf("[hub] #%d labeled \"%s\"\r\n", n, g_dev[slot].label);
        }
        nvs_save();

    } else if (sscanf(line, "pin %lu %hu", (unsigned long *)&pin, &disc) == 2) {
        if (disc > 4095) { printf("[hub] discriminator must be 0-4095\r\n"); return; }
        g_pin = pin; g_disc = disc; g_payload[0] = '\0'; nvs_save();
        printf("[hub] PIN=%" PRIu32 " disc=%u saved\r\n", pin, disc);

    } else if (strcmp(line, "pair") == 0) {
        if (!g_paired) {
            do_commission();
        } else if (dev_first_free_slot() < 0) {
            printf("[hub] All %d slots in use - 'remove <n>' first.\r\n", MAX_DEVICES);
        } else {
            printf("[hub] Rebooting into commissioning mode (slot #%d)...\r\n", dev_first_free_slot() + 1);
            g_commission_mode = true; nvs_save();
            vTaskDelay(pdMS_TO_TICKS(500)); esp_restart();
        }

    } else if (strncmp(line, "pairlist", 8) == 0 && (line[8] == '\0' || line[8] == ' ')) {
        /* Batch pairing: queue several payloads; the hub commissions them one per
         * reboot-isolated session (each device found by its own discriminator over
         * BLE), then prints a paired/failed report. 'pairlist' shows the queue,
         * 'pairlist clear' empties it. */
        const char *p = line + 8; while (*p == ' ') ++p;
        if (*p == '\0') { pairq_print_status(); return; }
        if (strcmp(p, "clear") == 0) {
            memset(&g_pairq, 0, sizeof(g_pairq)); nvs_save();
            printf("[hub] pair-queue cleared\r\n"); return;
        }
        int freeslots = 0;
        for (int i = 0; i < MAX_DEVICES; ++i) if (!g_dev[i].paired) ++freeslots;
        pairq_t q; memset(&q, 0, sizeof(q));
        char buf[600]; strncpy(buf, p, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
        for (char *t = strtok(buf, " ,"); t; t = strtok(nullptr, " ,")) {
            if (q.n >= PAIRQ_MAX) { printf("[hub] too many payloads (max %d)\r\n", PAIRQ_MAX); return; }
            if (strlen(t) >= sizeof(q.e[0].code)) { printf("[hub] payload too long: %s\r\n", t); return; }
            strncpy(q.e[q.n].code, t, sizeof(q.e[q.n].code) - 1);
            q.e[q.n].status = PQ_PENDING; ++q.n;
        }
        if (q.n == 0) { printf("[hub] Usage: pairlist <code1> <code2> ...   |   pairlist [clear]\r\n"); return; }
        if (q.n > freeslots) {
            printf("[hub] %u payload(s) but only %d free slot(s) - 'remove <n>' first or shorten the list\r\n",
                   q.n, freeslots); return;
        }
        g_pairq = q; nvs_save();
        printf("[hub] Queued %u device(s) for batch pairing - rebooting into commissioning...\r\n", q.n);
        vTaskDelay(pdMS_TO_TICKS(500)); esp_restart();

    } else if (strncmp(line, "remove ", 7) == 0 || strncmp(line, "unpair ", 7) == 0) {
        const char *p = line + 7; while (*p == ' ') ++p;
        int n = atoi(p);
        if (n < 1 || n > MAX_DEVICES || !g_dev[n - 1].paired) {
            printf("[hub] Usage: remove <n> (a paired slot - see 'devices').\r\n"); return;
        }
        int slot = n - 1;
        printf("[hub] Removing #%d (node=0x%016" PRIx64 "): decommissioning then rebooting...\r\n",
               n, g_dev[slot].node_id);
        /* Proper decommission (FOUNDATION §6): send RemoveFabric + shut down the
         * subscription BEFORE dropping the local record, else the device stays
         * bound to our fabric and orphaned on Thread. Give it time to go out. */
        s_decommission_node = g_dev[slot].node_id;
        chip::DeviceLayer::PlatformMgr().ScheduleWork(decommission_work, 0);
        vTaskDelay(pdMS_TO_TICKS(3000));
        g_dev[slot].paired = false; g_dev[slot].eui64_known = false;
        g_dev[slot].has_data = false; g_dev[slot].payload[0] = '\0';
        g_dev[slot].label[0] = '\0';   /* forget user label; slot may be re-paired */
        g_devcap[slot] = 0;      /* forget caps; a re-paired device may differ */
        nvs_save();
        vTaskDelay(pdMS_TO_TICKS(300)); esp_restart();

    } else if (strncmp(line, "blescan", 7) == 0) {
        /* Passive BLE scan for devices advertising as commissionable. Gives the
         * discriminator + VID/PID; the passcode is never advertised (see ble_scan.h). */
        const char *p = line + 7; while (*p == ' ') ++p;
        int secs = (*p != '\0') ? atoi(p) : 5;
        ble_scan_start((uint16_t)secs);

    } else if (strcmp(line, "reset") == 0) {
        printf("[hub] Factory reset (erasing NVS) + restart...\r\n");
        vTaskDelay(pdMS_TO_TICKS(150));
        nvs_flash_erase(); esp_restart();

    } else if (strcmp(line, "status") == 0) {
        EventBits_t b = xEventGroupGetBits(s_evt);
        printf("[hub] devices=%d/%d  thread=%s  radio=%s\r\n", g_dev_count, MAX_DEVICES,
               (b & EVT_THREAD_READY) ? "ready" : "starting",
               g_paired ? "operating (Thread, BLE off)" : "BLE commissioning");
        if (g_payload[0] != '\0') printf("  next-pair: payload=%s\r\n", g_payload);
        else                      printf("  next-pair: pin=%" PRIu32 " disc=%u\r\n", g_pin, g_disc);
        printf("  debug: %s (persisted)\r\n", g_debug_verbose ? "ON" : "off");
        if (g_pairq.n > 0) {
            int p = 0, f = 0;
            for (int i = 0; i < g_pairq.n; ++i) {
                if (g_pairq.e[i].status == PQ_PAIRED) ++p; else if (g_pairq.e[i].status == PQ_FAILED) ++f;
            }
            printf("  pair-queue: %u/%u in progress (attempt %u); %d paired, %d failed so far\r\n",
                   g_pairq.cur < g_pairq.n ? g_pairq.cur + 1 : g_pairq.n, g_pairq.n, g_pairq.attempts, p, f);
        }
        inspector_print_devices();

    } else if (strncmp(line, "settime ", 8) == 0) {
        /* The typed time is LOCAL wall time. An optional trailing tz offset
         * (+HH:MM / -HH:MM / Z) also SETS the hub timezone; without one the
         * current tz is used. UTC (stored in the system clock) = local - tz. */
        const char *p = line + 8; while (*p == ' ') ++p;
        int Y=0,Mo=0,D=0,H=0,Mi=0,S=0; char tzs[16] = {0};
        int nf = sscanf(p, "%d-%d-%d %d:%d:%d %15s", &Y,&Mo,&D,&H,&Mi,&S, tzs);
        if (nf < 6) { tzs[0]=0; nf = sscanf(p, "%d-%d-%dT%d:%d:%d%15s", &Y,&Mo,&D,&H,&Mi,&S, tzs); }
        if (nf < 6) {
            printf("[hub] Usage: settime YYYY-MM-DD HH:MM:SS [+HH:MM]  (local; add offset to set tz)\r\n"); return;
        }
        if (Y < 2000 || Mo<1||Mo>12||D<1||D>31||H<0||H>23||Mi<0||Mi>59||S<0||S>59) {
            printf("[hub] Invalid date/time\r\n"); return;
        }
        if (nf >= 7 && tzs[0]) {   /* parse + apply the timezone offset */
            if (tzs[0]=='Z' || tzs[0]=='z') schedule_set_tz(0);
            else if (tzs[0]=='+' || tzs[0]=='-') {
                int sg = (tzs[0]=='-') ? -1 : 1, th=0, tm=0;
                if (sscanf(tzs+1, "%d:%d", &th, &tm) >= 1) schedule_set_tz(sg*(th*60+tm));
            }
        }
        int tz = schedule_tz_offset_min();
        struct timeval tv = {}; tv.tv_sec = utc_to_unix_ts(Y,Mo,D,H,Mi,S) - (time_t)tz*60;
        settimeofday(&tv, nullptr);
        printf("[hub] Clock set: local %04d-%02d-%02d %02d:%02d:%02d  tz %+03d:%02d\r\n",
               Y,Mo,D,H,Mi,S, tz/60, abs(tz)%60);

    } else if (strncmp(line, "debug ", 6) == 0) {
        const char *p = line + 6; while (*p == ' ') ++p;
        bool set = true;
        if (strcmp(p, "on") == 0)  { g_debug_verbose = true;  apply_debug_level(true);  printf("[hub] Debug ON (persists across reboots)\r\n"); }
        else if (strcmp(p,"off")==0){ g_debug_verbose = false; apply_debug_level(false); printf("[hub] Debug OFF (persists across reboots)\r\n"); }
        else { printf("[hub] Usage: debug on | off\r\n"); set = false; }
        if (set) {   /* persist so the setting survives reboots (incl. batch pairing) */
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, K_DEBUG, g_debug_verbose ? 1 : 0); nvs_commit(h); nvs_close(h);
            }
        }

    } else if (strcmp(line, "heap") == 0) {
        /* RAM diagnostics: current/min-ever/largest-block heap + stack headroom
         * (high-water = MINIMUM ever free, in bytes) of our own tasks. */
        printf("[hub] heap free=%u  min-ever=%u  largest-block=%u\r\n",
               (unsigned)esp_get_free_heap_size(),
               (unsigned)esp_get_minimum_free_heap_size(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        if (s_serial_task_h)
            printf("  serial    stack min-free=%u B\r\n",
                   (unsigned)(uxTaskGetStackHighWaterMark(s_serial_task_h) * sizeof(StackType_t)));
        if (s_sub_task_h)
            printf("  subscribe stack min-free=%u B\r\n",
                   (unsigned)(uxTaskGetStackHighWaterMark(s_sub_task_h) * sizeof(StackType_t)));

    } else {
        printf("[hub] Core commands:\r\n"
               "  payload <MT:...|code>   set onboarding code for the NEXT device\r\n"
               "  pin <PIN> <disc>        set manual credentials (disc 0-4095)\r\n"
               "  blescan [secs]          scan BLE for devices in pairing mode (disc+VID/PID)\r\n"
               "  pair                    add a device (reboots into commissioning)\r\n"
               "  pairlist <c1> <c2> ...  batch-pair several devices, then report which paired\r\n"
               "  remove <n>              decommission a device slot (reboots)\r\n"
               "  name <n> [label]        label a device (empty clears); shown in all lists\r\n"
               "  reset                   factory reset (erase NVS) + restart\r\n"
               "  status                  show current state\r\n"
               "  settime YYYY-MM-DD HH:MM:SS [+HH:MM]   set the clock (local; offset sets tz)\r\n"
               "  debug on|off            verbose log toggle\r\n"
               "  heap                    RAM diagnostics (heap + task stack headroom)\r\n"
               "  backup                  print the whole config (devices/bindings/logic/schedule) as JSON\r\n"
               "  restore                 paste a backup JSON to restore (re-pairs devices as needed)\r\n"
               "  restore wipe            factory-erase first, then restore exactly from the JSON\r\n"
               "Inspector commands:\r\n");
        inspector_print_help();
        printf("Binding commands:\r\n");
        bindings_print_help();
        printf("Logic commands:\r\n");
        logic_print_help();
        printf("Schedule commands:\r\n");
        schedule_print_help();
        dash_print_help();
    }
}

/* ===================================================================== */
/*  Command mutex + JSON-protocol entry points (v1.8)                      */
/* ===================================================================== */
static SemaphoreHandle_t s_cmd_mutex = nullptr;
void hub_cmd_lock(void)   { if (s_cmd_mutex) xSemaphoreTake(s_cmd_mutex, portMAX_DELAY); }
void hub_cmd_unlock(void) { if (s_cmd_mutex) xSemaphoreGive(s_cmd_mutex); }
/* Run a console line under the command mutex so the UART serial task serialises
 * with the USB comm task (both mutate g_dev / rule tables / NVS). */
void hub_console_exec(const char *line) { hub_cmd_lock(); handle_cmd(line); hub_cmd_unlock(); }

/* Non-rebooting global commands for the USB protocol: reuse the (validated)
 * console handlers. Called by comm.cpp with the command mutex already held. */
bool hub_action_json(const char *cmd, const cJSON *req, cJSON *result, char *err, size_t cap)
{
    char line[192];
    if (!strcmp(cmd, "name")) {
        int dev = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(req, "dev"));
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(req, "label"));
        snprintf(line, sizeof(line), "name %d %s", dev, label ? label : "");
        handle_cmd(line); return true;
    }
    if (!strcmp(cmd, "debug")) { handle_cmd(cJSON_IsTrue(cJSON_GetObjectItem(req, "on")) ? "debug on" : "debug off"); return true; }
    if (!strcmp(cmd, "payload")) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(req, "code"));
        if (!code) { snprintf(err, cap, "need 'code'"); return false; }
        snprintf(line, sizeof(line), "payload %s", code); handle_cmd(line); return true;
    }
    if (!strcmp(cmd, "pin")) {
        snprintf(line, sizeof(line), "pin %d %d", (int)cJSON_GetNumberValue(cJSON_GetObjectItem(req, "pin")),
                 (int)cJSON_GetNumberValue(cJSON_GetObjectItem(req, "disc")));
        handle_cmd(line); return true;
    }
    if (!strcmp(cmd, "settime")) {
        const char *iso = cJSON_GetStringValue(cJSON_GetObjectItem(req, "iso"));
        if (!iso) { snprintf(err, cap, "need 'iso'"); return false; }
        snprintf(line, sizeof(line), "settime %s", iso); handle_cmd(line); return true;
    }
    if (!strcmp(cmd, "scan")) {   /* re-enumerate a device (async; result via status/tree later) */
        int dev = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(req, "dev"));
        if (dev < 1 || dev > MAX_DEVICES || !g_dev[dev - 1].paired) { snprintf(err, cap, "no such device #%d", dev); return false; }
        snprintf(line, sizeof(line), "scan %d", dev); handle_cmd(line);
        if (result) cJSON_AddBoolToObject(result, "scanning", true);
        return true;
    }
    snprintf(err, cap, "unknown command '%s'", cmd);
    return false;
}

/* Rebooting actions: build the console line and run it (console handlers seed
 * state + reboot). comm.cpp has already sent its JSON response. */
bool hub_stage_pair(const char *pl, char *err, size_t cap)
{
    if (!pl || !pl[0]) { snprintf(err, cap, "empty payload"); return false; }
    char line[96]; snprintf(line, sizeof(line), "pairlist %s", pl);
    handle_cmd(line); return true;   /* reboots */
}
bool hub_stage_pairlist(const cJSON *pls, int *queued, char *err, size_t cap)
{
    char line[256]; int off = snprintf(line, sizeof(line), "pairlist"); int n = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, pls) {
        const char *s = cJSON_GetStringValue(p);
        if (s && s[0] && off < (int)sizeof(line) - 2) { off += snprintf(line + off, sizeof(line) - off, " %s", s); ++n; }
    }
    if (queued) *queued = n;
    if (n == 0) { snprintf(err, cap, "no payloads"); return false; }
    handle_cmd(line); return true;   /* reboots */
}
bool hub_remove_device(int dev, char *err, size_t cap)
{
    if (dev < 1 || dev > MAX_DEVICES || !g_dev[dev - 1].paired) { snprintf(err, cap, "no paired device #%d", dev); return false; }
    char line[32]; snprintf(line, sizeof(line), "remove %d", dev);
    handle_cmd(line); return true;   /* reboots */
}
void hub_factory_reset(void) { handle_cmd("reset"); }   /* reboots */

/* ===================================================================== */
/*  Tasks                                                                  */
/* ===================================================================== */
static void sched_mdns_restart_after_thread_ready(intptr_t)
{
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(1000), restart_mdns_after_thread_resume, nullptr);
}

static void sched_hint_timers(intptr_t)
{
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(15000), scan_thread_peers, (void *)21);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(30000), scan_thread_peers, (void *)22);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(60000), scan_thread_peers, (void *)100);
    /* Keep retrying the initial subscribe for devices that never reported
     * (sleepy/ICD devices like the BILRESA button). */
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(SUB_RETRY_MS), subscribe_retry_timer, nullptr);
    /* Arm the logic engine's periodic re-evaluation / TRACK re-assert timer. */
    logic_start();
}

static void thread_init_task(void *)
{
    otInstance *ot = esp_openthread_get_instance();
    if (!ot) { ESP_LOGE(TAG, "OT instance not available"); vTaskDelete(nullptr); return; }
    if (!esp_openthread_lock_acquire(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Failed to acquire OT lock"); vTaskDelete(nullptr); return;
    }

    otOperationalDatasetTlvs tlvs = {};
    bool has_dataset = (otDatasetGetActiveTlvs(ot, &tlvs) == OT_ERROR_NONE && tlvs.mLength > 0);
    if (!has_dataset) {
        ESP_LOGI(TAG, "Thread: no dataset - creating new network");
        otOperationalDataset ds = {};
        if (otDatasetCreateNewNetwork(ot, &ds) == OT_ERROR_NONE) otDatasetSetActive(ot, &ds);
        else ESP_LOGE(TAG, "otDatasetCreateNewNetwork failed");
    } else {
        ESP_LOGI(TAG, "Thread: using stored dataset (%u bytes)", (unsigned)tlvs.mLength);
    }
    if (!otIp6IsEnabled(ot)) otIp6SetEnabled(ot, true);
    if (otThreadGetDeviceRole(ot) == OT_DEVICE_ROLE_DISABLED) otThreadSetEnabled(ot, true);
    esp_openthread_lock_release();

    ESP_LOGI(TAG, "Thread: waiting for network to form...");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(500))) continue;
        otDeviceRole role = otThreadGetDeviceRole(ot);
        esp_openthread_lock_release();
        if (role >= OT_DEVICE_ROLE_CHILD) {
            ESP_LOGI(TAG, "Thread ready - role: %s",
                     role == OT_DEVICE_ROLE_LEADER ? "leader" :
                     role == OT_DEVICE_ROLE_ROUTER ? "router" : "child");
            esp_openthread_lock_acquire(portMAX_DELAY);
            otSrpServerSetEnabled(esp_openthread_get_instance(), true);
            esp_openthread_lock_release();
            ESP_LOGI(TAG, "SRP server enabled; HEAP free=%u", (unsigned)esp_get_free_heap_size());
            xEventGroupSetBits(s_evt, EVT_THREAD_READY);
            /* Boot done: green if operating, amber (blink) if in commissioning mode. */
            status_led_set_state(g_paired ? LED_OPERATING : LED_PAIRING);
            if (g_paired) {
                xEventGroupSetBits(s_evt, EVT_COMMISSIONED);
                chip::DeviceLayer::PlatformMgr().ScheduleWork(sched_hint_timers, 0);
            }
            chip::DeviceLayer::PlatformMgr().ScheduleWork(sched_mdns_restart_after_thread_ready, 0);
            break;
        }
    }
    vTaskDelete(nullptr);
}

static void btn_task(void *)
{
    while (1) {
        EventBits_t b = xEventGroupWaitBits(s_evt, EVT_SHORT | EVT_LONG, pdTRUE, pdFALSE, portMAX_DELAY);
        if (b & EVT_LONG) { ESP_LOGW(TAG, "Factory reset"); nvs_flash_erase(); esp_restart(); }
        if (b & EVT_SHORT) {
            if (!g_paired) do_commission();
            else { ESP_LOGI(TAG, "Operating mode - 'pair' to add, 'remove <n>' to drop"); inspector_print_devices(); }
        }
    }
}

static void auto_commission_task(void *)
{
    xEventGroupWaitBits(s_evt, EVT_THREAD_READY, pdFALSE, pdFALSE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (!g_paired && (g_payload[0] != '\0' || g_pin != 0)) {
        ESP_LOGI(TAG, "Auto-commissioning (Thread ready)");
        do_commission();
    } else {
        ESP_LOGW(TAG, "Auto-commission skipped (no payload/pin or already paired)");
    }
    vTaskDelete(nullptr);
}

static void subscription_task(void *)
{
    xEventGroupWaitBits(s_evt, EVT_COMMISSIONED, pdTRUE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "Starting subscriptions for %d paired device(s)", g_dev_count);
    vTaskDelay(pdMS_TO_TICKS(2000));
    CHIP_ERROR ce = chip::DeviceLayer::PlatformMgr().ScheduleWork(subscribe_work, 0);
    if (ce != CHIP_NO_ERROR) ESP_LOGE(TAG, "ScheduleWork(subscribe) failed: %" CHIP_ERROR_FORMAT, ce.Format());
    s_sub_task_h = nullptr;    /* this task self-deletes; never leave the `heap`
                                * command a dangling handle to query */
    vTaskDelete(nullptr);
}

/* Serial console. MUST get its stack while heap is available (FOUNDATION §7 /
 * project_alpstuga_serial): in commissioning mode free heap is ~15 KB. */
static void serial_task(void *)
{
    char buf[192]; int pos = 0;
    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (c == '\n' || c == '\r') { if (pos > 0) { buf[pos] = '\0'; hub_console_exec(buf); pos = 0; } }
        else if (c >= 0x20 && pos < (int)sizeof(buf) - 1) buf[pos++] = (char)c;
    }
}

/* ===================================================================== */
/*  app_main                                                               */
/* ===================================================================== */
static void console_input_init(void) { setvbuf(stdin, nullptr, _IONBF, 0); }

extern "C" void app_main(void)
{
    console_input_init();
    status_led_init();          /* blue while booting; set green/amber once Thread is up */

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    nvs_load();
    bindings_load();   /* hub-mediated binding rules (own NVS key) */
    logic_load();      /* value-triggered logic rules (own NVS key) */
    schedule_load();   /* schedulers + shared calendar + tz (own NVS keys) */

    /* Batch pairing (v1.7): a non-empty queue drives commissioning across reboots.
     * Handle completion/skip here, then feed the current entry into g_payload so the
     * existing do_commission/commission_work path runs UNCHANGED. Queue empty ->
     * this whole block is a no-op and the single-device flow is byte-identical. */
    if (g_pairq.n > 0) {
        if (g_pairq.cur < g_pairq.n && g_pairq.attempts >= PAIRQ_MAX_ATTEMPTS) {
            ESP_LOGW(TAG, "Batch: giving up on payload after %d attempts, skipping", PAIRQ_MAX_ATTEMPTS);
            g_pairq.e[g_pairq.cur].status = PQ_FAILED;
            g_pairq.cur++; g_pairq.attempts = 0; nvs_save();
        }
        if (g_pairq.cur >= g_pairq.n || dev_first_free_slot() < 0) {
            pairq_print_report();                                  /* which paired / failed */
            memset(&g_pairq, 0, sizeof(g_pairq));
            g_payload[0] = '\0';                                   /* leave a clean state */
            nvs_save();                                            /* -> boots to operating */
        } else {
            g_pairq.attempts++;
            strncpy(g_payload, g_pairq.e[g_pairq.cur].code, sizeof(g_payload) - 1);
            g_payload[sizeof(g_payload) - 1] = '\0';
            nvs_save();
            ESP_LOGW(TAG, "Batch: pairing %u/%u (attempt %u): %s",
                     g_pairq.cur + 1, g_pairq.n, g_pairq.attempts, g_payload);
        }
    }

    if (g_payload[0] == '\0' && sizeof(DEFAULT_PAYLOAD) > 1) {
        strncpy(g_payload, DEFAULT_PAYLOAD, sizeof(g_payload) - 1);
        ESP_LOGW(TAG, "Using build-time DEFAULT_PAYLOAD");
    }

    /* Restore phase 2: once any re-pairing queued by `restore` has drained (batch
     * finished, or none was needed), resolve backup device identities to their
     * current slots and apply labels/bindings/logic. (bindings/logic tables were
     * loaded above; restore_finalize rebuilds + re-saves them.) */
    if (restore_pending() && !batch_active())
        restore_finalize();

    /* Boot mode: operating (>=1 paired) vs commissioning (0 paired, the one-shot
     * g_commission_mode flag set by 'pair', or a batch in progress). The flag is
     * one-shot; the queue is the persistent driver that survives per-device reboots. */
    if (g_commission_mode || batch_active()) {
        g_commission_mode = false; nvs_save();
        g_paired = false; g_auto_commission = true;
        ESP_LOGW(TAG, "Commissioning session (%s; %d already paired)",
                 batch_active() ? "batch" : "adding a device", g_dev_count);
    } else {
        g_paired = (g_dev_count > 0);
    }

    apply_debug_level(g_debug_verbose);   /* restore persisted debug state (K_DEBUG) */
    ESP_LOGI(TAG, "Matter Hub V1.0 starting - mode=%s devices=%d/%d",
             g_paired ? "operating" : "commissioning", g_dev_count, MAX_DEVICES);

    s_evt = xEventGroupCreate();
    configASSERT(s_evt);

    gpio_config_t bc = {
        .pin_bit_mask = 1ULL << BUTTON_PIN, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&bc));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)BUTTON_PIN, btn_isr, nullptr));

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Matter controller (server never started - controller mode). */
    auto &ctrl = esp_matter::controller::matter_controller_client::get_instance();
    /* MUST be set before init() - init() wires the check-in handler to whichever
     * delegate is registered. Without it a sleepy LIT-ICD never reconnects after a
     * reboot (see HubCheckInDelegate). Needs the local esp-matter patch adding
     * set_check_in_delegate(). */
    ctrl.set_check_in_delegate(&s_check_in_delegate);
    err = ctrl.init(CONTROLLER_NODE_ID, CONTROLLER_FABRIC, 5580);
    if (err != ESP_OK) { ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(err)); return; }
    /* BLE only in commissioning mode; off while operating (FOUNDATION §4). */
    err = ctrl.setup_commissioner(/*enable_ble*/ !g_paired);
    if (err != ESP_OK) { ESP_LOGE(TAG, "setup_commissioner failed: %s", esp_err_to_name(err)); return; }

    err = (chip::DeviceLayer::PlatformMgr().StartEventLoopTask() == CHIP_NO_ERROR) ? ESP_OK : ESP_FAIL;
    ESP_ERROR_CHECK(err);

    /* In controller mode esp_matter::start() is never called, so Thread is not
     * auto-initialised - do it ourselves (FOUNDATION §1). */
    static esp_openthread_platform_config_t ot_cfg = {
        .radio_config = { .radio_mode = RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = HOST_CONNECTION_MODE_NONE },
        .port_config  = { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 },
    };
    ESP_ERROR_CHECK(set_openthread_platform_config(&ot_cfg));
    if (chip::DeviceLayer::ThreadStackMgr().InitThreadStack() != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "InitThreadStack failed - halting"); return;
    }
    if (chip::DeviceLayer::ThreadStackMgr().StartThreadTask() != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "StartThreadTask failed - halting"); return;
    }
    ESP_LOGI(TAG, "Thread stack initialised");

    ESP_LOGI(TAG, "Press BOOT to commission | hold 5s to factory reset");
    ESP_LOGI(TAG, "Console: payload/pin/pair/remove/status/settime | devices/tree/read/write/invoke | bind/bindings/unbind/bindable");

    /* Task-creation order matters (FOUNDATION §7): serial_task MUST get its stack
     * while heap is available. Create always-needed tasks first; sensor/subscribe
     * tasks only when paired (they'd otherwise waste stack blocking in commissioning
     * mode and starve serial_task). */
    s_cmd_mutex = xSemaphoreCreateMutex();   /* serialises UART console + USB comm command execution */
    xTaskCreate(thread_init_task, "thread_init", 3072, nullptr, 6, nullptr);
    xTaskCreate(btn_task,         "btn",         3584, nullptr, 5, nullptr);   /* 2560->3584: also calls inspector_print_devices (label snprintf) on BOOT press */
    /* 5120 (was 3584): v1.7 added device-label snprintf buffers on the print paths
     * and a label temp array in nvs_save, deepening the console call chain past the
     * old margin (~1.7 KB free) -> Stack protection fault. FOUNDATION §7: budget
     * >=4 KB for snprintf-into-buffer console tasks. */
    if (xTaskCreate(serial_task,  "serial",      5120, nullptr, 5, &s_serial_task_h) != pdPASS)
        ESP_LOGE(TAG, "serial_task create FAILED (low heap)");
    if (g_paired) {
        xTaskCreate(subscription_task, "subscribe", 4096, nullptr, 4, &s_sub_task_h);
    } else if (g_auto_commission) {
        xTaskCreate(auto_commission_task, "autocomm", 3072, nullptr, 4, nullptr);
    }
    /* Always start the USB-JSON protocol - even unpaired - so a host (the S3 portal)
     * can reach a fresh hub to configure or restore it. comm_start() gracefully no-ops
     * if the driver can't install under heap pressure. */
    comm_start();   /* USB-Serial-JTAG JSON protocol on the native USB port (COM7) */
    ESP_LOGI(TAG, "HEAP after tasks free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
