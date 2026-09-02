/*
 * matter_names.h - best-effort human labels for Matter cluster / device-type
 * ids. Only common ids are named; anything unknown renders numeric (0x....).
 * Kept small and static (flash .rodata, no RAM). Not exhaustive - extend as
 * needed. Ids from the Matter 1.x Application Cluster / Device Library specs.
 */
#pragma once

#include <cstdint>
#include <cstdio>

struct id_name_t { uint32_t id; const char *name; };

/* ---- clusters (server) ---------------------------------------------------- */
static const id_name_t k_cluster_names[] = {
    {0x0003, "Identify"},
    {0x0004, "Groups"},
    {0x0006, "OnOff"},
    {0x0008, "LevelControl"},
    {0x001D, "Descriptor"},
    {0x001E, "Binding"},
    {0x001F, "AccessControl"},
    {0x0028, "BasicInformation"},
    {0x0029, "OTASoftwareUpdateProvider"},
    {0x002A, "OTASoftwareUpdateRequestor"},
    {0x002B, "LocalizationConfiguration"},
    {0x002C, "TimeFormatLocalization"},
    {0x002E, "PowerSourceConfiguration"},
    {0x002F, "PowerSource"},
    {0x0030, "GeneralCommissioning"},
    {0x0031, "NetworkCommissioning"},
    {0x0033, "GeneralDiagnostics"},
    {0x0034, "SoftwareDiagnostics"},
    {0x0035, "ThreadNetworkDiagnostics"},
    {0x0037, "EthernetNetworkDiagnostics"},
    {0x0038, "TimeSynchronization"},
    {0x0039, "BridgedDeviceBasicInformation"},
    {0x003C, "AdministratorCommissioning"},
    {0x003E, "OperationalCredentials"},
    {0x003F, "GroupKeyManagement"},
    {0x0040, "FixedLabel"},
    {0x0041, "UserLabel"},
    {0x0045, "BooleanState"},
    {0x0050, "ModeSelect"},
    {0x0101, "DoorLock"},
    {0x0102, "WindowCovering"},
    {0x0201, "Thermostat"},
    {0x0202, "FanControl"},
    {0x0300, "ColorControl"},
    {0x0400, "IlluminanceMeasurement"},
    {0x0402, "TemperatureMeasurement"},
    {0x0403, "PressureMeasurement"},
    {0x0405, "RelativeHumidityMeasurement"},
    {0x0406, "OccupancySensing"},
    {0x040C, "CarbonMonoxideConcentration"},
    {0x040D, "CarbonDioxideConcentration"},
    {0x042A, "Pm25ConcentrationMeasurement"},
    {0x042B, "FormaldehydeConcentration"},
    {0x042C, "Pm1ConcentrationMeasurement"},
    {0x005B, "AirQuality"},
    {0x003B, "Switch"},
    {0x0046, "IcdManagement"},
};

/* ---- device types --------------------------------------------------------- */
static const id_name_t k_devtype_names[] = {
    {0x0016, "RootNode"},
    {0x0011, "PowerSource"},
    {0x000E, "AggregatorBridge"},
    {0x0013, "BridgedNode"},
    {0x0100, "OnOffLight"},
    {0x0101, "DimmableLight"},
    {0x010C, "ColorTemperatureLight"},
    {0x010D, "ExtendedColorLight"},
    {0x0103, "OnOffLightSwitch"},
    {0x010A, "OnOffPlugInUnit"},
    {0x010B, "DimmablePlugInUnit"},
    {0x0015, "ContactSensor"},
    {0x0106, "LightSensor"},
    {0x0107, "OccupancySensor"},
    {0x0302, "TemperatureSensor"},
    {0x0307, "HumiditySensor"},
    {0x0305, "PressureSensor"},
    {0x0076, "SmokeCOAlarm"},
    {0x002C, "AirQualitySensor"},
    {0x0202, "WindowCovering"},
    {0x000A, "DoorLock"},
    {0x0301, "Thermostat"},
    {0x002B, "Fan"},
    {0x000F, "GenericSwitch"},
    {0x0012, "OTARequestor"},
};

static inline const char *lookup_name(const id_name_t *tbl, size_t n, uint32_t id)
{
    for (size_t i = 0; i < n; ++i) if (tbl[i].id == id) return tbl[i].name;
    return nullptr;
}

static inline const char *cluster_name(uint32_t id)
{
    return lookup_name(k_cluster_names, sizeof(k_cluster_names)/sizeof(k_cluster_names[0]), id);
}

static inline const char *devtype_name(uint32_t id)
{
    return lookup_name(k_devtype_names, sizeof(k_devtype_names)/sizeof(k_devtype_names[0]), id);
}

/* Format "<name> (0x0006)" or just "0x0006" when unknown, into buf. */
static inline const char *cluster_label(uint32_t id, char *buf, size_t n)
{
    const char *nm = cluster_name(id);
    if (nm) snprintf(buf, n, "%s (0x%04lx)", nm, (unsigned long)id);
    else    snprintf(buf, n, "0x%04lx", (unsigned long)id);
    return buf;
}

static inline const char *devtype_label(uint32_t id, char *buf, size_t n)
{
    const char *nm = devtype_name(id);
    if (nm) snprintf(buf, n, "%s (0x%04lx)", nm, (unsigned long)id);
    else    snprintf(buf, n, "0x%04lx", (unsigned long)id);
    return buf;
}

/* ===================================================================== *
 *  Per-cluster attribute names.
 *
 *  Attribute ids are only unique WITHIN a cluster (0x0000 is MeasuredValue in
 *  TemperatureMeasurement but DataModelRevision in BasicInformation), so the
 *  lookup is keyed by (cluster, attr). The six 0xFFFx global attributes are the
 *  same in every cluster and handled first. All static (flash .rodata, no RAM).
 *  Not exhaustive; unknown ids render numeric. See docs / Matter cluster spec.
 * ===================================================================== */

/* Attributes shared by simple measurement clusters (Temperature 0x0402,
 * RelativeHumidity 0x0405, Pressure 0x0403, Flow 0x0404, ...). */
static const id_name_t k_attr_measurement[] = {
    {0x0000, "MeasuredValue"}, {0x0001, "MinMeasuredValue"},
    {0x0002, "MaxMeasuredValue"}, {0x0003, "Tolerance"},
};

/* Attributes shared by every Concentration Measurement cluster
 * (CarbonMonoxide 0x040C, CarbonDioxide 0x040D, PM2.5 0x042A, PM1 0x042C,
 * PM10 0x042D, TVOC 0x042E, Formaldehyde 0x042B, NO2, Ozone, Radon, ...). */
static const id_name_t k_attr_concentration[] = {
    {0x0000, "MeasuredValue"}, {0x0001, "MinMeasuredValue"}, {0x0002, "MaxMeasuredValue"},
    {0x0003, "PeakMeasuredValue"}, {0x0004, "PeakMeasuredValueWindow"},
    {0x0005, "AverageMeasuredValue"}, {0x0006, "AverageMeasuredValueWindow"},
    {0x0007, "Uncertainty"}, {0x0008, "MeasurementUnit"},
    {0x0009, "MeasurementMedium"}, {0x000A, "LevelValue"},
};

static const id_name_t k_attr_identify[] = {
    {0x0000, "IdentifyTime"}, {0x0001, "IdentifyType"},
};
static const id_name_t k_attr_onoff[] = {
    {0x0000, "OnOff"}, {0x4000, "GlobalSceneControl"}, {0x4001, "OnTime"},
    {0x4002, "OffWaitTime"}, {0x4003, "StartUpOnOff"},
};
static const id_name_t k_attr_descriptor[] = {
    {0x0000, "DeviceTypeList"}, {0x0001, "ServerList"},
    {0x0002, "ClientList"}, {0x0003, "PartsList"}, {0x0004, "TagList"},
};
static const id_name_t k_attr_accesscontrol[] = {
    {0x0000, "ACL"}, {0x0001, "Extension"}, {0x0002, "SubjectsPerAccessControlEntry"},
    {0x0003, "TargetsPerAccessControlEntry"}, {0x0004, "AccessControlEntriesPerFabric"},
};
static const id_name_t k_attr_basicinfo[] = {
    {0x0000, "DataModelRevision"}, {0x0001, "VendorName"}, {0x0002, "VendorID"},
    {0x0003, "ProductName"}, {0x0004, "ProductID"}, {0x0005, "NodeLabel"},
    {0x0006, "Location"}, {0x0007, "HardwareVersion"}, {0x0008, "HardwareVersionString"},
    {0x0009, "SoftwareVersion"}, {0x000A, "SoftwareVersionString"}, {0x000B, "ManufacturingDate"},
    {0x000C, "PartNumber"}, {0x000D, "ProductURL"}, {0x000E, "ProductLabel"},
    {0x000F, "SerialNumber"}, {0x0010, "LocalConfigDisabled"}, {0x0011, "Reachable"},
    {0x0012, "UniqueID"}, {0x0013, "CapabilityMinima"}, {0x0014, "ProductAppearance"},
    {0x0015, "SpecificationVersion"}, {0x0016, "MaxPathsPerInvoke"},
};
static const id_name_t k_attr_ota[] = {
    {0x0000, "DefaultOTAProviders"}, {0x0001, "UpdatePossible"},
    {0x0002, "UpdateState"}, {0x0003, "UpdateStateProgress"},
};
static const id_name_t k_attr_gencomm[] = {
    {0x0000, "Breadcrumb"}, {0x0001, "BasicCommissioningInfo"}, {0x0002, "RegulatoryConfig"},
    {0x0003, "LocationCapability"}, {0x0004, "SupportsConcurrentConnection"},
};
static const id_name_t k_attr_netcomm[] = {
    {0x0000, "MaxNetworks"}, {0x0001, "Networks"}, {0x0002, "ScanMaxTimeSeconds"},
    {0x0003, "ConnectMaxTimeSeconds"}, {0x0004, "InterfaceEnabled"},
    {0x0005, "LastNetworkingStatus"}, {0x0006, "LastNetworkID"}, {0x0007, "LastConnectErrorValue"},
    {0x0008, "SupportedThreadFeatures"}, {0x0009, "ThreadVersion"},
};
static const id_name_t k_attr_gendiag[] = {
    {0x0000, "NetworkInterfaces"}, {0x0001, "RebootCount"}, {0x0002, "UpTime"},
    {0x0003, "TotalOperationalHours"}, {0x0004, "BootReason"}, {0x0005, "ActiveHardwareFaults"},
    {0x0006, "ActiveRadioFaults"}, {0x0007, "ActiveNetworkFaults"}, {0x0008, "TestEventTriggersEnabled"},
};
static const id_name_t k_attr_threaddiag[] = {
    {0x0000, "Channel"}, {0x0001, "RoutingRole"}, {0x0002, "NetworkName"}, {0x0003, "PanId"},
    {0x0004, "ExtendedPanId"}, {0x0005, "MeshLocalPrefix"}, {0x0006, "OverrunCount"},
    {0x0007, "NeighborTable"}, {0x0008, "RouteTable"}, {0x0009, "PartitionId"},
    {0x000A, "Weighting"}, {0x000B, "DataVersion"}, {0x000C, "StableDataVersion"},
    {0x000D, "LeaderRouterId"}, {0x0038, "SecurityPolicy"}, {0x0039, "ChannelPage0Mask"},
    {0x003A, "OperationalDatasetComponents"}, {0x003B, "ActiveNetworkFaultsList"},
};
static const id_name_t k_attr_timesync[] = {
    {0x0000, "UTCTime"}, {0x0001, "Granularity"}, {0x0002, "TimeSource"},
    {0x0003, "TrustedTimeSource"}, {0x0004, "DefaultNTP"}, {0x0005, "TimeZone"},
    {0x0006, "DSTOffset"}, {0x0007, "LocalTime"}, {0x0008, "TimeZoneDatabase"},
    {0x0009, "NTPServerAvailable"}, {0x000A, "TimeZoneListMaxSize"},
    {0x000B, "DSTOffsetListMaxSize"}, {0x000C, "SupportsDNSResolve"},
};
static const id_name_t k_attr_admincomm[] = {
    {0x0000, "WindowStatus"}, {0x0001, "AdminFabricIndex"}, {0x0002, "AdminVendorId"},
};
static const id_name_t k_attr_opcreds[] = {
    {0x0000, "NOCs"}, {0x0001, "Fabrics"}, {0x0002, "SupportedFabrics"},
    {0x0003, "CommissionedFabrics"}, {0x0004, "TrustedRootCertificates"}, {0x0005, "CurrentFabricIndex"},
};
static const id_name_t k_attr_groupkey[] = {
    {0x0000, "GroupKeyMap"}, {0x0001, "GroupTable"},
    {0x0002, "MaxGroupsPerFabric"}, {0x0003, "MaxGroupKeysPerFabric"},
};
static const id_name_t k_attr_airquality[] = {
    {0x0000, "AirQuality"},
};
static const id_name_t k_attr_switch[] = {
    {0x0000, "NumberOfPositions"}, {0x0001, "CurrentPosition"}, {0x0002, "MultiPressMax"},
};
/* PowerSource: wired attrs 0x03-0x0A, battery attrs 0x0B+. NOTE BatPercentRemaining
 * is in HALF-percent units (0..200), so 114 means 57%. */
static const id_name_t k_attr_powersource[] = {
    {0x0000, "Status"}, {0x0001, "Order"}, {0x0002, "Description"},
    {0x0003, "WiredAssessedInputVoltage"}, {0x0004, "WiredAssessedInputFrequency"},
    {0x0005, "WiredCurrentType"}, {0x0006, "WiredAssessedCurrent"},
    {0x0007, "WiredNominalVoltage"}, {0x0008, "WiredMaximumCurrent"},
    {0x0009, "WiredPresent"}, {0x000A, "ActiveWiredFaults"},
    {0x000B, "BatVoltage"}, {0x000C, "BatPercentRemaining"}, {0x000D, "BatTimeRemaining"},
    {0x000E, "BatChargeLevel"}, {0x000F, "BatReplacementNeeded"}, {0x0010, "BatReplaceability"},
    {0x0011, "BatPresent"}, {0x0012, "ActiveBatFaults"}, {0x0013, "BatReplacementDescription"},
    {0x0014, "BatCommonDesignation"}, {0x0015, "BatANSIDesignation"}, {0x0016, "BatIECDesignation"},
    {0x0017, "BatApprovedChemistry"}, {0x0018, "BatCapacity"}, {0x0019, "BatQuantity"},
    {0x001A, "BatChargeState"}, {0x001B, "BatTimeToFullCharge"},
    {0x001C, "BatFunctionalWhileCharging"}, {0x001D, "BatChargingCurrent"},
    {0x001E, "ActiveBatChargeFaults"}, {0x001F, "EndpointList"},
};
/* ICD Management - sleepy ("intermittently connected") battery devices. */
static const id_name_t k_attr_icd[] = {
    {0x0000, "IdleModeDuration"}, {0x0001, "ActiveModeDuration"}, {0x0002, "ActiveModeThreshold"},
    {0x0003, "RegisteredClients"}, {0x0004, "ICDCounter"}, {0x0005, "ClientsSupportedPerFabric"},
    {0x0006, "UserActiveModeTriggerHint"}, {0x0007, "UserActiveModeTriggerInstruction"},
    {0x0008, "OperatingMode"}, {0x0009, "MaximumCheckInBackOff"},
};

/* cluster -> its attribute table */
struct cluster_attr_tab_t { uint32_t cluster; const id_name_t *tbl; size_t count; };
#define ATTRTAB(cl, arr) { (cl), (arr), sizeof(arr)/sizeof((arr)[0]) }
static const cluster_attr_tab_t k_cluster_attr_tabs[] = {
    ATTRTAB(0x0003, k_attr_identify),      ATTRTAB(0x0006, k_attr_onoff),
    ATTRTAB(0x001D, k_attr_descriptor),    ATTRTAB(0x001F, k_attr_accesscontrol),
    ATTRTAB(0x0028, k_attr_basicinfo),     ATTRTAB(0x002A, k_attr_ota),
    ATTRTAB(0x0030, k_attr_gencomm),       ATTRTAB(0x0031, k_attr_netcomm),
    ATTRTAB(0x0033, k_attr_gendiag),       ATTRTAB(0x0035, k_attr_threaddiag),
    ATTRTAB(0x0038, k_attr_timesync),      ATTRTAB(0x003C, k_attr_admincomm),
    ATTRTAB(0x003E, k_attr_opcreds),       ATTRTAB(0x003F, k_attr_groupkey),
    ATTRTAB(0x005B, k_attr_airquality),    ATTRTAB(0x003B, k_attr_switch),
    ATTRTAB(0x002F, k_attr_powersource),   ATTRTAB(0x0046, k_attr_icd),
    ATTRTAB(0x0402, k_attr_measurement),   ATTRTAB(0x0403, k_attr_measurement),
    ATTRTAB(0x0404, k_attr_measurement),   ATTRTAB(0x0405, k_attr_measurement),
    /* every concentration-measurement cluster shares one table */
    ATTRTAB(0x040C, k_attr_concentration), ATTRTAB(0x040D, k_attr_concentration),
    ATTRTAB(0x042A, k_attr_concentration), ATTRTAB(0x042B, k_attr_concentration),
    ATTRTAB(0x042C, k_attr_concentration), ATTRTAB(0x042D, k_attr_concentration),
    ATTRTAB(0x042E, k_attr_concentration),
};
#undef ATTRTAB

static inline const char *global_attr_name(uint32_t attr)
{
    switch (attr) {
    case 0xFFF8: return "GeneratedCommandList";
    case 0xFFF9: return "AcceptedCommandList";
    case 0xFFFA: return "EventList";
    case 0xFFFB: return "AttributeList";
    case 0xFFFC: return "FeatureMap";
    case 0xFFFD: return "ClusterRevision";
    default:     return nullptr;
    }
}

static inline const char *attr_name(uint32_t cluster, uint32_t attr)
{
    const char *g = global_attr_name(attr);
    if (g) return g;
    for (size_t i = 0; i < sizeof(k_cluster_attr_tabs)/sizeof(k_cluster_attr_tabs[0]); ++i)
        if (k_cluster_attr_tabs[i].cluster == cluster)
            return lookup_name(k_cluster_attr_tabs[i].tbl, k_cluster_attr_tabs[i].count, attr);
    return nullptr;
}

/* Format "<name> (0x0000)" or just "0x0000" when unknown, into buf. */
static inline const char *attr_label(uint32_t cluster, uint32_t attr, char *buf, size_t n)
{
    const char *nm = attr_name(cluster, attr);
    if (nm) snprintf(buf, n, "%s (0x%04lx)", nm, (unsigned long)attr);
    else    snprintf(buf, n, "0x%04lx", (unsigned long)attr);
    return buf;
}

/* ===================================================================== *
 *  Per-cluster ACCEPTED-command names (the ids you 'invoke').
 *  Keyed by (cluster, command); command ids repeat across clusters. No global
 *  commands exist. Covers this device's clusters plus common controllable ones.
 * ===================================================================== */
static const id_name_t k_cmd_identify[] = {
    {0x00, "Identify"}, {0x01, "TriggerEffect"},
};
static const id_name_t k_cmd_groups[] = {
    {0x00, "AddGroup"}, {0x01, "ViewGroup"}, {0x02, "GetGroupMembership"},
    {0x03, "RemoveGroup"}, {0x04, "RemoveAllGroups"}, {0x05, "AddGroupIfIdentifying"},
};
static const id_name_t k_cmd_onoff[] = {
    {0x00, "Off"}, {0x01, "On"}, {0x02, "Toggle"}, {0x40, "OffWithEffect"},
    {0x41, "OnWithRecallGlobalScene"}, {0x42, "OnWithTimedOff"},
};
static const id_name_t k_cmd_levelcontrol[] = {
    {0x00, "MoveToLevel"}, {0x01, "Move"}, {0x02, "Step"}, {0x03, "Stop"},
    {0x04, "MoveToLevelWithOnOff"}, {0x05, "MoveWithOnOff"}, {0x06, "StepWithOnOff"},
    {0x07, "StopWithOnOff"}, {0x08, "MoveToClosestFrequency"},
};
static const id_name_t k_cmd_colorcontrol[] = {
    {0x00, "MoveToHue"}, {0x01, "MoveHue"}, {0x02, "StepHue"}, {0x03, "MoveToSaturation"},
    {0x04, "MoveSaturation"}, {0x05, "StepSaturation"}, {0x06, "MoveToHueAndSaturation"},
    {0x07, "MoveToColor"}, {0x08, "MoveColor"}, {0x09, "StepColor"},
    {0x0A, "MoveToColorTemperature"}, {0x47, "StopMoveStep"},
    {0x4B, "MoveColorTemperature"}, {0x4C, "StepColorTemperature"},
};
static const id_name_t k_cmd_windowcovering[] = {
    {0x00, "UpOrOpen"}, {0x01, "DownOrClose"}, {0x02, "StopMotion"},
    {0x04, "GoToLiftValue"}, {0x05, "GoToLiftPercentage"},
    {0x07, "GoToTiltValue"}, {0x08, "GoToTiltPercentage"},
};
static const id_name_t k_cmd_doorlock[] = {
    {0x00, "LockDoor"}, {0x01, "UnlockDoor"}, {0x03, "UnlockWithTimeout"},
};
static const id_name_t k_cmd_ota[] = {
    {0x00, "AnnounceOTAProvider"},
};
static const id_name_t k_cmd_gencomm[] = {
    {0x00, "ArmFailSafe"}, {0x02, "SetRegulatoryConfig"}, {0x04, "CommissioningComplete"},
};
static const id_name_t k_cmd_netcomm[] = {
    {0x00, "ScanNetworks"}, {0x02, "AddOrUpdateWiFiNetwork"}, {0x03, "AddOrUpdateThreadNetwork"},
    {0x04, "RemoveNetwork"}, {0x06, "ConnectNetwork"}, {0x08, "ReorderNetwork"},
};
static const id_name_t k_cmd_gendiag[] = {
    {0x00, "TestEventTrigger"}, {0x01, "TimeSnapshot"}, {0x03, "PayloadTestRequest"},
};
static const id_name_t k_cmd_timesync[] = {
    {0x00, "SetUTCTime"}, {0x01, "SetTrustedTimeSource"}, {0x02, "SetTimeZone"},
    {0x03, "SetDSTOffset"}, {0x04, "SetDefaultNTP"},
};
static const id_name_t k_cmd_admincomm[] = {
    {0x00, "OpenCommissioningWindow"}, {0x01, "OpenBasicCommissioningWindow"},
    {0x02, "RevokeCommissioning"},
};
static const id_name_t k_cmd_opcreds[] = {
    {0x00, "AttestationRequest"}, {0x02, "CertificateChainRequest"}, {0x04, "CSRRequest"},
    {0x06, "AddNOC"}, {0x07, "UpdateNOC"}, {0x09, "UpdateFabricLabel"},
    {0x0A, "RemoveFabric"}, {0x0B, "AddTrustedRootCertificate"},
};
static const id_name_t k_cmd_groupkey[] = {
    {0x00, "KeySetWrite"}, {0x01, "KeySetRead"}, {0x03, "KeySetRemove"},
    {0x04, "KeySetReadAllIndices"},
};
static const id_name_t k_cmd_icd[] = {
    {0x00, "RegisterClient"}, {0x02, "UnregisterClient"}, {0x03, "StayActiveRequest"},
};

struct cluster_cmd_tab_t { uint32_t cluster; const id_name_t *tbl; size_t count; };
#define CMDTAB(cl, arr) { (cl), (arr), sizeof(arr)/sizeof((arr)[0]) }
static const cluster_cmd_tab_t k_cluster_cmd_tabs[] = {
    CMDTAB(0x0003, k_cmd_identify),   CMDTAB(0x0004, k_cmd_groups),
    CMDTAB(0x0006, k_cmd_onoff),      CMDTAB(0x0008, k_cmd_levelcontrol),
    CMDTAB(0x0300, k_cmd_colorcontrol), CMDTAB(0x0102, k_cmd_windowcovering),
    CMDTAB(0x0101, k_cmd_doorlock),   CMDTAB(0x002A, k_cmd_ota),
    CMDTAB(0x0030, k_cmd_gencomm),    CMDTAB(0x0031, k_cmd_netcomm),
    CMDTAB(0x0033, k_cmd_gendiag),    CMDTAB(0x0038, k_cmd_timesync),
    CMDTAB(0x003C, k_cmd_admincomm),  CMDTAB(0x003E, k_cmd_opcreds),
    CMDTAB(0x003F, k_cmd_groupkey),   CMDTAB(0x0046, k_cmd_icd),
};
#undef CMDTAB

static inline const char *cmd_name(uint32_t cluster, uint32_t cmd)
{
    for (size_t i = 0; i < sizeof(k_cluster_cmd_tabs)/sizeof(k_cluster_cmd_tabs[0]); ++i)
        if (k_cluster_cmd_tabs[i].cluster == cluster)
            return lookup_name(k_cluster_cmd_tabs[i].tbl, k_cluster_cmd_tabs[i].count, cmd);
    return nullptr;
}

/* Format "<name> (0x01)" or just "0x01" when unknown, into buf. */
static inline const char *cmd_label(uint32_t cluster, uint32_t cmd, char *buf, size_t n)
{
    const char *nm = cmd_name(cluster, cmd);
    if (nm) snprintf(buf, n, "%s (0x%02lx)", nm, (unsigned long)cmd);
    else    snprintf(buf, n, "0x%02lx", (unsigned long)cmd);
    return buf;
}

/* ===================================================================== *
 *  Per-cluster EVENT names. Events are how a Generic Switch reports button
 *  presses (there are no commands and no "was pressed" attribute), so these
 *  are what make the event subscription readable.
 * ===================================================================== */
static const id_name_t k_evt_switch[] = {
    {0x00, "SwitchLatched"}, {0x01, "InitialPress"}, {0x02, "LongPress"},
    {0x03, "ShortRelease"}, {0x04, "LongRelease"},
    {0x05, "MultiPressOngoing"}, {0x06, "MultiPressComplete"},
};
static const id_name_t k_evt_basicinfo[] = {
    {0x00, "StartUp"}, {0x01, "ShutDown"}, {0x02, "Leave"}, {0x03, "ReachableChanged"},
};
static const id_name_t k_evt_powersource[] = {
    {0x00, "WiredFaultChange"}, {0x01, "BatFaultChange"}, {0x02, "BatChargeFaultChange"},
};
static const id_name_t k_evt_gendiag[] = {
    {0x00, "HardwareFaultChange"}, {0x01, "RadioFaultChange"},
    {0x02, "NetworkFaultChange"}, {0x03, "BootReason"},
};
static const id_name_t k_evt_threaddiag[] = {
    {0x00, "ConnectionStatus"}, {0x01, "NetworkFaultChange"},
};
static const id_name_t k_evt_timesync[] = {
    {0x00, "DSTTableEmpty"}, {0x01, "DSTStatus"}, {0x02, "TimeZoneStatus"},
    {0x03, "TimeFailure"}, {0x04, "MissingTrustedTimeSource"},
};

struct cluster_evt_tab_t { uint32_t cluster; const id_name_t *tbl; size_t count; };
#define EVTTAB(cl, arr) { (cl), (arr), sizeof(arr)/sizeof((arr)[0]) }
static const cluster_evt_tab_t k_cluster_evt_tabs[] = {
    EVTTAB(0x003B, k_evt_switch),     EVTTAB(0x0028, k_evt_basicinfo),
    EVTTAB(0x002F, k_evt_powersource), EVTTAB(0x0033, k_evt_gendiag),
    EVTTAB(0x0035, k_evt_threaddiag),  EVTTAB(0x0038, k_evt_timesync),
};
#undef EVTTAB

static inline const char *event_name(uint32_t cluster, uint32_t evt)
{
    for (size_t i = 0; i < sizeof(k_cluster_evt_tabs)/sizeof(k_cluster_evt_tabs[0]); ++i)
        if (k_cluster_evt_tabs[i].cluster == cluster)
            return lookup_name(k_cluster_evt_tabs[i].tbl, k_cluster_evt_tabs[i].count, evt);
    return nullptr;
}

/* Format "<name> (0x03)" or just "0x03" when unknown, into buf. */
static inline const char *event_label(uint32_t cluster, uint32_t evt, char *buf, size_t n)
{
    const char *nm = event_name(cluster, evt);
    if (nm) snprintf(buf, n, "%s (0x%02lx)", nm, (unsigned long)evt);
    else    snprintf(buf, n, "0x%02lx", (unsigned long)evt);
    return buf;
}
