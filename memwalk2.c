// memwalk2.c (C11, no C++ features)
// Memory access microbench: stream / stride / chase
//
// Build:
//   gcc -O2 -std=c11 -static -fno-omit-frame-pointer -o memwalk2 memwalk2.c
//
// Examples:
//   ./memwalk2 --mode stream --bytes 256M --iters 30
//   ./memwalk2 --mode stride --bytes 256M --stride-lines 4 --iters 80
//   ./memwalk2 --mode chase  --bytes 256M --iters 2 --seed 1

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

// size parsing (e.g., 256M, 1G, 64K)
static uint64_t parse_bytes(const char *s)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s) return 0;

    uint64_t mul = 1;
    if (*end) {
        if (!strcmp(end, "K") || !strcmp(end, "k")) mul = 1024ULL;
        else if (!strcmp(end, "M") || !strcmp(end, "m")) mul = 1024ULL * 1024ULL;
        else if (!strcmp(end, "G") || !strcmp(end, "g")) mul = 1024ULL * 1024ULL * 1024ULL;
        else return 0;
    }
    double bytes = v * (double)mul;
    if (bytes < 0.0) return 0;
    return (uint64_t)bytes;
}

// xorshift rng
static uint64_t rng_state = 0x123456789abcdef0ULL;

static inline uint64_t xorshift64(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

// Fisher-Yates shuffle
static void shuffle_u32(uint32_t *a, uint32_t n)
{
    if (n <= 1) return;
    for (uint32_t i = n - 1; i > 0; --i) {
        uint32_t j = (uint32_t)(xorshift64() % (uint64_t)(i + 1));
        uint32_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

// aligned alloc
static void *xaligned_alloc(size_t align, size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0) die("posix_memalign");
    return p;
}

static inline void fence(void)
{
    __asm__ __volatile__("" ::: "memory");
}

// modes
typedef enum { MW_STREAM, MW_STRIDE, MW_CHASE } mw_mode_t;

static mw_mode_t parse_mode(const char *s)
{
    if (!strcmp(s, "stream")) return MW_STREAM;
    if (!strcmp(s, "stride")) return MW_STRIDE;
    if (!strcmp(s, "chase"))  return MW_CHASE;
    fprintf(stderr, "Unknown mode: %s (use stream|stride|chase)\n", s);
    exit(2);
}

// workers
static void do_stream(uint8_t *buf, uint64_t n_lines, uint32_t line_size,
                      uint32_t loops, volatile uint64_t *checksum)
{
    for (uint32_t t = 0; t < loops; t++) {
        for (uint64_t i = 0; i < n_lines; i++) {
            *checksum += buf[i * (uint64_t)line_size];
        }
    }
}

static void do_stride(uint8_t *buf, uint64_t n_lines, uint32_t line_size,
                      uint32_t stride_lines, uint32_t loops, volatile uint64_t *checksum)
{
    uint64_t step = (uint64_t)stride_lines;
    if (step == 0) step = 1;
    for (uint32_t t = 0; t < loops; t++) {
        for (uint64_t i = 0; i < n_lines; i += step) {
            *checksum += buf[i * (uint64_t)line_size];
        }
    }
}

// Build pointer-chase permutation as a single cycle over lines.
// Store next index in first 4 bytes of each line.
static uint32_t build_chase(uint8_t *buf, uint64_t n_lines, uint32_t line_size, uint64_t seed)
{
    uint32_t *perm = (uint32_t *)malloc((size_t)n_lines * sizeof(uint32_t));
    if (!perm) die("malloc perm");

    for (uint32_t i = 0; i < (uint32_t)n_lines; i++) perm[i] = i;

    rng_state = seed ? seed : 1;
    shuffle_u32(perm, (uint32_t)n_lines);

    for (uint64_t k = 0; k < n_lines; k++) {
        uint32_t cur = perm[k];
        uint32_t nxt = perm[(k + 1) % n_lines];
        uint32_t *p = (uint32_t *)(buf + (uint64_t)cur * (uint64_t)line_size);
        *p = nxt;
    }

    uint32_t start = perm[0];
    free(perm);
    return start;
}

static void do_chase(uint8_t *buf, uint64_t n_lines, uint32_t line_size,
                     uint32_t loops, uint64_t seed, volatile uint64_t *checksum)
{
    uint32_t idx = build_chase(buf, n_lines, line_size, seed);
    fence();

    uint64_t steps = (uint64_t)loops * n_lines;
    for (uint64_t s = 0; s < steps; s++) {
        uint8_t *line = buf + (uint64_t)idx * (uint64_t)line_size;
        *checksum += line[8];               // read some payload
        idx = *(uint32_t *)line;            // dependent load
    }
    *checksum ^= (uint64_t)idx;
}

// main
int main(int argc, char **argv)
{
    mw_mode_t mode = MW_STREAM;
    uint64_t bytes = 256ULL * 1024ULL * 1024ULL;  // 256MiB
    uint32_t line_size = 64;                      // bytes
    uint32_t stride_lines = 4;                    // 4 lines = 256B
    uint32_t iters = 50;                          // loops (stream/stride)
    uint32_t warmup = 0;                          // warmup loops inside program
    uint64_t seed = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--mode") && i + 1 < argc) {
            mode = parse_mode(argv[++i]);
        } else if (!strcmp(a, "--bytes") && i + 1 < argc) {
            bytes = parse_bytes(argv[++i]);
            if (!bytes) { fprintf(stderr, "Bad --bytes\n"); return 2; }
        } else if (!strcmp(a, "--line") && i + 1 < argc) {
            line_size = (uint32_t)strtoul(argv[++i], NULL, 0);
            if (line_size < 16 || (line_size & (line_size - 1)) != 0) {
                fprintf(stderr, "--line must be power-of-two >= 16\n");
                return 2;
            }
        } else if (!strcmp(a, "--stride-lines") && i + 1 < argc) {
            stride_lines = (uint32_t)strtoul(argv[++i], NULL, 0);
            if (!stride_lines) { fprintf(stderr, "Bad --stride-lines\n"); return 2; }
        } else if (!strcmp(a, "--iters") && i + 1 < argc) {
            iters = (uint32_t)strtoul(argv[++i], NULL, 0);
            if (!iters) { fprintf(stderr, "Bad --iters\n"); return 2; }
        } else if (!strcmp(a, "--warmup") && i + 1 < argc) {
            warmup = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            seed = (uint64_t)strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--help")) {
            printf("memwalk2 options:\n");
            printf("  --mode stream|stride|chase\n");
            printf("  --bytes <N[K|M|G]>\n");
            printf("  --line <bytes>            (default 64)\n");
            printf("  --stride-lines <lines>    (stride mode; default 4)\n");
            printf("  --iters <N>\n");
            printf("  --warmup <N>\n");
            printf("  --seed <N>\n");
            return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s (use --help)\n", a);
            return 2;
        }
    }

    if (bytes < line_size) bytes = line_size;
    bytes = (bytes / line_size) * line_size;

    uint64_t n_lines = bytes / line_size;
    if (n_lines < 1024) {
        fprintf(stderr, "Working set too small (%" PRIu64 " lines). Increase --bytes.\n", n_lines);
        return 2;
    }

    uint8_t *buf = (uint8_t *)xaligned_alloc(64, (size_t)bytes);

    // Touch pages to ensure allocation
    for (uint64_t i = 0; i < bytes; i += 4096) {
        buf[i] = (uint8_t)(i ^ 0xA5);
    }
    fence();

    volatile uint64_t checksum = 0;

    // Program-level warmup
    if (warmup) {
        if (mode == MW_STREAM) {
            do_stream(buf, n_lines, line_size, warmup, &checksum);
        } else if (mode == MW_STRIDE) {
            do_stride(buf, n_lines, line_size, stride_lines, warmup, &checksum);
        } else {
            do_chase(buf, n_lines, line_size, warmup, seed, &checksum);
        }
        fence();
    }

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    if (mode == MW_STREAM) {
        do_stream(buf, n_lines, line_size, iters, &checksum);
    } else if (mode == MW_STRIDE) {
        do_stride(buf, n_lines, line_size, stride_lines, iters, &checksum);
    } else {
        do_chase(buf, n_lines, line_size, iters, seed, &checksum);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double dt = (double)(ts1.tv_sec - ts0.tv_sec) +
                (double)(ts1.tv_nsec - ts0.tv_nsec) * 1e-9;

    const char *mname = (mode == MW_STREAM) ? "stream" :
                        (mode == MW_STRIDE) ? "stride" : "chase";
    printf("memwalk2: mode=%s bytes=%" PRIu64 " line=%u iters=%u stride_lines=%u seed=%" PRIu64 "\n",
           mname, bytes, line_size, iters, stride_lines, seed);
    printf("memwalk2: checksum=%" PRIu64 " host_seconds=%.6f\n",
           (uint64_t)checksum, dt);

    free(buf);
    return 0;
}
