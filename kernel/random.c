#include "kernel.h"
#include "random.h"
#include "random_core.h"
#include "proc.h"
#include "timer.h"
#include "string.h"
#include "spinlock.h"

#define ENTROPY_READY_BITS 128u
#define ENTROPY_MAX_BITS   256u

static uint8_t pool[32];
static uint64_t pool_events;
static uint64_t pool_generation;
static unsigned credited_bits;
static bool pool_initialized;
static bool hardware_source;

static uint8_t crng_key[32];
static uint8_t crng_nonce[12];
static uint32_t crng_counter;
static uint64_t crng_generation;
static bool crng_seeded;

static uint64_t timing_last[ENTROPY_SOURCE_MAX];
static uint64_t timing_delta[ENTROPY_SOURCE_MAX];
static spinlock_t random_lock = SPINLOCK_INIT;

static uint64_t read_tsc(void)
{
    uint32_t lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void sha_piece(struct random_sha256 *s, const void *p, size_t n)
{
    random_sha256_update(s, p, n);
}

static void pool_bootstrap_locked(void)
{
    static const char domain[] = "KestrelOS entropy pool v1";
    struct random_sha256 s;

    if (pool_initialized)
        return;
    random_sha256_init(&s);
    sha_piece(&s, domain, sizeof(domain));
    random_sha256_final(&s, pool);
    pool_events = 0;
    pool_generation = 1;
    credited_bits = 0;
    pool_initialized = true;
}

static void pool_mix_locked(enum entropy_source source, const void *data,
                            size_t len, unsigned credit)
{
    static const char domain[] = "KestrelOS entropy event v1";
    struct random_sha256 s;
    uint64_t event = ++pool_events;
    uint32_t src = (uint32_t)source;
    uint64_t length = len;

    pool_bootstrap_locked();
    random_sha256_init(&s);
    sha_piece(&s, domain, sizeof(domain));
    sha_piece(&s, pool, sizeof(pool));
    sha_piece(&s, &event, sizeof(event));
    sha_piece(&s, &src, sizeof(src));
    sha_piece(&s, &length, sizeof(length));
    if (data && len)
        sha_piece(&s, data, len);
    random_sha256_final(&s, pool);
    pool_generation++;

    if (credit > ENTROPY_MAX_BITS - credited_bits)
        credit = ENTROPY_MAX_BITS - credited_bits;
    credited_bits += credit;
}

void entropy_pool_add(enum entropy_source source, const void *data, size_t len,
                      unsigned credit_bits)
{
    uint64_t flags;

    if (source <= 0 || source >= ENTROPY_SOURCE_MAX || (!data && len))
        return;
    if (credit_bits > len * 8)
        credit_bits = (unsigned)(len * 8);
    flags = spin_lock_irqsave(&random_lock);
    pool_mix_locked(source, data, len, credit_bits);
    spin_unlock_irqrestore(&random_lock, flags);
}

void entropy_pool_add_timing(enum entropy_source source, uint64_t sample)
{
    struct {
        uint64_t sample;
        uint64_t delta;
        uint64_t delta2;
        uint64_t ticks;
    } event;
    unsigned credit = 0;

    if (source <= 0 || source >= ENTROPY_SOURCE_MAX)
        return;
    uint64_t flags = spin_lock_irqsave(&random_lock);
    event.sample = sample;
    event.delta = sample - timing_last[source];
    event.delta2 = event.delta - timing_delta[source];
    event.ticks = timer_ticks();
    if (timing_last[source] != 0 && event.delta2 != 0)
        credit = 1;
    timing_last[source] = sample;
    timing_delta[source] = event.delta;
    pool_mix_locked(source, &event, sizeof(event), credit);
    spin_unlock_irqrestore(&random_lock, flags);
}

void entropy_pool_add_interrupt(enum entropy_source source, uint32_t detail)
{
    struct {
        uint64_t tsc;
        uint64_t ticks;
        uint32_t detail;
        uint32_t source;
    } event;

    event.tsc = read_tsc();
    event.ticks = timer_ticks();
    event.detail = detail;
    event.source = (uint32_t)source;
    entropy_pool_add_timing(source, event.tsc);
    entropy_pool_add(source, &event, sizeof(event), 0);
}

static void crng_reseed_locked(void)
{
    static const char key_domain[] = "KestrelOS ChaCha20 key v1";
    static const char nonce_domain[] = "KestrelOS ChaCha20 nonce v1";
    struct random_sha256 s;
    uint8_t digest[32];
    uint64_t generation = pool_generation;

    pool_bootstrap_locked();
    random_sha256_init(&s);
    sha_piece(&s, key_domain, sizeof(key_domain));
    sha_piece(&s, pool, sizeof(pool));
    sha_piece(&s, crng_key, sizeof(crng_key));
    sha_piece(&s, &generation, sizeof(generation));
    random_sha256_final(&s, crng_key);

    random_sha256_init(&s);
    sha_piece(&s, nonce_domain, sizeof(nonce_domain));
    sha_piece(&s, pool, sizeof(pool));
    sha_piece(&s, crng_key, sizeof(crng_key));
    sha_piece(&s, &generation, sizeof(generation));
    random_sha256_final(&s, digest);
    memcpy(crng_nonce, digest, sizeof(crng_nonce));
    memset(digest, 0, sizeof(digest));
    crng_counter = 1;
    crng_generation = pool_generation;
    crng_seeded = true;
}

static void crng_generate_locked(uint8_t *out, size_t len)
{
    uint8_t block[64];

    if (!crng_seeded || crng_generation != pool_generation)
        crng_reseed_locked();
    while (len) {
        size_t take = len < sizeof(block) ? len : sizeof(block);

        random_chacha20_block(block, crng_key, crng_counter++, crng_nonce);
        memcpy(out, block, take);
        out += take;
        len -= take;
        if (crng_counter == 0)
            crng_reseed_locked();
    }

    /* Fast key erasure: output one unseen block and replace the live key
     * and nonce, so recovering current state does not reveal old output. */
    random_chacha20_block(block, crng_key, crng_counter++, crng_nonce);
    memcpy(crng_key, block, sizeof(crng_key));
    memcpy(crng_nonce, block + sizeof(crng_key), sizeof(crng_nonce));
    memset(block, 0, sizeof(block));
}

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b,
                  uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

static bool cpu_rdseed64(uint64_t *value)
{
    unsigned char ok;

    for (int attempt = 0; attempt < 64; attempt++) {
        __asm__ volatile("rdseed %0; setc %1"
                         : "=r"(*value), "=qm"(ok) : : "cc");
        if (ok)
            return true;
        __asm__ volatile("pause");
    }
    return false;
}

static bool cpu_rdrand64(uint64_t *value)
{
    unsigned char ok;

    for (int attempt = 0; attempt < 16; attempt++) {
        __asm__ volatile("rdrand %0; setc %1"
                         : "=r"(*value), "=qm"(ok) : : "cc");
        if (ok)
            return true;
        __asm__ volatile("pause");
    }
    return false;
}

void random_init(uint32_t boot_seed)
{
    uint32_t a, b, c, d;
    uint32_t max_leaf;
    bool have_rdseed = false;
    bool have_rdrand = false;
    uint64_t early[8];
    int gathered = 0;

    cpuid(0, 0, &a, &b, &c, &d);
    max_leaf = a;
    if (max_leaf >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        have_rdrand = (c & (1u << 30)) != 0;
    }
    if (max_leaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        have_rdseed = (b & (1u << 18)) != 0;
    }

    entropy_pool_add(ENTROPY_BOOT, &boot_seed, sizeof(boot_seed), 4);
    for (int i = 0; i < 8; i++) {
        early[i] = read_tsc();
        if (i)
            early[i] ^= early[i - 1] << (i & 7);
    }
    entropy_pool_add(ENTROPY_BOOT, early, sizeof(early), 2);

    if (have_rdseed) {
        for (int i = 0; i < 4; i++) {
            uint64_t value;
            if (!cpu_rdseed64(&value))
                break;
            entropy_pool_add(ENTROPY_RDSEED, &value, sizeof(value), 64);
            gathered++;
        }
    }
    if (have_rdrand && gathered < 2) {
        for (int i = 0; i < 4; i++) {
            uint64_t value;
            if (!cpu_rdrand64(&value))
                break;
            entropy_pool_add(ENTROPY_RDRAND, &value, sizeof(value), 32);
            gathered++;
        }
    }
    hardware_source = gathered > 0;
    memset(early, 0, sizeof(early));

    /* Seed the generator immediately so all later entropy can be folded into
     * live state. Blocking interfaces still wait for ENTROPY_READY_BITS. */
    uint64_t flags = spin_lock_irqsave(&random_lock);
    crng_reseed_locked();
    spin_unlock_irqrestore(&random_lock, flags);
    kprintf("random: SHA-256 pool + ChaCha20 CSPRNG (%s, %u/%u bits)\n",
            hardware_source ? (have_rdseed ? "RDSEED" : "RDRAND")
                            : "timing entropy",
            credited_bits, ENTROPY_READY_BITS);
}

bool random_ready(void)
{
    uint64_t flags = spin_lock_irqsave(&random_lock);
    bool ready = credited_bits >= ENTROPY_READY_BITS;
    spin_unlock_irqrestore(&random_lock, flags);
    return ready;
}

unsigned random_entropy_bits(void)
{
    uint64_t flags = spin_lock_irqsave(&random_lock);
    unsigned bits = credited_bits;
    spin_unlock_irqrestore(&random_lock, flags);
    return bits;
}

bool random_has_hardware_source(void)
{
    return hardware_source;
}

int random_get_bytes(void *buf, size_t len, unsigned flags)
{
    uint8_t *out = buf;

    if ((!buf && len) || (flags & ~(RANDOM_NONBLOCK | RANDOM_STRONG)))
        return -1;
    while (!random_ready()) {
        if (flags & RANDOM_NONBLOCK)
            return -1;
        if (!sched_active)
            return -1;
        task_sleep_ticks(1);
    }
    if (len) {
        uint64_t irq = spin_lock_irqsave(&random_lock);
        crng_generate_locked(out, len);
        spin_unlock_irqrestore(&random_lock, irq);
    }
    return 0;
}

uint32_t random_u32(void)
{
    uint32_t value = 0;

    if (random_get_bytes(&value, sizeof(value), 0) < 0) {
        uint64_t sample = read_tsc();
        entropy_pool_add(ENTROPY_TIMER, &sample, sizeof(sample), 0);
        uint64_t irq = spin_lock_irqsave(&random_lock);
        crng_generate_locked((uint8_t *)&value, sizeof(value));
        spin_unlock_irqrestore(&random_lock, irq);
    }
    return value;
}
