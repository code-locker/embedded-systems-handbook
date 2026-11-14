/*
 * 01_assert_and_logging.c — printf debugging done WELL: levels, location, time, errno, hexdump
 *
 * Concept : A good log line answers WHEN (monotonic timestamp), WHERE (file:
 *           line function), HOW BAD (level) and WHAT (values, errno text).
 *           Logs go to stderr (unbuffered, separate from data on stdout).
 *           assert() documents and checks INTERNAL invariants and vanishes with
 *           -DNDEBUG; errors that can really happen at run time (bad input, I/O)
 *           need real error handling, not assert.
 * Build   : make run P=dsa/03-debugging/c/01_assert_and_logging.c
 *           LOG_LEVEL=3 ./build/dsa/03-debugging/c/01_assert_and_logging   (show DEBUG lines)
 * Read    : ../README.md  (section "3.4 printf/log debugging done well" and "3.5 assert")
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum log_level { LOG_ERR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DBG = 3 };
static int g_log_level = LOG_INFO;       /* runtime switch: no rebuild needed */
static unsigned g_log_lines = 0;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);   /* monotonic: never jumps with NTP */
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The format attribute makes gcc type-check the printf-style arguments. */
__attribute__((format(printf, 5, 6)))
static void log_at(int level, const char *file, int line, const char *func, const char *fmt, ...)
{
    static const char *names[] = {"ERR ", "WARN", "INFO", "DBG "};
    static double t0;
    if (level > g_log_level)
        return;
    if (t0 == 0) t0 = now_s();
    fprintf(stderr, "[%9.6f] %s %s:%d %s(): ", now_s() - t0, names[level], file, line, func);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    g_log_lines++;
}

/* __FILE__/__LINE__/__func__ must be captured at the CALL SITE: only a macro can do that. */
#define LOG(level, ...) log_at(level, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define ERR(...)  LOG(LOG_ERR, __VA_ARGS__)
#define WARN(...) LOG(LOG_WARN, __VA_ARGS__)
#define INFO(...) LOG(LOG_INFO, __VA_ARGS__)
#define DBG(...)  LOG(LOG_DBG, __VA_ARGS__)

/* A check that stays in release builds and says WHAT failed. */
#define CHECK(cond) do { if (!(cond)) { ERR("CHECK failed: %s", #cond); abort(); } } while (0)

/* Hex dump: THE tool for buffers, packets, register blocks. */
static void hexdump(const char *title, const void *data, size_t len)
{
    const unsigned char *p = data;
    fprintf(stderr, "%s (%zu bytes):\n", title, len);
    for (size_t off = 0; off < len; off += 16) {
        fprintf(stderr, "  %04zx: ", off);
        for (size_t i = 0; i < 16; i++)
            off + i < len ? fprintf(stderr, "%02x ", p[off + i]) : fprintf(stderr, "   ");
        fprintf(stderr, " |");
        for (size_t i = 0; i < 16 && off + i < len; i++)
            fputc(isprint(p[off + i]) ? p[off + i] : '.', stderr);
        fprintf(stderr, "|\n");
    }
}

static int parse_fps(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v <= 0 || v > 240) {
        WARN("rejecting fps string \"%s\"", s);      /* expected error: handle it */
        return -1;
    }
    *out = (int)v;
    DBG("parsed fps=%d", *out);
    return 0;
}

int main(void)
{
    const char *lvl = getenv("LOG_LEVEL");
    if (lvl) g_log_level = atoi(lvl);

    INFO("starting, log level %d", g_log_level);

    int fps = 0;
    CHECK(parse_fps("30", &fps) == 0 && fps == 30);
    CHECK(parse_fps("abc", &fps) == -1);
    CHECK(parse_fps("9999", &fps) == -1);

    /* errno + strerror: always log the reason a syscall failed. */
    FILE *f = fopen("/nonexistent/dir/file", "r");
    if (!f)
        WARN("fopen failed: %s (errno %d)", strerror(errno), errno);
    CHECK(f == NULL && errno == ENOENT);

    struct { uint32_t magic; uint16_t w, h; char tag[8]; } hdr = {0xCAFEF00Du, 1920, 1080, "frame"};
    hexdump("frame header", &hdr, sizeof hdr);

    /* assert: an internal invariant. Compiled out with -DNDEBUG — so NEVER put
     * side effects inside: assert(x = compute()) disappears in release.      */
    int widths[] = {640, 1280, 1920};
    for (size_t i = 1; i < sizeof widths / sizeof widths[0]; i++)
        assert(widths[i] > widths[i - 1] && "widths must be sorted");

    _Static_assert(sizeof(uint32_t) == 4, "compile-time assert costs nothing at run time");

    CHECK(g_log_lines >= 4);
    INFO("done, %u log lines", g_log_lines);
    printf("All checks passed.\n");
    return 0;
}
