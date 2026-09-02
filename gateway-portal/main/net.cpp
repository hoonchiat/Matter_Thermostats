/*
 * net.cpp - Wi-Fi STA + mDNS + SNTP (see net.h).
 *
 * Office Wi-Fi credentials are compiled in (single-user, single-network appliance).
 */
#include "net.h"

#include <cstring>
#include <cstdio>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "cfg.h"

static const char *TAG = "net";

/* Wi-Fi STA + SoftAP credentials, hostname, NTP and tz all come from cfg (persisted,
 * editable from the Configuration tab); defaults live in cfg.cpp. The SoftAP (APSTA)
 * means the portal is always reachable at 192.168.4.1 with a captive popup - no
 * mDNS/multicast, router, or IP-discovery needed. */
#define AP_SSID_PREFIX   "MatterGateway"    /* used when cfg.ap_ssid is empty (auto) */
#define AP_IP_A          192
#define AP_IP_B          168
#define AP_IP_C          4
#define AP_IP_D          1

static volatile bool s_connected  = false;
static volatile bool s_time_valid = false;
static char          s_ip[16]      = "0.0.0.0";
static char          s_ap_ssid[33] = AP_SSID_PREFIX;

bool        net_wifi_connected(void) { return s_connected; }
bool        net_time_valid(void)     { return s_time_valid; }
const char *net_ip_str(void)         { return s_ip; }
int         net_tz_offset_min(void)  { return cfg_get()->tz_min; }
const char *net_ap_ssid(void)        { return s_ap_ssid; }

static void on_time_sync(struct timeval *tv)
{
    s_time_valid = true;
    ESP_LOGI(TAG, "SNTP synced (utc epoch %lld)", (long long)(tv ? tv->tv_sec : 0));
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* connect is kicked off explicitly after the diagnostic scan in net_start() */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        strcpy(s_ip, "0.0.0.0");
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d, rssi=%d) - reconnecting",
                 e ? e->reason : -1, e ? e->rssi : 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "Got IP %s  (reach the portal at http://%s.local/ or the SoftAP)", s_ip, cfg_get()->hostname);
    }
}

/* One-time diagnostic: list the 2.4GHz APs the S3 can actually see, so a failed
 * association can be told apart from the AP simply not being in range. */
static void scan_and_log(void)
{
    wifi_scan_config_t sc = {};        /* all channels, active scan */
    ESP_LOGI(TAG, "scanning for 2.4GHz APs...");
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { ESP_LOGW(TAG, "scan failed"); return; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t recs[20];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) { ESP_LOGW(TAG, "get records failed"); return; }
    ESP_LOGI(TAG, "found %u AP(s):", (unsigned)n);
    bool target = false;
    for (int i = 0; i < n; ++i) {
        ESP_LOGI(TAG, "  ch%2d rssi %4d auth%d  \"%s\"",
                 recs[i].primary, recs[i].rssi, recs[i].authmode, (char *)recs[i].ssid);
        if (strcmp((char *)recs[i].ssid, cfg_get()->sta_ssid) == 0) target = true;
    }
    ESP_LOGW(TAG, "target SSID \"%s\" %s in the 2.4GHz scan", cfg_get()->sta_ssid, target ? "FOUND" : "NOT found");
}

/* Captive-portal DNS: answer every A query with the SoftAP IP (192.168.4.1) so any
 * URL a client on our AP opens lands on the portal, and the OS shows the captive
 * popup automatically. Only AP clients query us (the AP's DHCP hands out the S3 as
 * their DNS); STA-side name resolution is untouched. */
static void captive_dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { ESP_LOGW(TAG, "captive dns: socket failed"); vTaskDelete(nullptr); return; }
    struct sockaddr_in sa = {};
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons(53);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGW(TAG, "captive dns: bind :53 failed"); close(s); vTaskDelete(nullptr); return;
    }
    ESP_LOGI(TAG, "captive DNS up (all names -> %d.%d.%d.%d)", AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D);
    uint8_t buf[320];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;                 /* already a response */
        /* find end of the (single) question: QNAME labels, 0, QTYPE(2), QCLASS(2) */
        int q = 12;
        while (q < n && buf[q]) q += buf[q] + 1;
        q += 1 + 4;
        if (q > n) continue;
        bool is_a = (buf[q - 4] == 0x00 && buf[q - 3] == 0x01);   /* QTYPE == A */
        buf[2] = 0x81; buf[3] = 0x80;                /* QR=1, RD=1, RA=1 */
        buf[6] = 0; buf[7] = is_a ? 1 : 0;           /* ANCOUNT */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;     /* NSCOUNT=0, ARCOUNT=0 (drop any OPT) */
        int p = q;
        if (is_a && p + 16 <= (int)sizeof(buf)) {
            buf[p++] = 0xC0; buf[p++] = 0x0C;                     /* name -> offset 12 */
            buf[p++] = 0x00; buf[p++] = 0x01;                     /* type A */
            buf[p++] = 0x00; buf[p++] = 0x01;                     /* class IN */
            buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3C;  /* TTL 60 */
            buf[p++] = 0x00; buf[p++] = 0x04;                     /* RDLENGTH 4 */
            buf[p++] = AP_IP_A; buf[p++] = AP_IP_B; buf[p++] = AP_IP_C; buf[p++] = AP_IP_D;
        }
        sendto(s, buf, p, 0, (struct sockaddr *)&from, fl);
    }
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_evt, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_evt, nullptr, nullptr));

    const portal_cfg_t *c = cfg_get();
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid,     c->sta_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, c->sta_pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;        /* forward-compat with WPA3 APs */

    /* SoftAP SSID: cfg override, else "MatterGateway-XXXX" (MAC suffix). WPA2. */
    if (c->ap_ssid[0]) {
        strncpy(s_ap_ssid, c->ap_ssid, sizeof(s_ap_ssid) - 1);
        s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';
    } else {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
    }
    wifi_config_t ap = {};
    strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len       = strlen(s_ap_ssid);
    strncpy((char *)ap.ap.password, c->ap_pass, sizeof(ap.ap.password) - 1);
    ap.ap.channel        = 1;                      /* moves to the STA's channel once joined */
    ap.ap.max_connection = 4;
    ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* AP DHCP: hand out the S3 itself as the DNS server so the captive DNS catches
     * every lookup from clients on our AP. */
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D);
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer_dns = 0x02;                       /* OFFER_DNS */
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));
    esp_netif_dhcps_start(ap_netif);
    xTaskCreate(captive_dns_task, "captdns", 4096, nullptr, 4, nullptr);

    scan_and_log();               /* diagnostic: what can the S3 actually see? */
    esp_wifi_connect();           /* now attempt to join WIFI_SSID */

    /* mDNS: <hostname>.local. Services (the HTTP server) are added in Phase 3. */
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(c->hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set("Matter Gateway Portal"));

    /* SNTP: begin syncing as soon as the network is up. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(c->ntp);
    sntp_cfg.sync_cb = on_time_sync;
    sntp_cfg.start   = true;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

    ESP_LOGI(TAG, "net started: SoftAP \"%s\" (portal http://%d.%d.%d.%d/), STA -> \"%s\", "
             "mDNS %s.local, SNTP %s, tz %+d min",
             s_ap_ssid, AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D, c->sta_ssid, c->hostname,
             c->ntp, c->tz_min);
}
