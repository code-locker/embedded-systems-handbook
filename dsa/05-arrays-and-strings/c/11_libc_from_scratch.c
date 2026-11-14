/*
 * 11_libc_from_scratch.c — <string.h> and friends, written by hand
 *
 * Concept : Every str* function is a short loop over bytes that stops at
 *           '\0'. Writing them yourself burns in three facts: (1) they are
 *           O(n) because the length is not stored, (2) none of them knows the
 *           size of the destination buffer, (3) the corner cases (strncpy not
 *           terminating, strtok's hidden static state, strtol's error
 *           reporting) are where real bugs come from. Each my_* function is
 *           checked against the real libc version.
 * Build   : make run P=dsa/05-arrays-and-strings/c/11_libc_from_scratch.c
 * Read    : ../notes/strings.md  (section "The standard library from scratch")
 */
#define _POSIX_C_SOURCE 200809L /* for strtok_r */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
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

/* ---------------------------------------------------------------- strlen --
 * O(n): the ONLY way to know the length is to walk to the '\0'.
 *   "abc"  ->  [a][b][c][\0]
 *               ^p ------> ^p stops here; length = p - s = 3            */
static size_t my_strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

/* ---------------------------------------------------------------- strcpy --
 * O(n). Copies INCLUDING the '\0'. Has no idea how big dst is: if
 * strlen(src) >= sizeof dst, it writes past the end (buffer overflow).
 * The classic one-liner: the assignment copies, the loop test checks
 * whether the byte just copied was the terminator.                      */
static char *my_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

/* --------------------------------------------------------------- strncpy --
 * O(n) — and n, not strlen(src)! strncpy was designed for fixed-width,
 * NOT-necessarily-terminated fields (old UNIX directory entries: 14-byte
 * names). Its two surprises:
 *   1. if strlen(src) >= n, NO '\0' is written      -> not a C string!
 *   2. if strlen(src) <  n, the rest is padded with '\0' up to n bytes
 *      (so strncpy(buf, "x", 4096) writes 4096 bytes every time).
 *
 *   strncpy(d, "hello", 3):  d = [h][e][l]           <- no terminator
 *   strncpy(d, "hi",    5):  d = [h][i][\0][\0][\0]  <- padded        */
static char *my_strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

/* --------------------------------------------------------------- strlcpy --
 * BSD's fix (in glibc only since 2.38): copies at most size-1 bytes, ALWAYS
 * terminates (if size > 0), and returns strlen(src) so you can detect
 * truncation:  if (my_strlcpy(d, s, sizeof d) >= sizeof d) -> truncated.
 * O(strlen(src)) because it must compute the full source length.        */
static size_t my_strlcpy(char *dst, const char *src, size_t size)
{
    size_t srclen = my_strlen(src);
    if (size > 0) {
        size_t n = srclen < size - 1 ? srclen : size - 1;
        for (size_t i = 0; i < n; i++)
            dst[i] = src[i];
        dst[n] = '\0';
    }
    return srclen;
}

/* ---------------------------------------------------------------- strcat --
 * O(strlen(dst) + strlen(src)): it first has to FIND the end of dst.
 * That is why a loop of strcat(buf, piece) is O(n^2) — "Shlemiel the
 * painter": every call walks the whole growing string again. Keep a
 * pointer/length to the end instead (see 14_string_builder.c).         */
static char *my_strcat(char *dst, const char *src)
{
    my_strcpy(dst + my_strlen(dst), src);
    return dst;
}

/* ---------------------------------------------------------------- strcmp --
 * O(min(len))  — stops at first difference or at the end.
 * Compares as UNSIGNED char (the standard says so): otherwise byte 0xE9
 * ('é' in Latin-1) would be negative and sort before 'a'.
 * Returns <0, 0, >0 — NOT necessarily -1/0/1; never test `== -1`.      */
static int my_strcmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (*x && *x == *y) {
        x++;
        y++;
    }
    return (int)*x - (int)*y;
}

/* ---------------------------------------------------------------- strchr --
 * O(n). Note: searching for '\0' is legal and returns a pointer to the
 * terminator — handy for "find the end".                                */
static char *my_strchr(const char *s, int c)
{
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch)
            return (char *)(size_t)s; /* libc's signature drops const too */
        if (*s == '\0')
            return NULL;
    }
}

/* ---------------------------------------------------------------- strstr --
 * Naive: O(n*m) worst case ("aaaa...ab" in "aaaa...a"). glibc uses the
 * Two-Way algorithm (O(n+m), O(1) space). KMP, also O(n+m), is in
 * 18_string_algorithms.c.                                               */
static char *my_strstr(const char *hay, const char *needle)
{
    if (*needle == '\0')
        return (char *)(size_t)hay; /* empty needle matches at position 0 */
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (*n == '\0')
            return (char *)(size_t)hay;
    }
    return NULL;
}

/* ---------------------------------------------------------------- memset --
 * O(n). Works on BYTES: memset(int_array, 1, ...) gives 0x01010101 per int,
 * not 1. Only 0 and -1 (0xFF) are "safe" fill values for ints.          */
static void *my_memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

/* ---------------------------------------------------------------- strtok --
 * Splits in place by WRITING '\0' over each delimiter, and remembers where
 * it stopped in a hidden STATIC variable:
 *
 *   s = "a,b,,c"  after 3 calls:  [a][\0][b][\0][,][c][\0]
 *                                              ^ saved (static!)
 *
 * Consequences: (1) it modifies its input (can't tokenise a literal),
 * (2) not reentrant — tokenising two strings interleaved, or from two
 * threads, corrupts the saved pointer, (3) empty fields vanish (",,").
 * strtok_r makes the caller own the "saved" pointer -> reentrant.       */
static char *my_strtok_r(char *s, const char *delim, char **save)
{
    if (s == NULL)
        s = *save;
    s += strspn(s, delim); /* skip leading delimiters */
    if (*s == '\0') {
        *save = s;
        return NULL;
    }
    char *end = s + strcspn(s, delim); /* first delimiter after token */
    if (*end != '\0') {
        *end = '\0';       /* cut the string here ...                    */
        *save = end + 1;   /* ... and resume after it next time          */
    } else {
        *save = end;       /* token runs to end of string                */
    }
    return s;
}

static char *my_strtok(char *s, const char *delim)
{
    static char *hidden; /* <- THE problem: one global cursor for everyone */
    return my_strtok_r(s, delim, &hidden);
}

/* ----------------------------------------------------------------- atoi --
 * atoi has NO error reporting: atoi("abc") == 0 == atoi("0"), and overflow
 * is undefined behaviour. Use strtol (below) for anything from outside.  */
static int my_atoi(const char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    int sign = 1;
    if (*s == '+' || *s == '-')
        sign = (*s++ == '-') ? -1 : 1;
    int v = 0;
    while (isdigit((unsigned char)*s))
        v = v * 10 + (*s++ - '0'); /* real atoi: overflow = UB; we avoid big inputs */
    return sign * v;
}

/* --------------------------------------------------- strtol, done right --
 * The full checking recipe for strtol: set errno = 0 first, then check
 *   (a) endptr == s          -> no digits at all
 *   (b) errno == ERANGE      -> overflow (value clamped to LONG_MAX/MIN)
 *   (c) *endptr != '\0'      -> trailing garbage ("12abc")
 * parse_long wraps this into a bool so callers can't forget a step.     */
static bool parse_long(const char *s, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s)
        return false;          /* "", "abc", "   "            */
    if (errno == ERANGE)
        return false;          /* "99999999999999999999"     */
    while (isspace((unsigned char)*end))
        end++;                 /* allow trailing whitespace "42\n" */
    if (*end != '\0')
        return false;          /* "12abc"                    */
    *out = v;
    return true;
}

/* Our own overflow-safe parser, to see how strtol detects overflow:
 * before doing v = v*10 + d, check v > (LONG_MAX - d) / 10.
 * We accumulate NEGATIVELY because |LONG_MIN| = LONG_MAX + 1: the most
 * negative value fits, its positive twin would not.                   */
static bool my_parse_long(const char *s, long *out)
{
    while (isspace((unsigned char)*s))
        s++;
    bool neg = false;
    if (*s == '+' || *s == '-')
        neg = (*s++ == '-');
    if (!isdigit((unsigned char)*s))
        return false;
    long v = 0; /* always <= 0 */
    while (isdigit((unsigned char)*s)) {
        int d = *s++ - '0';
        if (v < (LONG_MIN + d) / 10)
            return false; /* v*10 - d would go below LONG_MIN */
        v = v * 10 - d;
    }
    while (isspace((unsigned char)*s))
        s++;
    if (*s != '\0')
        return false;
    if (!neg) {
        if (v == LONG_MIN)
            return false; /* +9223372036854775808 does not fit */
        v = -v;
    }
    *out = v;
    return true;
}

/* ----------------------------------------------------------------- itoa --
 * Not standard C (use snprintf), but a classic. Digits come out backwards
 * (v % 10 is the LAST digit), so fill from the end of a local buffer.
 * Work with the NEGATIVE value so INT_MIN doesn't overflow when negated.
 *   -305:  tmp = [ ][ ][ ][ ][-][3][0][5][\0]
 *                            ^ start                                   */
static char *my_itoa(int value, char *out, size_t outsz)
{
    char tmp[16];
    char *p = tmp + sizeof tmp;
    *--p = '\0';
    int v = value < 0 ? value : -value; /* v <= 0 */
    do {
        *--p = (char)('0' - v % 10); /* v%10 is in -9..0 for v <= 0 */
        v /= 10;
    } while (v != 0);
    if (value < 0)
        *--p = '-';
    my_strlcpy(out, p, outsz);
    return out;
}

static int sign(int x) { return (x > 0) - (x < 0); }

int main(void)
{
    puts("== strlen / strcpy / strcat ==");
    const char *samples[] = {"", "a", "hello", "embedded linux", "caf\xC3\xA9"};
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++)
        CHECK(my_strlen(samples[i]) == strlen(samples[i]));
    char buf[64];
    my_strcpy(buf, "Beagle");
    my_strcat(buf, "Bone ");
    my_strcat(buf, "AI-64");
    printf("  built \"%s\" (strlen %zu)\n", buf, my_strlen(buf));
    CHECK(strcmp(buf, "BeagleBone AI-64") == 0);

    puts("== strncpy: the non-terminating trap ==");
    char small[4];
    my_memset(small, 'X', sizeof small);
    my_strncpy(small, "hello", sizeof small);
    /* small = [h][e][l][l]  -- NO '\0'. printf("%s", small) would read off
     * the end of the array. Check byte by byte instead.                 */
    CHECK(memcmp(small, "hell", 4) == 0);
    printf("  strncpy(small, \"hello\", 4) -> last byte is '%c', not '\\0'\n",
           small[3]);
    small[sizeof small - 1] = '\0'; /* the mandatory manual fix */
    CHECK(strcmp(small, "hel") == 0);
    char pad[6];
    my_strncpy(pad, "hi", sizeof pad);
    CHECK(pad[2] == 0 && pad[5] == 0); /* zero padded all the way */
    char ref[6];
    strncpy(ref, "hi", sizeof ref);
    CHECK(memcmp(pad, ref, sizeof pad) == 0);

    puts("== strlcpy: truncation you can detect ==");
    char dst[8];
    size_t need = my_strlcpy(dst, "TDA4VM-J721E", sizeof dst);
    printf("  strlcpy into 8 bytes -> \"%s\", wanted %zu chars -> %s\n", dst,
           need, need >= sizeof dst ? "TRUNCATED" : "ok");
    CHECK(need == 12 && strcmp(dst, "TDA4VM-") == 0);
    CHECK(my_strlcpy(dst, "ok", sizeof dst) == 2 && strcmp(dst, "ok") == 0);

    puts("== strcmp / strchr / strstr ==");
    const char *pairs[][2] = {{"abc", "abd"}, {"abc", "abc"}, {"abc", "ab"},
                              {"", "a"},     {"\xE9", "a"},  {"Zeta", "alpha"}};
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        CHECK(sign(my_strcmp(pairs[i][0], pairs[i][1])) ==
              sign(strcmp(pairs[i][0], pairs[i][1])));
    CHECK(my_strcmp("\xE9", "a") > 0); /* unsigned compare: 0xE9 > 'a' */
    const char *path = "/dev/ttyS2";
    CHECK(my_strchr(path, 't') == strchr(path, 't'));
    CHECK(my_strchr(path, 'q') == NULL);
    CHECK(my_strchr(path, '\0') == path + strlen(path));
    const char *hay = "ababcabcabababd";
    const char *needles[] = {"ababd", "abc", "", "zzz", "d", hay};
    for (size_t i = 0; i < sizeof needles / sizeof needles[0]; i++)
        CHECK(my_strstr(hay, needles[i]) == strstr(hay, needles[i]));
    printf("  strstr(\"%s\", \"ababd\") at offset %td\n", hay,
           my_strstr(hay, "ababd") - hay);

    puts("== memset works on bytes ==");
    int ints[4];
    my_memset(ints, 1, sizeof ints);
    printf("  memset(ints, 1, ...) -> ints[0] = 0x%08x (not 1!)\n",
           (unsigned)ints[0]);
    CHECK(ints[0] == 0x01010101);
    my_memset(ints, 0, sizeof ints);
    CHECK(ints[3] == 0);

    puts("== strtok vs strtok_r ==");
    char line[] = "eth0,192.168.7.2,,up";
    char *tok;
    const char *expect[] = {"eth0", "192.168.7.2", "up"};
    int k = 0;
    for (tok = my_strtok(line, ","); tok; tok = my_strtok(NULL, ","), k++) {
        printf("  token %d: \"%s\"\n", k, tok);
        CHECK(strcmp(tok, expect[k]) == 0);
    }
    CHECK(k == 3); /* the empty field between ",," disappeared */

    /* Nested tokenising: split "k=v;k=v" by ';' then each by '='.
     * With plain strtok the inner loop would overwrite the outer cursor.
     * With strtok_r each loop owns its own save pointer.               */
    char cfg[] = "baud=115200;bits=8;parity=none";
    char *outer_save, *inner_save;
    int pairs_seen = 0;
    for (char *kv = my_strtok_r(cfg, ";", &outer_save); kv;
         kv = my_strtok_r(NULL, ";", &outer_save)) {
        char *key = my_strtok_r(kv, "=", &inner_save);
        char *val = my_strtok_r(NULL, "=", &inner_save);
        CHECK(key && val);
        printf("  %s -> %s\n", key, val);
        pairs_seen++;
    }
    CHECK(pairs_seen == 3);
    /* and it agrees with libc's strtok_r */
    char a1[] = "  a  bb ccc ", a2[] = "  a  bb ccc ";
    char *s1, *s2, *t1 = my_strtok_r(a1, " ", &s1), *t2 = strtok_r(a2, " ", &s2);
    while (t1 || t2) {
        CHECK(t1 && t2 && strcmp(t1, t2) == 0);
        t1 = my_strtok_r(NULL, " ", &s1);
        t2 = strtok_r(NULL, " ", &s2);
    }

    puts("== atoi vs strtol with error checks ==");
    CHECK(my_atoi("  -42xyz") == atoi("  -42xyz"));
    CHECK(my_atoi("abc") == 0 && my_atoi("0") == 0); /* indistinguishable! */
    struct { const char *in; bool ok; long v; } cases[] = {
        {"42", true, 42},           {"  -17  ", true, -17},
        {"", false, 0},             {"abc", false, 0},
        {"12abc", false, 0},        {"9223372036854775807", true, LONG_MAX},
        {"-9223372036854775808", true, LONG_MIN},
        {"9223372036854775808", false, 0}, {"+", false, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        long v1 = 0, v2 = 0;
        bool ok1 = parse_long(cases[i].in, &v1);
        bool ok2 = my_parse_long(cases[i].in, &v2);
        printf("  [%s]%*s strtol:%s  mine:%s\n", cases[i].in, (int)(24 - strlen(cases[i].in)), "", ok1 ? "ok " : "ERR",
               ok2 ? "ok " : "ERR");
        CHECK(ok1 == cases[i].ok && ok2 == cases[i].ok);
        if (ok1)
            CHECK(v1 == cases[i].v && v2 == cases[i].v);
    }

    puts("== itoa ==");
    int nums[] = {0, 7, -305, 2147483647, -2147483647 - 1};
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        char mine[16], theirs[16];
        my_itoa(nums[i], mine, sizeof mine);
        snprintf(theirs, sizeof theirs, "%d", nums[i]);
        printf("  %d -> \"%s\"\n", nums[i], mine);
        CHECK(strcmp(mine, theirs) == 0);
    }

    puts("\nall checks passed");
    return 0;
}
