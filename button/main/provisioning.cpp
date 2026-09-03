/*
 * Per-device provisioning: read {discriminator, passcode, serial} from the `fctry`
 * partition and feed them to Matter via a custom CommissionableDataProvider. The
 * SPAKE2+ verifier is computed on-device from the stored passcode + a fixed salt,
 * so the HTML provisioning tool only has to write the passcode (no crypto in the
 * browser). Falls back to the compile-time test defaults if the partition is blank.
 */
#include "provisioning.h"

#include <cstring>
#include <esp_log.h>
#include <esp_partition.h>

#include <esp_matter.h>
#include <esp_matter_providers.h>

#include <platform/CommissionableDataProvider.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Span.h>
#include <lib/support/CodeUtils.h>

using namespace chip;

static const char *TAG = "provisioning";

#define PROV_MAGIC      0x50524F56u    /* 'PROV' */
#define PROV_PART_LABEL "fctry"

/* Fixed 64-byte layout the HTML provisioning tool writes at offset 0 of `fctry`. */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* PROV_MAGIC                     */
    uint16_t version;       /* 1                              */
    uint16_t discriminator; /* 0..4095                        */
    uint32_t passcode;      /* 1..99999998 (valid Matter PIN) */
    char     serial[33];    /* NUL-terminated                 */
    uint8_t  reserved[17];  /* pad to 64                      */
} prov_blob_t;

/* Shared salt + iteration count. The verifier still differs per device because the
 * passcode differs; the commissioner receives this salt/iterations during PASE, so
 * only the passcode needs to be on the label/QR. */
static const uint8_t  kSalt[32] = {
    0x50, 0x4a, 0x6f, 0x73, 0x68, 0x75, 0x61, 0x2d, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x2d, 0x50,
    0x72, 0x6f, 0x76, 0x2d, 0x53, 0x61, 0x6c, 0x74, 0x2d, 0x76, 0x31, 0x2e, 0x30, 0x21, 0x21, 0x21
};
static const uint32_t kIter = 1000;

static uint16_t s_disc         = 3840;       /* test defaults if unprovisioned */
static uint32_t s_pass         = 20202021;
static char     s_serial[33]   = { 0 };
static bool     s_provisioned  = false;

class ProvCommissionableDataProvider : public DeviceLayer::CommissionableDataProvider {
public:
    CHIP_ERROR GetSetupDiscriminator(uint16_t &v) override { v = s_disc; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t &c) override { c = kIter; return CHIP_NO_ERROR; }
    CHIP_ERROR GetSpake2pSalt(MutableByteSpan &buf) override
    {
        VerifyOrReturnError(buf.size() >= sizeof(kSalt), CHIP_ERROR_BUFFER_TOO_SMALL);
        memcpy(buf.data(), kSalt, sizeof(kSalt));
        buf.reduce_size(sizeof(kSalt));
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetSpake2pVerifier(MutableByteSpan &buf, size_t &outLen) override
    {
        Crypto::Spake2pVerifier verifier;
        ReturnErrorOnFailure(verifier.Generate(kIter, ByteSpan(kSalt, sizeof(kSalt)), s_pass));
        ReturnErrorOnFailure(verifier.Serialize(buf));
        outLen = buf.size();
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetSetupPasscode(uint32_t &v) override { v = s_pass; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
};

static ProvCommissionableDataProvider s_provider;

void provisioning_init(const char *default_serial)
{
    if (default_serial && default_serial[0]) {
        strncpy(s_serial, default_serial, sizeof(s_serial) - 1);
    }

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PROV_PART_LABEL);
    if (part) {
        prov_blob_t blob;
        if (esp_partition_read(part, 0, &blob, sizeof(blob)) == ESP_OK && blob.magic == PROV_MAGIC &&
            blob.discriminator <= 0x0FFF && blob.passcode >= 1 && blob.passcode <= 99999998) {
            s_disc = blob.discriminator;
            s_pass = blob.passcode;
            blob.serial[sizeof(blob.serial) - 1] = '\0';
            if (blob.serial[0]) {
                strncpy(s_serial, blob.serial, sizeof(s_serial) - 1);
            }
            s_provisioned = true;
            ESP_LOGI(TAG, "Provisioned from fctry: discriminator=%u passcode=%08u serial=%s",
                     s_disc, (unsigned) s_pass, s_serial);
        }
    }
    if (!s_provisioned) {
        ESP_LOGW(TAG, "fctry not provisioned - using test defaults (discriminator=%u)", s_disc);
    }

    esp_matter::set_custom_commissionable_data_provider(&s_provider);
}

const char *provisioning_serial(void)         { return s_serial; }
uint16_t    provisioning_discriminator(void)  { return s_disc; }
bool        provisioning_is_provisioned(void) { return s_provisioned; }
