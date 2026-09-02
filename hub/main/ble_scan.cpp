/* ble_scan.cpp - see ble_scan.h. */
#include "ble_scan.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/ChipDeviceScanner.h>

static const char *TAG = "blescan";

#define MAX_SEEN 12

using chip::DeviceLayer::Internal::ChipDeviceScanner;
using chip::DeviceLayer::Internal::ChipDeviceScannerDelegate;

/* Collects commissionable devices seen during one scan, de-duplicated by
 * discriminator (a device advertises continuously while in pairing mode). */
class BleScanDelegate : public ChipDeviceScannerDelegate
{
public:
    void reset() { m_n = 0; }

#ifdef CONFIG_BT_NIMBLE_ENABLED
    void OnDeviceScanned(const struct ble_hs_adv_fields & /*fields*/, const ble_addr_t & /*addr*/,
                         const chip::Ble::ChipBLEDeviceIdentificationInfo &info) override
#else
    void OnDeviceScanned(esp_ble_addr_type_t & /*addr_type*/, esp_bd_addr_t & /*addr*/,
                         const chip::Ble::ChipBLEDeviceIdentificationInfo &info) override
#endif
    {
        uint16_t disc = info.GetDeviceDiscriminator();
        for (int i = 0; i < m_n; ++i) if (m_dev[i].discriminator == disc) return;   /* already reported */
        if (m_n < MAX_SEEN) {
            m_dev[m_n].discriminator = disc;
            m_dev[m_n].vid = (uint16_t)info.GetVendorId();
            m_dev[m_n].pid = (uint16_t)info.GetProductId();
            ++m_n;
        }
        if (m_console)   /* JSON mode collects silently and returns the list on completion */
            printf("[hub]   discriminator=%u (0x%03x)  VID=0x%04x  PID=0x%04x\r\n",
                   (unsigned)disc, (unsigned)disc,
                   (unsigned)info.GetVendorId(), (unsigned)info.GetProductId());
    }

    /* NOTE: CHIP's NimBLE scanner only calls OnScanComplete() from an explicit
     * StopScan() - a natural scan timeout (BLE_GAP_EVENT_DISC_COMPLETE) just
     * clears mIsScanning and never notifies the delegate. So we drive completion
     * ourselves from a timer (see ble_scan_finish) and keep this a no-op to
     * guarantee exactly one summary. */
    void OnScanComplete() override {}

    void set_console(bool on) { m_console = on; }
    const ble_scan_dev_t *devs() const { return m_dev; }
    int count() const { return m_n; }

    void print_summary()
    {
        if (m_n == 0) {
            printf("[hub] BLE scan complete - no devices in pairing mode found.\r\n"
                   "  Put the device into pairing mode and retry. If it never appears,\r\n"
                   "  the device is not advertising (not a hub problem).\r\n");
        } else {
            printf("[hub] BLE scan complete - %d device(s) in pairing mode.\r\n"
                   "  The passcode is NOT advertised (secret by design). Take the 8-digit\r\n"
                   "  passcode from the device label, then:\r\n"
                   "    pin <passcode> <discriminator>    then    pair\r\n", m_n);
        }
    }

private:
    ble_scan_dev_t m_dev[MAX_SEEN];
    int            m_n = 0;
    bool           m_console = true;
};

static BleScanDelegate s_delegate;
static uint16_t        s_secs = 5;
static ble_scan_done_fn s_on_done = nullptr;

/* Fires at the requested duration: stop the scan and deliver the result. */
static void ble_scan_finish(chip::System::Layer *, void *)
{
    ChipDeviceScanner::GetInstance().StopScan();   /* no-op if it already ended */
    if (s_on_done) s_on_done(s_delegate.devs(), s_delegate.count());
    else           s_delegate.print_summary();
}

/* Runs on the CHIP task - BLE APIs must not be called from the console task. */
static void ble_scan_work(intptr_t)
{
    s_delegate.reset();
    s_delegate.set_console(s_on_done == nullptr);
    auto &sc = ChipDeviceScanner::GetInstance();
    if (sc.Init(&s_delegate) != CHIP_NO_ERROR) {
        if (s_on_done) s_on_done(nullptr, -1);
        else printf("[hub] BLE scanner init failed\r\n");
        return;
    }
    /* Ask the stack for a slightly longer scan than we intend, then stop it
     * ourselves on our own timer - that way our timer always wins and we get a
     * deterministic single result (see the OnScanComplete note above). */
    CHIP_ERROR e = sc.StartScan((uint16_t)(s_secs + 5));
    if (e != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "StartScan failed (BLE busy or disabled?)");
        if (s_on_done) s_on_done(nullptr, -1);
        else printf("[hub] BLE scan failed to start: %" CHIP_ERROR_FORMAT "\r\n", e.Format());
        return;
    }
    if (s_on_done == nullptr)
        printf("[hub] BLE scanning %us for devices in pairing mode...\r\n", (unsigned)s_secs);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32((uint32_t)s_secs * 1000), ble_scan_finish, nullptr);
}

void ble_scan_start_ex(uint16_t seconds, ble_scan_done_fn on_done)
{
    if (seconds < 1)  seconds = 1;
    if (seconds > 60) seconds = 60;
    s_secs = seconds;
    s_on_done = on_done;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(ble_scan_work, 0);
}

void ble_scan_start(uint16_t seconds) { ble_scan_start_ex(seconds, nullptr); }
