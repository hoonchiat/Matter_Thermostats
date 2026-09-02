/*
 * device_model.h - generic Matter device model for the Matter Hub.
 *
 * Unlike the ALPSTUGA reference (which hardcoded temp/CO2/PM2.5 clusters), the
 * Hub discovers each paired device's structure at runtime by walking its
 * Descriptor (0x001D) and Basic Information (0x0028) clusters. The result is
 * cached in this bounded, statically-allocated tree - no dynamic allocation
 * (the C6 is heap-tight; see FOUNDATION.md §7).
 *
 * The tree stores only cluster PRESENCE + attribute/command COUNTS (v1.6 RAM
 * diet): every consumer (bindings/logic/dash/tree/ep, devcap scoping) needs
 * presence only, and the `cluster` command live-reads the id lists on demand
 * instead of caching them. One EndpointInfo is ~150 B (was ~2.4 KB with the id
 * lists cached inline). Enumeration logs a warning and truncates when a device
 * exceeds a cap rather than overflowing.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <openthread/thread.h>   /* otExtAddress, OT_EXT_ADDRESS_SIZE */

/*
 * Memory strategy: the endpoint tree is NOT stored inline in g_dev[] - that
 * would cost static .bss in EVERY mode, including BLE commissioning where free
 * heap is ~15 KB (FOUNDATION §7). Instead each EndpointInfo is allocated lazily
 * on enumeration (which only runs in operating mode) in small per-endpoint
 * blocks (~150 B) that are kind to a fragmented heap. Static cost in g_dev[] is
 * just MAX_EP pointers.
 */

/* ---- Capacity caps (tunable) --------------------------------------------- */
/* Raising MAX_DEVICES is cheap: the endpoint tree is heap-allocated per device
 * on enumeration, so an unused slot costs only its small DeviceInfo header. It
 * DOES change the NVS blob size though - nvs_load() migrates a smaller stored
 * table into the low slots so existing pairings survive the change. */
#define MAX_DEVICES   7       /* paired devices managed on the one fabric      */
#define MAX_EP        6       /* endpoints cached per device (pointer array)   */
#define MAX_CL        16      /* server clusters per endpoint (inline)         */
#define MAX_DT        4       /* device types per endpoint                      */

/* Fixed operational node id per slot: BASE + slot (deterministic re-pairs). */
#define NODE_ID_BASE  0x0000000200000001ULL

/* Controller (this hub) identity - single fabric. */
#define CONTROLLER_NODE_ID  0x0000000100000001ULL
#define CONTROLLER_FABRIC   0x0001ULL

/* Well-known Matter ids used by the enumerator (see matter_names.h for labels). */
#define CL_DESCRIPTOR       0x001DUL
#define CL_BASIC_INFO       0x0028UL
#define CL_ONOFF            0x0006UL   /* used by the on/off/toggle console shortcuts */
#define CMD_ONOFF_OFF       0x00UL
#define CMD_ONOFF_ON        0x01UL
#define CMD_ONOFF_TOGGLE    0x02UL
#define A_DESC_DEVICETYPELIST 0x0000UL
#define A_DESC_SERVERLIST     0x0001UL
#define A_DESC_CLIENTLIST     0x0002UL
#define A_DESC_PARTSLIST      0x0003UL
#define A_BASIC_VENDORID      0x0002UL   /* Basic Information: VendorID          */
#define A_BASIC_PRODUCTID     0x0004UL   /* Basic Information: ProductID         */
#define A_BASIC_PRODUCTNAME   0x0003UL   /* Basic Information: ProductName       */
#define A_BASIC_NODELABEL     0x0005UL   /* Basic Information: NodeLabel         */
#define A_GLOBAL_ATTRLIST     0xFFFBUL   /* global: AttributeList               */
#define A_GLOBAL_ACCEPTEDCMDS 0xFFF9UL   /* global: AcceptedCommandList         */

#define WILDCARD_EP   0xFFFFU
#define WILDCARD_ID   0xFFFFFFFFUL

/* Cached description of one server cluster on an endpoint: presence + counts
 * only. The id LISTS are not cached (they were 144 B/cluster of RAM) - the
 * `cluster` console command live-reads them on demand instead. */
struct ClusterInfo {
    uint32_t id;
    uint8_t  n_attr;     /* count from the cluster's AttributeList (0xFFFB)       */
    uint8_t  n_cmd;      /* count from its AcceptedCommandList (0xFFF9)           */
};

/* Cached description of one endpoint. */
struct EndpointInfo {
    uint16_t    id;
    uint8_t     n_dt;
    uint8_t     n_srv;
    uint32_t    device_types[MAX_DT];
    ClusterInfo servers[MAX_CL];
};

/*
 * One paired device. Identity + Thread-reachability tracking is populated at
 * commissioning / by the peer-hint scanner (Phase 1); the endpoint tree (vid,
 * pid, name, eps) is filled by the enumerator once CASE is up (Phase 2).
 */
struct DeviceInfo {
    /* ---- identity / pairing ------------------------------------------- */
    bool     paired;
    uint64_t node_id;
    char     payload[72];        /* onboarding code used to commission (label) */

    /* ---- discovered device tree (enumeration; lazily allocated) ------- */
    bool     enumerated;         /* true once the tree below is populated      */
    bool     enumerating;        /* true while a wildcard read is in flight     */
    uint8_t  enum_expected_eps;  /* endpoints expected (from ep0 PartsList); 0=? */
    uint8_t  enum_retry;         /* retries used for the current enumeration     */
    uint16_t vid;                /* Basic Information VendorID                  */
    uint16_t pid;                /* Basic Information ProductID                 */
    char     name[32];           /* ProductName / NodeLabel from the device     */
    char     label[32];          /* user-set label (persist K_DEVLABEL); "" = none.
                                  * Survives re-enumeration (only `name` is rewritten)
                                  * so identical models can be told apart.        */
    uint8_t  n_eps;
    EndpointInfo *eps[MAX_EP];   /* each malloc'd on enumeration; nullptr until */

    /* ---- Thread reachability / liveness ------------------------------- */
    otExtAddress eui64;
    bool         eui64_known;
    uint16_t     rloc16;
    uint32_t     sub_id;
    int8_t       rssi;           /* average RSSI dBm from child/neighbor table  */
    uint8_t      lqi;            /* link quality 0..3                            */
    bool         link_valid;
    volatile int64_t last_report_us;  /* liveness clock (esp_timer us)          */
    volatile bool    has_data;        /* has this device ever reported?         */
    volatile int64_t last_sub_attempt_us; /* when we last tried to subscribe    */
};

/* Defined in main.cpp; shared with inspector.cpp. */
extern DeviceInfo g_dev[MAX_DEVICES];
extern int        g_dev_count;

/* Persist the device table (incl. last-known vid/pid/name). Defined in main.cpp;
 * called by the enumerator so a device's identity survives reboots even while it
 * is asleep and cannot be re-enumerated. */
void hub_persist_devices(void);

/* Recompute a device's subscription-capability mask (which of the "extra"
 * measurement/battery clusters it actually exposes) from its enumerated tree, so
 * later boots subscribe only the paths it needs. Defined in main.cpp; called by
 * the enumerator once a device is fully enumerated (before hub_persist_devices). */
void hub_update_devcap(int slot);

/* ---- config backup/restore: device identity <-> current slot mapping --------
 * A backup references devices by INDEX into its own device list (identity), not by
 * slot. On restore the module apply-functions resolve each index to the device's
 * ACTUAL current slot (matched by payload) so bindings/logic follow the device
 * wherever it re-pairs. A rule referencing an unresolved (un-repaired) device is
 * skipped and reported. */
struct restore_map {
    const int         *idx2slot;   /* backup device index -> current slot, -1 if not paired */
    const char *const *idx_name;   /* backup device index -> display name (for skip report) */
    int                n_dev;      /* number of devices in the backup                       */
};

/* ---- small inline helpers (usable from any translation unit) -------------- */
static inline uint64_t slot_node_id(int slot) { return NODE_ID_BASE + (uint64_t)slot; }

static inline int dev_slot_by_node(uint64_t node_id)
{
    for (int i = 0; i < MAX_DEVICES; ++i)
        if (g_dev[i].paired && g_dev[i].node_id == node_id) return i;
    return -1;
}

static inline int dev_first_free_slot(void)
{
    for (int i = 0; i < MAX_DEVICES; ++i)
        if (!g_dev[i].paired) return i;
    return -1;
}

static inline int dev_count_paired(void)
{
    int n = 0;
    for (int i = 0; i < MAX_DEVICES; ++i) if (g_dev[i].paired) ++n;
    return n;
}

/* Human-facing device name for any listing: the user label plus the device's own
 * name when a label is set ("Kitchen (ALPSTUGA air quality monitor)"), else just
 * the device name (or "?" if not enumerated yet). Writes into caller's buffer. */
static inline const char *dev_display_name(const DeviceInfo &d, char *buf, size_t cap)
{
    const char *nm = d.name[0] ? d.name : "?";
    if (d.label[0]) snprintf(buf, cap, "%s (%s)", d.label, nm);
    else            snprintf(buf, cap, "%s", nm);
    return buf;
}

/* ---- endpoint/cluster lookup within a cached device ----------------------- */
static inline EndpointInfo *dev_find_ep(DeviceInfo &d, uint16_t ep)
{
    for (int i = 0; i < d.n_eps; ++i)
        if (d.eps[i] && d.eps[i]->id == ep) return d.eps[i];
    return nullptr;
}

/* Free a device's lazily-allocated endpoint tree and mark it un-enumerated. */
static inline void dev_free_tree(DeviceInfo &d)
{
    for (int i = 0; i < MAX_EP; ++i) { if (d.eps[i]) { free(d.eps[i]); d.eps[i] = nullptr; } }
    d.n_eps = 0;
    d.enumerated = false;
    /* Identity (vid/pid/name) is deliberately NOT cleared here: it is the last-known
     * identity that must survive a failed/empty enumeration read. This function's only
     * caller starts a fresh read that repopulates the identity on success (enum_attr_cb);
     * clearing it here meant a timed-out read persisted zeros over a good identity. */
}

static inline ClusterInfo *ep_find_cluster(EndpointInfo &e, uint32_t cl)
{
    for (int i = 0; i < e.n_srv; ++i)
        if (e.servers[i].id == cl) return &e.servers[i];
    return nullptr;
}
