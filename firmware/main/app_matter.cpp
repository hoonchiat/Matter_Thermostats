/*
 * app_matter.cpp — ESP-Matter data model for the Thermostat device.
 *
 * Creates the Root Node + Thermostat endpoint, wires the attribute-update and
 * identify callbacks to the app event queue, and provides the local->Matter
 * reporting helpers used by app_control.
 *
 * This layer tracks the installed ESP-Matter / connectedhomeip API. Points that
 * commonly shift between SDK versions are marked TODO(matter); cross-check them
 * against docs/MATTER.md and the version you build with.
 */
#include "app_priv.h"

#include "esp_log.h"
#include "esp_matter.h"
#include "esp_matter_console.h"
#include "esp_matter_ota.h"

#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

#define TAG "app_matter"

static uint16_t s_ep = 0;       /* thermostat endpoint id */
static uint16_t s_fan_ep = 0;   /* fan (Fan Control) endpoint id */
static uint16_t s_occ_ep = 0;   /* occupancy sensor endpoint id */

/* ---- mode conversion (Matter SystemMode <-> thermo_mode_t) ---------------- */

static int matter_to_mode(uint8_t sys_mode)
{
    switch (sys_mode) {
        case 0: return THERMO_MODE_OFF;
        case 1: return THERMO_MODE_AUTO;
        case 3: return THERMO_MODE_COOL;
        case 4: return THERMO_MODE_HEAT;
        case 5: return THERMO_MODE_HEAT;       /* EmergencyHeat -> Heat (v1) */
        case 7: return THERMO_MODE_FAN_ONLY;
        default: return THERMO_MODE_OFF;
    }
}
static uint8_t mode_to_matter(int mode)
{
    switch (mode) {
        case THERMO_MODE_OFF:      return 0;
        case THERMO_MODE_AUTO:     return 1;
        case THERMO_MODE_COOL:     return 3;
        case THERMO_MODE_HEAT:     return 4;
        case THERMO_MODE_FAN_ONLY: return 7;
        default: return 0;
    }
}

/* ---- fan conversion (Matter FanControl::FanMode <-> thermo_fan_speed_t) ----
 * FanMode: Off=0, Low=1, Medium=2, High=3, On=4, Auto=5, Smart=6.            */
static int matter_to_fan(uint8_t fan_mode)
{
    switch (fan_mode) {
        case 1:  return THERMO_FAN_LOW;
        case 2:  return THERMO_FAN_MED;
        case 3:  return THERMO_FAN_HIGH;
        case 4:  return THERMO_FAN_HIGH;   /* On    -> High */
        case 0:                            /* Off   -> Auto (fan follows call) */
        case 5:                            /* Auto  -> Auto */
        default: return THERMO_FAN_AUTO;
    }
}
static uint8_t fan_to_matter(int fan_speed)
{
    switch (fan_speed) {
        case THERMO_FAN_LOW:  return 1;
        case THERMO_FAN_MED:  return 2;
        case THERMO_FAN_HIGH: return 3;
        case THERMO_FAN_AUTO:
        default:              return 5;    /* Auto */
    }
}

/* ---- callbacks ------------------------------------------------------------ */

/* Remote writes land here (PRE_UPDATE = before the stack commits the value). */
static esp_err_t app_attribute_update_cb(callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv)
{
    if (type != PRE_UPDATE) return ESP_OK;

    /* --- Thermostat endpoint: mode / setpoints / display units (remote override) */
    if (endpoint_id == s_ep) {
        if (cluster_id == Thermostat::Id) {
            if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
                app_post_event(EVT_MATTER_SET_MODE, matter_to_mode(val->val.u8));
            } else if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
                app_post_event(EVT_MATTER_SET_HEAT, val->val.i16);
            } else if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
                app_post_event(EVT_MATTER_SET_COOL, val->val.i16);
            }
        } else if (cluster_id == ThermostatUserInterfaceConfiguration::Id) {
            if (attribute_id ==
                ThermostatUserInterfaceConfiguration::Attributes::TemperatureDisplayMode::Id) {
                app_post_event(EVT_MATTER_SET_UNITS, val->val.u8 /* 0=C, 1=F */);
            }
        }
    }
    /* --- Fan endpoint: fan speed (remote override) ------------------------- */
    else if (endpoint_id == s_fan_ep && cluster_id == FanControl::Id) {
        if (attribute_id == FanControl::Attributes::FanMode::Id) {
            app_post_event(EVT_MATTER_SET_FAN, matter_to_fan(val->val.u8));
        } else if (attribute_id == FanControl::Attributes::PercentSetting::Id) {
            /* Map a 0..100% request onto our discrete levels. */
            int pct = val->val.u8;
            int lvl = (pct == 0) ? THERMO_FAN_AUTO
                    : (pct <= 33) ? THERMO_FAN_LOW
                    : (pct <= 66) ? THERMO_FAN_MED
                                  : THERMO_FAN_HIGH;
            app_post_event(EVT_MATTER_SET_FAN, lvl);
        }
    }
    return ESP_OK;
}

static esp_err_t app_identification_cb(identification::callback_type_t type,
                                       uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv)
{
    ESP_LOGI(TAG, "identify: ep=%u effect=%u", endpoint_id, effect_id);
    /* TODO(app): blink the status LED while identifying. */
    return ESP_OK;
}

/* CHIP device events (commissioning / connectivity).
 * Mirrors the esp-matter `light` example's app_event_cb so pairing behaves
 * identically: BLE commissioning, and on the last fabric being removed we
 * re-open a (DNS-SD) basic commissioning window so the device can be re-added. */
static void app_device_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
        case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
            ESP_LOGI(TAG, "commissioning complete");
            app_post_event(EVT_MATTER_COMMISSIONED, 1);
            break;
        case chip::DeviceLayer::DeviceEventType::kThreadConnectivityChange:
            ESP_LOGI(TAG, "thread connectivity change");
            break;
        case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
            ESP_LOGI(TAG, "interface IP address changed");
            break;
        case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
            ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
            break;
        case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
            ESP_LOGI(TAG, "commissioning window opened");
            break;
        case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
            ESP_LOGI(TAG, "commissioning window closed");
            break;
        case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
            ESP_LOGI(TAG, "fabric removed");
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
                app_post_event(EVT_MATTER_COMMISSIONED, 0);
                /* Re-open commissioning (DNS-SD only) like the light example. */
                auto &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
                constexpr auto kTimeout = chip::System::Clock::Seconds16(300);
                if (!mgr.IsCommissioningWindowOpen()) {
                    mgr.OpenBasicCommissioningWindow(
                        kTimeout, chip::CommissioningWindowAdvertisement::kDnssdOnly);
                }
            }
            break;
        }
        default:
            break;
    }
}

/* ---- endpoint/cluster creation ------------------------------------------- */

int app_matter_start(void)
{
    node::config_t node_cfg;
    node_t *node = node::create(&node_cfg, app_attribute_update_cb, app_identification_cb);
    if (!node) { ESP_LOGE(TAG, "node create failed"); return ESP_FAIL; }

    thermostat::config_t th_cfg;
    /* Seed defaults from persisted config so the model matches local state. */
    app_lock();
    th_cfg.thermostat.occupied_heating_setpoint = (int16_t)g_state.cfg.heat_set_c100;
    th_cfg.thermostat.occupied_cooling_setpoint = (int16_t)g_state.cfg.cool_set_c100;
    th_cfg.thermostat.control_sequence_of_operation = 4;   /* Cooling & Heating */
    th_cfg.thermostat.system_mode = mode_to_matter(g_state.cfg.mode);
    app_unlock();

    endpoint_t *ep = thermostat::create(node, &th_cfg, ENDPOINT_FLAG_NONE, NULL);
    if (!ep) { ESP_LOGE(TAG, "thermostat endpoint failed"); return ESP_FAIL; }
    s_ep = endpoint::get_id(ep);

    /* TODO(matter): enable the HEAT|COOL|AUTO feature flags and add the
     * Thermostat User Interface Configuration cluster + setpoint-limit attrs
     * using the feature/cluster helpers for your esp-matter version, e.g.:
     *   cluster::thermostat::feature::heating::add(...);
     *   cluster::thermostat::feature::cooling::add(...);
     *   cluster::thermostat::feature::auto_mode::add(...);
     * See docs/MATTER.md for the exact attribute set. */

    /* Endpoint 2: a Fan device (0x002B) exposing the Fan Control cluster so the
     * fan speed (Auto/Low/Med/High) can be viewed and OVERRIDDEN from Matter. */
    fan::config_t fan_cfg;
    app_lock();
    fan_cfg.fan_control.fan_mode = fan_to_matter(g_state.cfg.fan_speed);
    app_unlock();
    /* FanModeSequence = 2 => Off/Low/Med/High/Auto (matches our levels). */
    fan_cfg.fan_control.fan_mode_sequence = 2;
    endpoint_t *fep = fan::create(node, &fan_cfg, ENDPOINT_FLAG_NONE, NULL);
    if (!fep) { ESP_LOGE(TAG, "fan endpoint failed"); return ESP_FAIL; }
    s_fan_ep = endpoint::get_id(fep);
    /* TODO(matter): if your esp-matter version needs it, add the FanControl
     * AUTO feature (cluster::fan_control::feature::auto::add(...)) so FanMode
     * Auto is advertised as supported. */

    /* Endpoint 3: an Occupancy Sensor (0x0107) exposing the resolved Home/Away
     * state (from the PIR sensor or the manual toggle) to the ecosystem. */
    /* TODO(matter): occupancy_sensor::config_t field names track the SDK version;
     * cross-check occupancy_sensing.occupancy / occupancy_sensor_type against yours. */
    occupancy_sensor::config_t occ_cfg;
    app_lock();
    occ_cfg.occupancy_sensing.occupancy = g_state.cfg.occ_manual_home ? 1 : 0;
    app_unlock();
    endpoint_t *oep = occupancy_sensor::create(node, &occ_cfg, ENDPOINT_FLAG_NONE, NULL);
    if (!oep) { ESP_LOGE(TAG, "occupancy endpoint failed"); return ESP_FAIL; }
    s_occ_ep = endpoint::get_id(oep);

    esp_matter::start(app_device_event_cb);

    /* Print the QR + manual pairing code to the console, same as the light
     * example. Commissioning is BLE, then the device joins Thread. */
    PrintOnboardingCodes(
        chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::init();
#endif

    ESP_LOGI(TAG, "matter started: thermostat ep=%u, fan ep=%u, occ ep=%u",
             s_ep, s_fan_ep, s_occ_ep);
    return ESP_OK;
}

/* ---- local -> Matter reporting helpers ------------------------------------ */

static void update_i16(uint32_t cluster, uint32_t attr, int16_t v)
{
    esp_matter_attr_val_t val = esp_matter_int16(v);
    attribute::update(s_ep, cluster, attr, &val);
}

void app_matter_report_temperature(int temp_c100, bool fault)
{
    if (!s_ep) return;
    esp_matter_attr_val_t val;
    if (fault) {
        val = esp_matter_nullable_int16(nullable<int16_t>());          /* null */
    } else {
        val = esp_matter_nullable_int16(nullable<int16_t>((int16_t)temp_c100));
    }
    attribute::update(s_ep, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, &val);
}

void app_matter_report_running_state(bool heat, bool cool, bool fan)
{
    if (!s_ep) return;
    uint16_t bits = (heat ? 0x01 : 0) | (cool ? 0x02 : 0) | (fan ? 0x04 : 0);
    esp_matter_attr_val_t val = esp_matter_bitmap16(bits);
    attribute::update(s_ep, Thermostat::Id,
                      Thermostat::Attributes::ThermostatRunningState::Id, &val);
}

void app_matter_report_setpoints(int heat_c100, int cool_c100)
{
    if (!s_ep) return;
    update_i16(Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id, (int16_t)heat_c100);
    update_i16(Thermostat::Id, Thermostat::Attributes::OccupiedCoolingSetpoint::Id, (int16_t)cool_c100);
}

void app_matter_report_mode(int mode)
{
    if (!s_ep) return;
    esp_matter_attr_val_t val = esp_matter_enum8(mode_to_matter(mode));
    attribute::update(s_ep, Thermostat::Id, Thermostat::Attributes::SystemMode::Id, &val);
}

void app_matter_report_units(bool fahrenheit)
{
    if (!s_ep) return;
    esp_matter_attr_val_t val = esp_matter_enum8(fahrenheit ? 1 : 0);   /* 0=C, 1=F */
    attribute::update(s_ep, ThermostatUserInterfaceConfiguration::Id,
                      ThermostatUserInterfaceConfiguration::Attributes::TemperatureDisplayMode::Id,
                      &val);
}

void app_matter_report_fan(int fan_speed)
{
    if (!s_fan_ep) return;
    esp_matter_attr_val_t val = esp_matter_enum8(fan_to_matter(fan_speed));
    attribute::update(s_fan_ep, FanControl::Id, FanControl::Attributes::FanMode::Id, &val);
}

void app_matter_report_occupancy(bool occupied)
{
    if (!s_occ_ep) return;
    /* Occupancy attribute is a bitmap8; bit0 = occupied. */
    esp_matter_attr_val_t val = esp_matter_bitmap8(occupied ? 1 : 0);
    attribute::update(s_occ_ep, OccupancySensing::Id,
                      OccupancySensing::Attributes::Occupancy::Id, &val);
}

void app_matter_factory_reset(void)
{
    esp_matter::factory_reset();   /* clears fabrics + Thread creds, then reboots */
}

void app_matter_get_pairing_code(char *out, int out_len)
{
    if (!out || out_len <= 0) return;
    out[0] = '\0';

    /* Build the 11-digit manual pairing code from the commissionable data. */
    chip::SetupPayload payload;
    uint32_t passcode = 0; uint16_t disc = 0;
    auto *provider = chip::DeviceLayer::GetCommissionableDataProvider();
    if (provider &&
        provider->GetSetupPasscode(passcode) == CHIP_NO_ERROR &&
        provider->GetSetupDiscriminator(disc) == CHIP_NO_ERROR) {
        payload.setUpPINCode = passcode;
        payload.discriminator.SetLongValue(disc);
        payload.rendezvousInformation.SetValue(chip::RendezvousInformationFlag::kBLE);

        std::string code;
        if (chip::ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(code)
                == CHIP_NO_ERROR) {
            strncpy(out, code.c_str(), out_len - 1);
            out[out_len - 1] = '\0';
        }
    }
}
