/*
 * Derive Matter pairing credentials from the nRF52840 FICR hardware device ID.
 *
 * The FICR DEVICEID registers hold a 64-bit value that is unique per die and
 * programmed at the factory.  We domain-separate FNV-1a hashes of those bytes
 * to produce a 12-bit discriminator, a valid passcode, a 16-byte PBKDF2 salt,
 * and a SPAKE2+ verifier — all without any runtime storage or factory data.
 */

#include "hw_pairing.h"

#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CHIPError.h>
#include <lib/support/Span.h>
#include <platform/CommissionableDataProvider.h>

#include <nrfx.h>
#include <zephyr/logging/log.h>

#include <string.h>

LOG_MODULE_REGISTER(hw_pairing, CONFIG_LOG_DEFAULT_LEVEL);

using namespace chip;
using namespace chip::Crypto;
using namespace chip::DeviceLayer;

/* ---- FNV-1a 32-bit ---------------------------------------------------- */

static constexpr uint32_t kFnvBasis = 2166136261u;
static constexpr uint32_t kFnvPrime = 16777619u;

static uint32_t fnv1a32(const uint8_t *data, size_t len, uint32_t seed)
{
    uint32_t h = seed;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= kFnvPrime;
    }
    return h;
}

/* ---- Passcode sanitisation -------------------------------------------- */

/* Matter spec Table 39 — forbidden passcodes. */
static constexpr uint32_t kForbidden[] = {
    00000000u, 11111111u, 22222222u, 33333333u, 44444444u,
    55555555u, 66666666u, 77777777u, 88888888u, 99999999u,
    12345678u, 87654321u,
};

static uint32_t make_passcode(uint32_t h)
{
    /* Map to [1, 99999998]. */
    uint32_t code = (h % 99999998u) + 1u;

    for (uint32_t f : kForbidden) {
        if (code == f) {
            /* Bump by one; no two forbidden values are adjacent. */
            code = (code % 99999998u) + 1u;
            break;
        }
    }
    return code;
}

/* ---- CommissionableDataProvider --------------------------------------- */

namespace {

class HwCommissionableDataProvider : public CommissionableDataProvider
{
public:
    CHIP_ERROR Init();

    CHIP_ERROR GetSetupDiscriminator(uint16_t & discriminator) override;
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t & iterationCount) override;
    CHIP_ERROR GetSpake2pSalt(MutableByteSpan & saltBuf) override;
    CHIP_ERROR GetSpake2pVerifier(MutableByteSpan & verifierBuf,
                                  size_t & verifierLen) override;
    CHIP_ERROR GetSetupPasscode(uint32_t & passcode) override;
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

private:
    static constexpr uint32_t kIterations   = 1000;
    static constexpr size_t   kSaltLen      = 16;
    static constexpr size_t   kVerifierLen  = kSpake2p_VerifierSerialized_Length;

    uint16_t mDiscriminator = 0;
    uint32_t mPasscode      = 0;
    uint8_t  mSalt[kSaltLen];
    uint8_t  mVerifier[kVerifierLen];
    bool     mReady = false;
};

HwCommissionableDataProvider gProvider;

} // namespace

CHIP_ERROR HwCommissionableDataProvider::Init()
{
    /* Read the 64-bit FICR unique device ID. */
    uint32_t id0 = NRF_FICR->DEVICEID[0];
    uint32_t id1 = NRF_FICR->DEVICEID[1];

    uint8_t raw[8];
    memcpy(raw,     &id0, 4);
    memcpy(raw + 4, &id1, 4);

    /* Discriminator — 12 bits. */
    uint32_t hDisc  = fnv1a32(raw, sizeof(raw), 0xD15C0001u);
    mDiscriminator  = static_cast<uint16_t>(hDisc & 0x0FFFu);

    /* Passcode — [1, 99999998] minus forbidden values. */
    uint32_t hPass = fnv1a32(raw, sizeof(raw), 0xD15C0002u);
    mPasscode      = make_passcode(hPass);

    /* Salt — 16 bytes from four domain-separated hashes. */
    for (int i = 0; i < 4; i++) {
        uint32_t hs = fnv1a32(raw, sizeof(raw),
                               static_cast<uint32_t>(0xD15C0003u + i));
        memcpy(mSalt + (i * 4), &hs, 4);
    }

    /* Compute the SPAKE2+ verifier (involves PBKDF2-SHA256 — ~100–500 ms). */
    Spake2pVerifier verifier;
    ReturnErrorOnFailure(verifier.Generate(kIterations,
                                           ByteSpan(mSalt, kSaltLen),
                                           mPasscode));

    MutableByteSpan verifierSpan(mVerifier, kVerifierLen);
    ReturnErrorOnFailure(verifier.Serialize(verifierSpan));

    mReady = true;

    LOG_INF("HW pairing: discriminator=%04u  passcode=%08u",
            mDiscriminator, mPasscode);
    return CHIP_NO_ERROR;
}

CHIP_ERROR HwCommissionableDataProvider::GetSetupDiscriminator(uint16_t & discriminator)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    discriminator = mDiscriminator;
    return CHIP_NO_ERROR;
}

CHIP_ERROR HwCommissionableDataProvider::GetSpake2pIterationCount(uint32_t & iterationCount)
{
    iterationCount = kIterations;
    return CHIP_NO_ERROR;
}

CHIP_ERROR HwCommissionableDataProvider::GetSpake2pSalt(MutableByteSpan & saltBuf)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(saltBuf.size() >= kSaltLen, CHIP_ERROR_BUFFER_TOO_SMALL);
    memcpy(saltBuf.data(), mSalt, kSaltLen);
    saltBuf.reduce_size(kSaltLen);
    return CHIP_NO_ERROR;
}

CHIP_ERROR HwCommissionableDataProvider::GetSpake2pVerifier(MutableByteSpan & verifierBuf,
                                                             size_t & verifierLen)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(verifierBuf.size() >= kVerifierLen, CHIP_ERROR_BUFFER_TOO_SMALL);
    memcpy(verifierBuf.data(), mVerifier, kVerifierLen);
    verifierBuf.reduce_size(kVerifierLen);
    verifierLen = kVerifierLen;
    return CHIP_NO_ERROR;
}

CHIP_ERROR HwCommissionableDataProvider::GetSetupPasscode(uint32_t & passcode)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    passcode = mPasscode;
    return CHIP_NO_ERROR;
}

/* ---- Public API ------------------------------------------------------- */

CHIP_ERROR HwPairing::Init()
{
    ReturnErrorOnFailure(gProvider.Init());
    SetCommissionableDataProvider(&gProvider);
    return CHIP_NO_ERROR;
}
