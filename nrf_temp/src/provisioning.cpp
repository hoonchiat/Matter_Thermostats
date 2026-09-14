/*
 * Per-device provisioning (see provisioning.h). Reads the fixed 64-byte blob the
 * browser provisioning tool writes at offset 0 of the `factory` partition and
 * exposes {discriminator, passcode, verifier} to Matter. Identical blob layout,
 * salt and iteration count to the ESP32 light/button, so one tool serves all.
 */
#include "provisioning.h"

#include <cstring>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>

#include <platform/CommissionableDataProvider.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Span.h>
#include <lib/support/CodeUtils.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;

#define PROV_MAGIC 0x50524F56u /* 'PROV' */

/* The `factory` partition, read by absolute flash offset via the nRF flash
 * controller. We go directly to the flash driver (not the flash_area / PM API)
 * because NCS's Partition Manager only exposes FIXED_PARTITION_ID for
 * PM-managed partitions, and this UF2 build partitions purely in devicetree. */
#define FACTORY_NODE DT_NODELABEL(factory_partition)

/* Fixed layout the HTML provisioning tool writes at offset 0 of `factory`
 * (little-endian; see buildFctry() in prov-template.html). */
typedef struct __attribute__((packed)) {
	uint32_t magic;         /* PROV_MAGIC                     */
	uint16_t version;       /* 1                              */
	uint16_t discriminator; /* 0..4095                        */
	uint32_t passcode;      /* 1..99999998 (valid Matter PIN) */
	char serial[33];        /* NUL-terminated                 */
	uint8_t reserved[17];   /* pad                            */
} prov_blob_t;

/* Shared salt + iteration count. The verifier still differs per device because
 * the passcode differs; the commissioner receives this salt/iterations during
 * PASE, so only the passcode has to be on the label/QR. Byte-identical to the
 * ESP32 firmware ("PJoshua-Matter-Prov-Salt-v1.0!!!"). */
static const uint8_t kSalt[32] = {
	0x50, 0x4a, 0x6f, 0x73, 0x68, 0x75, 0x61, 0x2d, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x2d, 0x50,
	0x72, 0x6f, 0x76, 0x2d, 0x53, 0x61, 0x6c, 0x74, 0x2d, 0x76, 0x31, 0x2e, 0x30, 0x21, 0x21, 0x21
};
static const uint32_t kIter = 1000;

static uint16_t s_disc = 3840; /* test defaults if unprovisioned */
static uint32_t s_pass = 20202021;
static char s_serial[33] = { 0 };
static bool s_provisioned = false;

class ProvCommissionableDataProvider : public DeviceLayer::CommissionableDataProvider {
public:
	CHIP_ERROR GetSetupDiscriminator(uint16_t &v) override
	{
		v = s_disc;
		return CHIP_NO_ERROR;
	}
	CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
	CHIP_ERROR GetSpake2pIterationCount(uint32_t &c) override
	{
		c = kIter;
		return CHIP_NO_ERROR;
	}
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
	CHIP_ERROR GetSetupPasscode(uint32_t &v) override
	{
		v = s_pass;
		return CHIP_NO_ERROR;
	}
	CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
};

static ProvCommissionableDataProvider s_provider;

CHIP_ERROR ProvisioningPreServerInit()
{
	const struct device *flash = DEVICE_DT_GET(DT_NODELABEL(flash_controller));
	if (device_is_ready(flash)) {
		prov_blob_t blob;
		int rc = flash_read(flash, DT_REG_ADDR(FACTORY_NODE), &blob, sizeof(blob));
		if (rc == 0 && blob.magic == PROV_MAGIC && blob.discriminator <= 0x0FFF &&
		    blob.passcode >= 1 && blob.passcode <= 99999998) {
			s_disc = blob.discriminator;
			s_pass = blob.passcode;
			blob.serial[sizeof(blob.serial) - 1] = '\0';
			if (blob.serial[0]) {
				strncpy(s_serial, blob.serial, sizeof(s_serial) - 1);
			}
			s_provisioned = true;
			LOG_INF("Provisioned from factory: discriminator=%u passcode=%08u serial=%s", s_disc,
				(unsigned) s_pass, s_serial);
		}
	}
	if (!s_provisioned) {
		LOG_WRN("factory partition blank - using test creds (discriminator=%u)", s_disc);
	}

	DeviceLayer::SetCommissionableDataProvider(&s_provider);
	return CHIP_NO_ERROR;
}

uint16_t ProvisioningDiscriminator()
{
	return s_disc;
}
const char *ProvisioningSerial()
{
	return s_serial;
}
bool ProvisioningIsProvisioned()
{
	return s_provisioned;
}
