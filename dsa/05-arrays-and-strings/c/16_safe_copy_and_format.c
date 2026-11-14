/*
 * 16_safe_copy_and_format.c — copying and formatting strings without overflow
 *
 * Concept : The dangerous functions (strcpy, strcat, sprintf, gets) do not
 *           know the destination size. The "n" versions are not automatically
 *           safe either (strncpy may not terminate; strncat's n is NOT the
 *           buffer size). The tool that is both standard and safe is
 *           snprintf: it always terminates and RETURNS THE LENGTH IT WANTED,
 *           which is how you detect truncation. We also build a tiny printf
 *           with <stdarg.h> to see how varargs work, and explain why
 *           printf(user_input) is a security hole.
 * Build   : make run P=dsa/05-arrays-and-strings/c/16_safe_copy_and_format.c
 * Read    : ../notes/strings.md  (section "Formatting and security")
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

/* --------------------------------------------------------------------------
 * Safe copy helper built on snprintf.
 * Returns true if the whole string fit. O(strlen(src)).
 *   n = snprintf(dst, size, "%s", src)
 *   n <  size  -> everything copied, terminated
 *   n >= size  -> truncated to size-1 chars, still terminated
 *   n <  0     -> encoding error (practically never for "%s")           */
static bool copy_str(char *dst, size_t size, const char *src)
{
    int n = snprintf(dst, size, "%s", src);
    return n >= 0 && (size_t)n < size;
}

/* Safe append: track the used length yourself (O(piece)), instead of strcat
 * re-scanning (O(total)) and instead of strncat's confusing n.          */
static bool append_str(char *dst, size_t size, size_t *used, const char *src)
{
    if (*used >= size)
        return false;
    int n = snprintf(dst + *used, size - *used, "%s", src);
    if (n < 0)
        return false;
    if ((size_t)n >= size - *used) {
        *used = size - 1; /* truncated: buffer is now full */
        return false;
    }
    *used += (size_t)n;
    return true;
}

/* --------------------------------------------------------------------------
 * A mini snprintf: supports %d %u %x %s %c %%.
 *
 * How varargs work (System V x86-64 / AAPCS64): the caller puts the extra
 * arguments in registers and on the stack; va_list is a cursor over them.
 * va_arg(ap, int) says "give me the next one AND I promise it is an int".
 * There is NO type information at run time — the format string is the only
 * description of what was passed. Lie in the format and va_arg reads garbage
 * (that is the format-string vulnerability, below).
 *
 * Note: char and short are promoted to int when passed through "...",
 * float to double. That's why %c reads an int.
 *
 * Returns the length the full output WOULD have (C99 snprintf contract).
 */
static void emit(char *buf, size_t size, size_t *pos, char c)
{
    if (*pos + 1 < size) /* leave room for '\0' */
        buf[*pos] = c;
    (*pos)++; /* count even when it doesn't fit */
}

static void emit_unsigned(char *buf, size_t size, size_t *pos, unsigned long v,
                          unsigned base)
{
    char tmp[32];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n--)
        emit(buf, size, pos, tmp[n]);
}

__attribute__((format(printf, 3, 4)))
static int mini_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t pos = 0;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            emit(buf, size, &pos, *f);
            continue;
        }
        switch (*++f) {
        case 'd': {
            int v = va_arg(ap, int);
            /* careful with INT_MIN: -v overflows, so widen first */
            long lv = v;
            if (lv < 0) {
                emit(buf, size, &pos, '-');
                lv = -lv;
            }
            emit_unsigned(buf, size, &pos, (unsigned long)lv, 10);
            break;
        }
        case 'u':
            emit_unsigned(buf, size, &pos, va_arg(ap, unsigned), 10);
            break;
        case 'x':
            emit_unsigned(buf, size, &pos, va_arg(ap, unsigned), 16);
            break;
        case 'c':
            emit(buf, size, &pos, (char)va_arg(ap, int)); /* promoted! */
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s)
                emit(buf, size, &pos, *s++);
            break;
        }
        case '%':
            emit(buf, size, &pos, '%');
            break;
        case '\0':
            f--; /* lone '%' at end: stop */
            break;
        default: /* unknown conversion: print it literally */
            emit(buf, size, &pos, '%');
            emit(buf, size, &pos, *f);
        }
    }
    va_end(ap);
    if (size > 0)
        buf[pos < size ? pos : size - 1] = '\0';
    return (int)pos;
}

/* Make strings the compiler can't see through, so gcc's compile-time
 * -Wformat-truncation / -Wstringop-overflow analysis doesn't fire on the
 * deliberate truncation demos (they are about RUN-time sizes).          */
static const char *runtime(const char *s)
{
    static char copy[128];
    volatile size_t n = strlen(s);
    memcpy(copy, s, n + 1);
    return copy;
}

int main(void)
{
    puts("== 1. the copy family at a glance ==");
    /*
     *  function          knows dst size?  always '\0'?  detect truncation?
     *  strcpy            no               yes           -- (overflows)
     *  strncpy(d,s,n)    yes (n)          NO            only by checking d[n-1]
     *  strncat(d,s,n)    no! n = max chars TO APPEND; must pass size-strlen(d)-1
     *  strlcpy(d,s,n)    yes              yes           return >= n
     *  snprintf(d,n,..)  yes              yes           return >= n
     *  memcpy(d,s,n)     you pass exact n not a string op; you add '\0'
     */
    char dev[12];
    CHECK(copy_str(dev, sizeof dev, runtime("/dev/ttyS2")));
    printf("  fits     : \"%s\"\n", dev);
    bool ok = copy_str(dev, sizeof dev, runtime("/dev/serial/by-id/usb-FTDI"));
    printf("  truncated: \"%s\" (ok=%d) — still terminated, still safe\n", dev, ok);
    CHECK(!ok && strlen(dev) == sizeof dev - 1);

    puts("== 2. snprintf's return value = what it WANTED to write ==");
    char small[8];
    int want = snprintf(small, sizeof small, "%s-%d", runtime("pipeline"), 42);
    printf("  wanted %d chars, buffer holds %zu -> \"%s\"\n", want,
           sizeof small - 1, small);
    CHECK(want == 11 && strcmp(small, "pipelin") == 0);
    /* Size query idiom: pass NULL,0 to learn the length, then allocate. */
    int len = snprintf(NULL, 0, "%s:%d", runtime("192.168.7.2"), 5000);
    char *exact = malloc((size_t)len + 1); /* we own it, we free it */
    CHECK(exact);
    snprintf(exact, (size_t)len + 1, "%s:%d", runtime("192.168.7.2"), 5000);
    printf("  exact-size alloc: %d bytes -> \"%s\"\n", len, exact);
    CHECK(strcmp(exact, "192.168.7.2:5000") == 0);
    free(exact);

    puts("== 3. bounded append without strcat/strncat ==");
    char cmd[24];
    size_t used = 0;
    cmd[0] = '\0';
    CHECK(append_str(cmd, sizeof cmd, &used, "gst-launch-1.0 "));
    CHECK(append_str(cmd, sizeof cmd, &used, "v4l2src "));
    CHECK(!append_str(cmd, sizeof cmd, &used, "! videoconvert ! autovideosink"));
    printf("  \"%s\" (used %zu of %zu) — truncation reported\n", cmd, used,
           sizeof cmd);
    CHECK(strlen(cmd) == sizeof cmd - 1);

    puts("== 4. our mini_snprintf vs libc snprintf ==");
    struct { char a[64], b[64]; int ra, rb; } t;
    t.ra = mini_snprintf(t.a, sizeof t.a, "%s=%d (0x%x) %c%% %u", "irq", -42, 255u, 'Z', 7u);
    t.rb = snprintf(t.b, sizeof t.b, "%s=%d (0x%x) %c%% %u", "irq", -42, 255u, 'Z', 7u);
    printf("  mini: \"%s\"\n  libc: \"%s\"\n", t.a, t.b);
    CHECK(t.ra == t.rb && strcmp(t.a, t.b) == 0);
    volatile int int_min = -2147483647 - 1; /* volatile: hide from -Wformat-truncation */
    t.ra = mini_snprintf(t.a, 6, "%d", int_min);
    t.rb = snprintf(t.b, 6, "%d", int_min);
    CHECK(t.ra == t.rb && strcmp(t.a, t.b) == 0); /* INT_MIN + truncation */
    t.ra = mini_snprintf(NULL, 0, "%s", "count me");
    CHECK(t.ra == 8);

    puts("== 5. format-string vulnerability (explained, not executed) ==");
    /*
     *  char name[64]; read_from_network(name);
     *  printf(name);               // BUG: name is used as the FORMAT
     *  printf("%s", name);         // correct: name is DATA
     *
     *  If an attacker sends  "%x %x %x %x"  printf walks va_arg over
     *  arguments that were never passed: it prints registers and stack
     *  words — leaking canaries, return addresses, pointers (ASLR bypass).
     *  "%s" dereferences a garbage word (crash). "%n" WRITES the number of
     *  bytes printed so far into the address it finds: an arbitrary write
     *  primitive, i.e. remote code execution. glibc's _FORTIFY_SOURCE=2
     *  refuses %n in writable format strings; -Wformat-security (in our
     *  -Wformat=2) warns about printf(non_literal) at compile time.
     */
    const char *evil = runtime("%x %x %n");
    char safe_out[64];
    snprintf(safe_out, sizeof safe_out, "%s", evil); /* data, not format */
    printf("  printed safely as data: \"%s\"\n", safe_out);
    CHECK(strcmp(safe_out, "%x %x %n") == 0);

    puts("== 6. stack canaries, in one picture ==");
    /*
     *  -fstack-protector-strong (Ubuntu's default) puts a random value
     *  between local arrays and the saved return address:
     *
     *   higher addr  +-------------------+
     *                | return address    |  <- what an overflow wants to hijack
     *                | saved frame ptr   |
     *                | CANARY (random)   |  <- checked before `ret`
     *                | char buf[16]      |  <- strcpy(buf, long_input) grows UP
     *   lower addr   +-------------------+
     *
     *  On return, if the canary changed: __stack_chk_fail() prints
     *  "*** stack smashing detected ***" and aborts — the hijacked return
     *  address is never used. It does NOT stop overwrites of other locals
     *  and doesn't help with heap overflows. ASan catches the overflow at
     *  the first bad byte instead. See 17_stack_smash_bug.c to watch both.
     */
    puts("  see the comment above and 17_stack_smash_bug.c");

    puts("\nall checks passed");
    return 0;
}
