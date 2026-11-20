/* CTR_DRBG seeded from the PA3 entropy pool (#14, docs/design.md "Entropy"). */
/* The reseed counter is read to keep a reseed out of the radio task
 * (rng_try_bytes); mbedTLS has no accessor for it. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "rng.h"

#include "entropy.h"
#include "log.h"
#include "timebase.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/platform_util.h"

#include <string.h>

/* 96-bit unique device ID (docs/datasheets/rm0383.txt:46698-46711). Mixed
 * into the seed as the personalization string, uncredited: it makes two
 * bridges differ even with equal noise, but it is not secret. */
#define UID_BASE 0x1FFF7A10u

static mbedtls_ctr_drbg_context drbg;
/* Set last in rng_init(), after the lock exists; read by other tasks. */
static volatile bool ready;
/* The DRBG has one state: every request holds this (#17, the radio task
 * draws from it too). A request can include a reseed, about 45 ms of ADC
 * readings in the caller's task (docs/design.md "Entropy: implementation"),
 * so the lock can be held that long. */
static SemaphoreHandle_t lock;

bool rng_init(void) {
    if (!lock) { /* once, also when the start-up test fails and is retried */
        lock = xSemaphoreCreateMutex();
        configASSERT(lock);
    }
    struct entropy_report r;
    bool                  ok = entropy_startup(&r);
    LOG("rng: PA3 start-up test %s: %lu readings, range %u-%u, longest run %u, APT max %u of 512, "
        "%lu read timeouts",
        ok ? "passed" : r.fail,
        (unsigned long)r.samples,
        (unsigned)r.min,
        (unsigned)r.max,
        (unsigned)r.rct_max,
        (unsigned)r.apt_max,
        (unsigned long)r.timeouts);
    if (!ok) {
        return false;
    }

    static const char tag[] = "tado-bridge ctr_drbg";
    unsigned char     pers[sizeof tag - 1 + 12];
    memcpy(pers, tag, sizeof tag - 1);
    memcpy(pers + sizeof tag - 1, (const void *)UID_BASE, 12);

    uint32_t t0 = timebase_us();
    mbedtls_ctr_drbg_init(&drbg);
    /* Seeds with MBEDTLS_CTR_DRBG_ENTROPY_LEN (48) bytes from entropy_get()
     * and reseeds from it every MBEDTLS_CTR_DRBG_RESEED_INTERVAL requests. */
    int ret = mbedtls_ctr_drbg_seed(&drbg, entropy_get, NULL, pers, sizeof pers);
    if (ret != 0) {
        LOG("rng: CTR_DRBG seed failed (%d): health test failed during collection", ret);
        mbedtls_ctr_drbg_free(&drbg);
        return false;
    }
    ready = true;
    LOG("rng: CTR_DRBG (AES-256) seeded with %u bytes of conditioned PA3 entropy in %lu ms",
        (unsigned)MBEDTLS_CTR_DRBG_ENTROPY_LEN,
        (unsigned long)((timebase_us() - t0) / 1000u));
    return true;
}

int rng_random(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    if (!ready) {
        return -1;
    }
    int ret = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    while (len > 0u && ret == 0) {
        size_t n = len < MBEDTLS_CTR_DRBG_MAX_REQUEST ? len : MBEDTLS_CTR_DRBG_MAX_REQUEST;
        ret      = mbedtls_ctr_drbg_random(&drbg, out, n); /* non-zero: a reseed failed, latched */
        out += n;
        len -= n;
    }
    xSemaphoreGive(lock);
    return ret;
}

bool rng_try_bytes(void *out, size_t len) {
    if (!ready || len > MBEDTLS_CTR_DRBG_MAX_REQUEST || xSemaphoreTake(lock, 0) != pdTRUE) {
        return false;
    }
    /* mbedTLS reseeds inside a request once the counter has passed the
     * interval (vendor/mbedtls/library/ctr_drbg.c, mbedtls_ctr_drbg_random_
     * with_add). Leave that request to a caller that may wait. */
    bool ok =
        drbg.reseed_counter < drbg.reseed_interval && mbedtls_ctr_drbg_random(&drbg, out, len) == 0;
    xSemaphoreGive(lock);
    return ok;
}

void rng_bytes(void *out, size_t len) {
    if (rng_random(NULL, out, len) != 0) {
        mbedtls_platform_zeroize(out, len);
        fw_panic("rng: no random numbers (not seeded, or reseed failed its health test)",
                 __FILE__,
                 __LINE__);
    }
}

uint32_t rng_u32(void) {
    uint32_t v;
    rng_bytes(&v, sizeof v);
    return v;
}
