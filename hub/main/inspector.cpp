/* inspector.cpp - see inspector.h. */
#include "inspector.h"
#include "device_model.h"
#include "matter_names.h"
#include "tlv_decode.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cinttypes>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

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

static const char *TAG = "inspector";

/* ===================================================================== *
 *  Enumeration - build the endpoint/cluster/attribute tree
 * ===================================================================== */

static EndpointInfo *enum_touch_ep(DeviceInfo &d, uint16_t ep_id)
{
    EndpointInfo *e = dev_find_ep(d, ep_id);
    if (e) return e;
    if (d.n_eps >= MAX_EP) {
        ESP_LOGW(TAG, "device 0x%016" PRIx64 ": >%d endpoints, truncating", d.node_id, MAX_EP);
        return nullptr;
    }
    e = (EndpointInfo *)calloc(1, sizeof(EndpointInfo));   /* small (~150 B) block */
    if (!e) { ESP_LOGE(TAG, "OOM: endpoint alloc"); return nullptr; }
    e->id = ep_id;
    d.eps[d.n_eps++] = e;
    return e;
}

static ClusterInfo *enum_touch_cluster(EndpointInfo &e, uint32_t cl_id)
{
    ClusterInfo *c = ep_find_cluster(e, cl_id);
    if (c) return c;
    if (e.n_srv >= MAX_CL) {
        ESP_LOGW(TAG, "endpoint %u: >%d clusters, truncating", e.id, MAX_CL);
        return nullptr;
    }
    c = &e.servers[e.n_srv++];
    memset(c, 0, sizeof(*c));
    c->id = cl_id;
    return c;
}

/* Count the elements of a TLV array (AttributeList / AcceptedCommandList). The
 * ids themselves are NOT stored (v1.6 RAM diet) - only the count is cached; the
 * `cluster` command live-reads the actual lists on demand. */
static uint8_t count_id_list(TLVReader &r)
{
    int cnt = 0;
    TLVType outer;
    if (r.EnterContainer(outer) != CHIP_NO_ERROR) return 0;
    while (r.Next() == CHIP_NO_ERROR) ++cnt;
    r.ExitContainer(outer);
    return (uint8_t)(cnt > 255 ? 255 : cnt);
}

/* Parse Descriptor.DeviceTypeList: array of struct { 0:DeviceType, 1:Revision }. */
static void parse_devtype_list(TLVReader &r, EndpointInfo &e)
{
    TLVType outer;
    if (r.EnterContainer(outer) != CHIP_NO_ERROR) return;
    while (r.Next() == CHIP_NO_ERROR) {
        TLVType inner;
        if (r.EnterContainer(inner) == CHIP_NO_ERROR) {
            if (r.Next() == CHIP_NO_ERROR) {           /* first field = DeviceType */
                uint64_t v = 0;
                if (r.Get(v) == CHIP_NO_ERROR && e.n_dt < MAX_DT)
                    e.device_types[e.n_dt++] = (uint32_t)v;
            }
            r.ExitContainer(inner);
        }
    }
    r.ExitContainer(outer);
}

/* Targeted-read attribute callback (v1.9 lean enum). The read requests ONLY:
 *   - Descriptor.ServerList (0x1D/0x01) per endpoint -> cluster map (one list/endpoint)
 *   - Descriptor.DeviceTypeList / ep0 PartsList       -> structure
 *   - four Basic Information identity attributes
 * ServerList is MANDATORY on every endpoint's Descriptor and names every server
 * cluster directly, so it enumerates the full cluster map in ONE compact list per
 * endpoint - ~20x less than the earlier per-cluster AttributeList wildcard (0xFFFB
 * over all clusters), whose chunked response over Thread timed out with 6 devices.
 * The AttributeList/AcceptedCommandList handlers below remain for compatibility but
 * are no longer prefetched; per-cluster attr/cmd counts are live-read on demand. */
static void enum_attr_cb(uint64_t node_id, const ConcreteDataAttributePath &path, TLVReader *data)
{
    int slot = dev_slot_by_node(node_id);
    if (slot < 0) return;
    DeviceInfo &d = g_dev[slot];

    EndpointInfo *e = enum_touch_ep(d, path.mEndpointId);
    if (!e || !data) return;
    TLVReader r; r.Init(*data);

    if (path.mAttributeId == A_GLOBAL_ATTRLIST) {          /* structure + attr count */
        ClusterInfo *c = enum_touch_cluster(*e, path.mClusterId);
        if (c) c->n_attr = count_id_list(r);
        return;
    }
    if (path.mAttributeId == A_GLOBAL_ACCEPTEDCMDS) {      /* command count */
        ClusterInfo *c = enum_touch_cluster(*e, path.mClusterId);
        if (c) c->n_cmd = count_id_list(r);
        return;
    }

    if (path.mEndpointId == 0 && path.mClusterId == CL_BASIC_INFO) {
        if (path.mAttributeId == A_BASIC_VENDORID) {
            uint64_t v = 0; if (r.Get(v) == CHIP_NO_ERROR) d.vid = (uint16_t)v;
        } else if (path.mAttributeId == A_BASIC_PRODUCTID) {
            uint64_t v = 0; if (r.Get(v) == CHIP_NO_ERROR) d.pid = (uint16_t)v;
        } else if (path.mAttributeId == A_BASIC_PRODUCTNAME ||
                   (path.mAttributeId == A_BASIC_NODELABEL && d.name[0] == '\0')) {
            char tmp[32];
            if (r.GetType() == chip::TLV::kTLVType_UTF8String &&
                r.GetString(tmp, sizeof(tmp)) == CHIP_NO_ERROR && tmp[0])
                strncpy(d.name, tmp, sizeof(d.name) - 1);
        }
    } else if (path.mClusterId == CL_DESCRIPTOR && path.mAttributeId == A_DESC_SERVERLIST) {
        /* ServerList = every server cluster on this endpoint. Add each to the tree;
         * this is what discovers the cluster map (replaces the AttributeList wildcard). */
        TLVType outer;
        if (r.EnterContainer(outer) == CHIP_NO_ERROR) {
            while (r.Next() == CHIP_NO_ERROR) {
                uint64_t cid = 0;
                if (r.Get(cid) == CHIP_NO_ERROR) enum_touch_cluster(*e, (uint32_t)cid);
            }
            r.ExitContainer(outer);
        }
    } else if (path.mClusterId == CL_DESCRIPTOR && path.mAttributeId == A_DESC_DEVICETYPELIST) {
        e->n_dt = 0;
        parse_devtype_list(r, *e);
    }
    if (path.mEndpointId == 0 && path.mClusterId == CL_DESCRIPTOR &&
        path.mAttributeId == A_DESC_PARTSLIST) {
        /* ep0 PartsList = every other endpoint. Total expected = 1 (root) + count.
         * Used to detect a read truncated by heap pressure (below). */
        d.enum_expected_eps = (uint8_t)(1 + count_id_list(r));
    }
}

/* Even the targeted read can be truncated when several devices reconnect at once
 * (post-reboot burst) plus an event backlog. We detect truncation against the
 * ep0 PartsList count and retry once heap recovers. The threshold was 30000 with
 * the old full-value wildcard read; the targeted read transfers ~10x less. */
#define ENUM_MIN_HEAP   30000     /* below this, defer the read rather than truncate */
#define ENUM_RETRY_MS   4000      /* wait for heap / event backlog to drain          */
#define MAX_ENUM_RETRY  4

static void enum_issue_read(int slot);
static void enum_retry_fire(chip::System::Layer *, void *ctx) { enum_issue_read((int)(intptr_t)ctx - 1); }

static void enum_done_cb(uint64_t node_id,
                         const ScopedMemoryBufferWithSize<AttributePathParams> &,
                         const ScopedMemoryBufferWithSize<EventPathParams> &)
{
    int slot = dev_slot_by_node(node_id);
    if (slot < 0) return;
    DeviceInfo &d = g_dev[slot];

    int clusters = 0;
    for (int i = 0; i < d.n_eps; ++i) clusters += d.eps[i]->n_srv;

    /* Empty read: the targeted read returned no endpoints at all - almost always the
     * large multi-path response timed out before any data arrived. Do NOT mark the
     * device enumerated or persist an empty tree (which would also wipe the last-known
     * identity). Retry a few times, then give up leaving enumerated=false + identity
     * intact, so `devices` still shows what the device is and `scan` can retry later. */
    if (d.n_eps == 0) {
        if (d.enum_retry < MAX_ENUM_RETRY) {
            d.enum_retry++;
            ESP_LOGW(TAG, "Empty enum #%d (no endpoints; heap free=%u) - retry %u/%d in %dms",
                     slot + 1, (unsigned)esp_get_free_heap_size(), d.enum_retry, MAX_ENUM_RETRY, ENUM_RETRY_MS);
            chip::DeviceLayer::SystemLayer().StartTimer(
                chip::System::Clock::Milliseconds32(ENUM_RETRY_MS), enum_retry_fire, (void *)(intptr_t)(slot + 1));
            return;
        }
        d.enumerating = false;
        ESP_LOGW(TAG, "Enum #%d failed: no data after %d retries; keeping last-known identity",
                 slot + 1, MAX_ENUM_RETRY);
        return;
    }

    /* Truncated read? Fewer endpoints than ep0's PartsList promised -> retry. */
    bool partial = (d.enum_expected_eps > 0 && d.n_eps < d.enum_expected_eps);
    if (partial && d.enum_retry < MAX_ENUM_RETRY) {
        d.enum_retry++;
        ESP_LOGW(TAG, "Partial enum #%d: %u of %u endpoints (heap free=%u) - retry %u/%d in %dms",
                 slot + 1, d.n_eps, d.enum_expected_eps, (unsigned)esp_get_free_heap_size(),
                 d.enum_retry, MAX_ENUM_RETRY, ENUM_RETRY_MS);
        /* enumerating stays true; do NOT mark enumerated or persist a partial tree. */
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(ENUM_RETRY_MS), enum_retry_fire, (void *)(intptr_t)(slot + 1));
        return;
    }

    d.enumerated  = true;
    d.enumerating = false;
    ESP_LOGI(TAG, "Enumerated #%d node=0x%016" PRIx64 " VID=0x%04x PID=0x%04x eps=%u clusters=%d%s (heap free=%u)",
             slot + 1, node_id, d.vid, d.pid, d.n_eps, clusters,
             partial ? " (PARTIAL - retries exhausted)" : "", (unsigned)esp_get_free_heap_size());
    /* Remember what this device IS, so `devices` stays readable when it is asleep
     * and cannot be re-enumerated (the tree itself stays RAM-only). Capture its
     * subscription-capability mask ONLY from a COMPLETE tree - a partial enum
     * could miss a sensor endpoint and wrongly under-scope it (silent data loss);
     * leaving caps unknown just keeps the full path set until it enumerates fully. */
    if (!partial) hub_update_devcap(slot);
    hub_persist_devices();
    char nm[72];
    printf("[hub] Device #%d enumerated: VID=0x%04x PID=0x%04x  \"%s\"  %u endpoint(s).%s "
           "Type 'tree %d' to view.\r\n",
           slot + 1, d.vid, d.pid, dev_display_name(d, nm, sizeof(nm)), d.n_eps,
           partial ? "  (PARTIAL - 'scan' to retry)" : "", slot + 1);
}

/* Issue the wildcard attribute read for one device (CHIP task). Deferred if heap
 * is too low to complete it, up to MAX_ENUM_RETRY. Both the fresh trigger and a
 * partial-read retry land here. */
static void enum_issue_read(int slot)
{
    if (slot < 0 || slot >= MAX_DEVICES || !g_dev[slot].paired) return;
    DeviceInfo &d = g_dev[slot];
    dev_free_tree(d);              /* clear tree (incl. any partial); re-alloc as discovered */
    d.enum_expected_eps = 0;

    size_t freeheap = esp_get_free_heap_size();
    if (freeheap < ENUM_MIN_HEAP && d.enum_retry < MAX_ENUM_RETRY) {
        d.enum_retry++;
        ESP_LOGW(TAG, "Defer enum #%d: heap %u < %u - retry %u/%d in %dms",
                 slot + 1, (unsigned)freeheap, ENUM_MIN_HEAP, d.enum_retry, MAX_ENUM_RETRY, ENUM_RETRY_MS);
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(ENUM_RETRY_MS), enum_retry_fire, (void *)(intptr_t)(slot + 1));
        return;
    }

    /* Targeted multi-path read (see enum_attr_cb): id LISTS + identity only,
     * never attribute values wholesale. Multi-path ctor is the same one dash
     * uses; the 3-arg AttributePathParams takes 0xFFFF/0xFFFFFFFF wildcards. */
    ScopedMemoryBufferWithSize<AttributePathParams> paths;
    ScopedMemoryBufferWithSize<EventPathParams>     ev;
    if (!paths.Alloc(7)) { ESP_LOGE(TAG, "OOM: enum paths"); d.enumerating = false; return; }
    /* Descriptor.ServerList is ONE compact cluster-id list per endpoint and enumerates
     * the full cluster map ~20x smaller than reading every cluster's AttributeList
     * wildcard (0xFFFB over all clusters). That larger response, chunked over Thread,
     * timed out with 6 devices - so we read ServerList instead. Per-cluster attribute/
     * command counts (n_attr/n_cmd) are no longer prefetched; the `cluster` command
     * live-reads them on demand. */
    paths[0] = AttributePathParams((uint16_t)WILDCARD_EP, (uint32_t)CL_DESCRIPTOR, (uint32_t)A_DESC_SERVERLIST);
    paths[1] = AttributePathParams((uint16_t)WILDCARD_EP, (uint32_t)CL_DESCRIPTOR, (uint32_t)A_DESC_DEVICETYPELIST);
    paths[2] = AttributePathParams((uint16_t)0, (uint32_t)CL_DESCRIPTOR, (uint32_t)A_DESC_PARTSLIST);
    paths[3] = AttributePathParams((uint16_t)0, (uint32_t)CL_BASIC_INFO, (uint32_t)A_BASIC_VENDORID);
    paths[4] = AttributePathParams((uint16_t)0, (uint32_t)CL_BASIC_INFO, (uint32_t)A_BASIC_PRODUCTID);
    paths[5] = AttributePathParams((uint16_t)0, (uint32_t)CL_BASIC_INFO, (uint32_t)A_BASIC_PRODUCTNAME);
    paths[6] = AttributePathParams((uint16_t)0, (uint32_t)CL_BASIC_INFO, (uint32_t)A_BASIC_NODELABEL);

    auto *cmd = chip::Platform::New<esp_matter::controller::read_command>(
        d.node_id, std::move(paths), std::move(ev), enum_attr_cb, enum_done_cb, nullptr);
    if (!cmd) { ESP_LOGE(TAG, "OOM: enum read_command"); d.enumerating = false; return; }
    esp_err_t e = cmd->send_command();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "enum read failed: %s", esp_err_to_name(e));
        chip::Platform::Delete(cmd);
        d.enumerating = false;
    } else {
        ESP_LOGI(TAG, "Enumerating device #%d (targeted read)...", slot + 1);
    }
}

/* CHIP-task work: start a FRESH enumeration (resets the retry counter). */
static void enum_work(intptr_t context)
{
    int slot = (int)context - 1;
    if (slot < 0 || slot >= MAX_DEVICES) return;
    g_dev[slot].enum_retry = 0;
    enum_issue_read(slot);
}

void inspector_on_case_up(uint64_t node_id)
{
    int slot = dev_slot_by_node(node_id);
    if (slot < 0) return;
    /* Run exactly once per device. Called from the first attribute report (which
     * this esp-matter build delivers reliably, unlike the subscribe done_cb).
     * The enumerating flag prevents every subsequent keepalive report from
     * kicking a second concurrent wildcard read. */
    if (g_dev[slot].enumerated || g_dev[slot].enumerating) return;
    g_dev[slot].enumerating = true;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(enum_work, (intptr_t)(slot + 1));
}

/* ===================================================================== *
 *  Live read / write / invoke  (each runs on the CHIP task)
 * ===================================================================== */

static struct { uint64_t node; uint16_t ep; uint32_t cl; uint32_t attr; bool single; } s_read_req;
static struct { uint64_t node; uint16_t ep; uint32_t cl; uint32_t attr; char json[96]; } s_write_req;
static struct { uint64_t node; uint16_t ep; uint32_t cl; uint32_t cmd;  char json[128]; } s_invoke_req;

static void read_attr_cb(uint64_t /*node*/, const ConcreteDataAttributePath &path, TLVReader *data)
{
    char val[256] = "<no data>";
    if (data) { TLVReader r; r.Init(*data); tlv_to_str(r, val, sizeof(val)); }
    char clb[40], alb[48];
    printf("  ep%u %s %s = %s\r\n",
           path.mEndpointId, cluster_label(path.mClusterId, clb, sizeof(clb)),
           attr_label(path.mClusterId, path.mAttributeId, alb, sizeof(alb)), val);
}

static void read_done_cb(uint64_t node_id,
                         const ScopedMemoryBufferWithSize<AttributePathParams> &,
                         const ScopedMemoryBufferWithSize<EventPathParams> &)
{
    printf("[hub] read done (node=0x%016" PRIx64 ")\r\n", node_id);
}

static void read_work(intptr_t)
{
    uint32_t attr = s_read_req.single ? s_read_req.attr : (uint32_t)WILDCARD_ID;
    auto *cmd = chip::Platform::New<esp_matter::controller::read_command>(
        s_read_req.node, s_read_req.ep, s_read_req.cl, attr,
        esp_matter::controller::READ_ATTRIBUTE, read_attr_cb, read_done_cb, nullptr);
    if (!cmd) { ESP_LOGE(TAG, "OOM: read_command"); return; }
    if (cmd->send_command() != ESP_OK) { ESP_LOGE(TAG, "read send failed"); chip::Platform::Delete(cmd); }
}

static void write_work(intptr_t)
{
    esp_err_t e = esp_matter::controller::send_write_attr_command(
        s_write_req.node, s_write_req.ep, s_write_req.cl, s_write_req.attr, s_write_req.json);
    printf("[hub] write %s (watch log for the device's response status)\r\n",
           e == ESP_OK ? "sent" : "FAILED to send");
}

static void invoke_success_cb(void *, const chip::app::ConcreteCommandPath &path,
                              const chip::app::StatusIB &status, TLVReader *data)
{
    char val[192] = "";
    if (data) { TLVReader r; r.Init(*data); tlv_to_str(r, val, sizeof(val)); }
    char clb[40], cmb[48];
    printf("[hub] invoke OK: ep%u %s %s status=0x%02x %s%s\r\n",
           path.mEndpointId, cluster_label(path.mClusterId, clb, sizeof(clb)),
           cmd_label(path.mClusterId, path.mCommandId, cmb, sizeof(cmb)),
           (unsigned)static_cast<uint8_t>(status.mStatus), val[0] ? "resp=" : "", val);
}

static void invoke_error_cb(void *, CHIP_ERROR error)
{
    printf("[hub] invoke FAILED: %" CHIP_ERROR_FORMAT "\r\n", error.Format());
}

static void invoke_work(intptr_t)
{
    auto *cmd = chip::Platform::New<esp_matter::controller::cluster_command>(
        s_invoke_req.node, s_invoke_req.ep, s_invoke_req.cl, s_invoke_req.cmd,
        s_invoke_req.json[0] ? s_invoke_req.json : "{}",
        chip::NullOptional, invoke_success_cb, invoke_error_cb);
    if (!cmd) { ESP_LOGE(TAG, "OOM: cluster_command"); return; }
    if (cmd->send_command() != ESP_OK) { ESP_LOGE(TAG, "invoke send failed"); chip::Platform::Delete(cmd); }
}

/* ===================================================================== *
 *  Console rendering
 * ===================================================================== */

void inspector_print_devices(void)
{
    printf("[hub] Devices (%d/%d paired):\r\n", g_dev_count, MAX_DEVICES);
    for (int i = 0; i < MAX_DEVICES; ++i) {
        DeviceInfo &d = g_dev[i];
        if (!d.paired) { printf("  #%d  <empty>\r\n", i + 1); continue; }
        char nm[72];
        printf("  #%d  payload=%s  \"%s\"\r\n"
               "      node=0x%016" PRIx64 "  VID=0x%04x PID=0x%04x  eps=%u  %s\r\n",
               i + 1, d.payload[0] ? d.payload : "(pin/disc)", dev_display_name(d, nm, sizeof(nm)),
               d.node_id, d.vid, d.pid, d.n_eps,
               d.enumerated ? "enumerated"
                            : (d.vid ? "(offline - last known identity)"
                                     : "(not enumerated yet)"));
    }
}

static void print_tree(DeviceInfo &d, int slot)
{
    char nm[72];
    printf("[hub] Device #%d  payload=%s  \"%s\"\r\n"
           "  node=0x%016" PRIx64 "  VID=0x%04x PID=0x%04x\r\n",
           slot + 1, d.payload[0] ? d.payload : "(pin/disc)", dev_display_name(d, nm, sizeof(nm)),
           d.node_id, d.vid, d.pid);
    if (!d.enumerated) { printf("  (not enumerated yet - type 'scan %d')\r\n", slot + 1); return; }
    for (int i = 0; i < d.n_eps; ++i) {
        EndpointInfo &e = *d.eps[i];
        printf("  Endpoint %u:", e.id);
        for (int t = 0; t < e.n_dt; ++t) {
            char b[40]; printf(" %s", devtype_label(e.device_types[t], b, sizeof(b)));
        }
        printf("\r\n");
        for (int c = 0; c < e.n_srv; ++c) {
            char clb[40];
            printf("    cluster %s  (%u attr, %u cmd)\r\n",
                   cluster_label(e.servers[c].id, clb, sizeof(clb)),
                   e.servers[c].n_attr, e.servers[c].n_cmd);
        }
    }
}

static void print_endpoint(DeviceInfo &d, uint16_t ep_id, int slot)
{
    EndpointInfo *e = dev_find_ep(d, ep_id);
    if (!e) { printf("[hub] #%d has no endpoint %u (try 'tree %d')\r\n", slot + 1, ep_id, slot + 1); return; }
    printf("[hub] #%d endpoint %u - device types:", slot + 1, ep_id);
    for (int t = 0; t < e->n_dt; ++t) { char b[40]; printf(" %s", devtype_label(e->device_types[t], b, sizeof(b))); }
    printf("\r\n  server clusters:\r\n");
    for (int c = 0; c < e->n_srv; ++c) {
        char clb[40];
        printf("    %s  (%u attr, %u cmd)\r\n",
               cluster_label(e->servers[c].id, clb, sizeof(clb)),
               e->servers[c].n_attr, e->servers[c].n_cmd);
    }
}

static void print_cluster(DeviceInfo &d, uint16_t ep_id, uint32_t cl_id, int slot)
{
    EndpointInfo *e = dev_find_ep(d, ep_id);
    ClusterInfo *c = e ? ep_find_cluster(*e, cl_id) : nullptr;
    if (!c) { printf("[hub] #%d ep %u has no cluster 0x%04lx (try 'ep %d %u')\r\n",
                     slot + 1, ep_id, (unsigned long)cl_id, slot + 1, ep_id); return; }
    char clb[40];
    printf("[hub] #%d ep %u cluster %s  (%u attributes, %u accepted commands)\r\n",
           slot + 1, ep_id, cluster_label(cl_id, clb, sizeof(clb)), c->n_attr, c->n_cmd);
    /* The id lists are no longer cached (v1.6 RAM diet) - live-read the cluster
     * instead, which shows every attribute with its label AND current value
     * (strictly more than the old cached listing). Reuses the `read` machinery. */
    printf("  reading live attributes...\r\n");
    s_read_req.node   = d.node_id;
    s_read_req.ep     = ep_id;
    s_read_req.cl     = cl_id;
    s_read_req.single = false;
    s_read_req.attr   = 0;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(read_work, 0);
    printf("  Invoke: 'invoke %d %u 0x%04lx <cmd>'\r\n",
           slot + 1, ep_id, (unsigned long)cl_id);
}

/* ===================================================================== *
 *  Command dispatch
 * ===================================================================== */

static uint32_t parse_id(const char *s) { return (uint32_t)strtoul(s, nullptr, 0); }   /* dec or 0x hex */

/* First endpoint on this device exposing the OnOff cluster, or -1. Lets the
 * on/off/toggle shortcuts work without the user knowing the endpoint (an IKEA
 * GRILLPLATS plug is ep1, but a multi-outlet/other device could differ). */
static int find_onoff_ep(DeviceInfo &d)
{
    for (int i = 0; i < d.n_eps; ++i)
        if (d.eps[i] && ep_find_cluster(*d.eps[i], CL_ONOFF)) return (int)d.eps[i]->id;
    return -1;
}

/* Resolve a 1-based slot argument to a paired device, or print an error. */
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

void inspector_print_help(void)
{
    printf("  devices | list       list paired devices (VID/PID/#eps)\r\n"
           "  device <n>           one-device summary\r\n"
           "  tree <n>             full endpoint -> cluster tree\r\n"
           "  ep <n> <ep>          device types + clusters of an endpoint\r\n"
           "  cluster <n> <ep> <cl>  attributes + accepted commands of a cluster\r\n"
           "  read <n> <ep> <cl> [attr]   live-read attribute value(s)\r\n"
           "  scan <n>             re-enumerate a device\r\n"
           "  on|off|toggle <n> [ep]  switch a device's OnOff cluster (ep auto-found)\r\n"
           "  write <n> <ep> <cl> <attr> <json>   write an attribute, e.g. {\"0:U8\":1}\r\n"
           "  invoke <n> <ep> <cl> <cmd> [json]   invoke a command, e.g. invoke 1 1 0x6 0x1\r\n");
}

bool inspector_handle_cmd(const char *line)
{
    /* Tokenize a private copy. */
    char buf[192];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *argv[10]; int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 10; t = strtok(nullptr, " ")) argv[argc++] = t;
    if (argc == 0) return false;
    const char *cmd = argv[0];

    if (!strcmp(cmd, "devices") || !strcmp(cmd, "list")) {
        inspector_print_devices();
        return true;
    }
    if (!strcmp(cmd, "device") && argc >= 2) {
        int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
        char nm[72];
        if (d) printf("[hub] #%d payload=%s \"%s\"  node=0x%016" PRIx64 " VID=0x%04x PID=0x%04x eps=%u %s\r\n",
                      slot + 1, d->payload[0] ? d->payload : "(pin/disc)", dev_display_name(*d, nm, sizeof(nm)),
                      d->node_id, d->vid, d->pid, d->n_eps, d->enumerated ? "" : "(not enumerated)");
        return true;
    }
    if (!strcmp(cmd, "tree") && argc >= 2) {
        int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
        if (d) print_tree(*d, slot);
        return true;
    }
    if (!strcmp(cmd, "ep") && argc >= 3) {
        int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
        if (d) print_endpoint(*d, (uint16_t)parse_id(argv[2]), slot);
        return true;
    }
    if (!strcmp(cmd, "cluster") && argc >= 4) {
        int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
        if (d) print_cluster(*d, (uint16_t)parse_id(argv[2]), parse_id(argv[3]), slot);
        return true;
    }
    if (!strcmp(cmd, "scan") && argc >= 2) {
        int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
        if (d) {
            d->enumerated  = false;
            d->enumerating = true;
            chip::DeviceLayer::PlatformMgr().ScheduleWork(enum_work, (intptr_t)(slot + 1));
            printf("[hub] re-enumerating #%d...\r\n", slot + 1);
        }
        return true;
    }
    if (!strcmp(cmd, "read") && argc >= 4) {
        int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
        if (d) {
            s_read_req.node   = d->node_id;
            s_read_req.ep     = (uint16_t)parse_id(argv[2]);
            s_read_req.cl     = parse_id(argv[3]);
            s_read_req.single = (argc >= 5);
            s_read_req.attr   = s_read_req.single ? parse_id(argv[4]) : 0;
            printf("[hub] reading #%d ep%u cl 0x%04lx %s...\r\n", slot + 1, s_read_req.ep,
                   (unsigned long)s_read_req.cl, s_read_req.single ? "" : "(all attrs)");
            chip::DeviceLayer::PlatformMgr().ScheduleWork(read_work, 0);
        }
        return true;
    }
    if (!strcmp(cmd, "write") && argc >= 6) {
        int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
        if (d) {
            s_write_req.node = d->node_id;
            s_write_req.ep   = (uint16_t)parse_id(argv[2]);
            s_write_req.cl   = parse_id(argv[3]);
            s_write_req.attr = parse_id(argv[4]);
            strncpy(s_write_req.json, argv[5], sizeof(s_write_req.json) - 1);
            s_write_req.json[sizeof(s_write_req.json) - 1] = '\0';
            printf("[hub] writing #%d ep%u cl 0x%04lx attr 0x%04lx = %s\r\n", slot + 1,
                   s_write_req.ep, (unsigned long)s_write_req.cl,
                   (unsigned long)s_write_req.attr, s_write_req.json);
            chip::DeviceLayer::PlatformMgr().ScheduleWork(write_work, 0);
        }
        return true;
    }
    /* on/off/toggle <n> [ep] - friendly wrappers over OnOff invoke. */
    if ((!strcmp(cmd, "on") || !strcmp(cmd, "off") || !strcmp(cmd, "toggle")) && argc >= 2) {
        int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
        if (!d) return true;
        int ep = (argc >= 3) ? (int)parse_id(argv[2]) : find_onoff_ep(*d);
        if (ep < 0) {
            printf("[hub] #%d has no OnOff cluster%s. Try '%s %d <ep>', or 'tree %d' to look.\r\n",
                   slot + 1, d->enumerated ? "" : " (not enumerated yet - try 'scan')",
                   cmd, slot + 1, slot + 1);
            return true;
        }
        s_invoke_req.node = d->node_id;
        s_invoke_req.ep   = (uint16_t)ep;
        s_invoke_req.cl   = CL_ONOFF;
        s_invoke_req.cmd  = !strcmp(cmd, "off") ? CMD_ONOFF_OFF
                          : !strcmp(cmd, "on")  ? CMD_ONOFF_ON
                                                : CMD_ONOFF_TOGGLE;
        s_invoke_req.json[0] = '\0';
        printf("[hub] %s #%d (ep%d)...\r\n", cmd, slot + 1, ep);
        chip::DeviceLayer::PlatformMgr().ScheduleWork(invoke_work, 0);
        return true;
    }

    if (!strcmp(cmd, "invoke") && argc >= 5) {
        int slot; DeviceInfo *d = slot_arg(argv[1], &slot);
        if (d) {
            s_invoke_req.node = d->node_id;
            s_invoke_req.ep   = (uint16_t)parse_id(argv[2]);
            s_invoke_req.cl   = parse_id(argv[3]);
            s_invoke_req.cmd  = parse_id(argv[4]);
            s_invoke_req.json[0] = '\0';
            if (argc >= 6) { strncpy(s_invoke_req.json, argv[5], sizeof(s_invoke_req.json) - 1);
                             s_invoke_req.json[sizeof(s_invoke_req.json) - 1] = '\0'; }
            char iclb[40], icmb[48];
            printf("[hub] invoking #%d ep%u %s %s args=%s\r\n", slot + 1, s_invoke_req.ep,
                   cluster_label(s_invoke_req.cl, iclb, sizeof(iclb)),
                   cmd_label(s_invoke_req.cl, s_invoke_req.cmd, icmb, sizeof(icmb)),
                   s_invoke_req.json[0] ? s_invoke_req.json : "{}");
            chip::DeviceLayer::PlatformMgr().ScheduleWork(invoke_work, 0);
        }
        return true;
    }

    return false;   /* not an inspector command */
}
