/*
 * 10_strings_char_pointers.c — the C string model: char* + a '\0' terminator
 *
 * Concept : C has no string type. A "string" is a pointer to the first char
 *           of a run of bytes that ends with a 0 byte. Its length is not
 *           stored anywhere: strlen walks until '\0' (O(n)). Every string bug
 *           is one of: missing terminator, buffer too small, writing a literal.
 * Build   : make run P=dsa/01-pointers/c/10_strings_char_pointers.c
 * Read    : ../README.md  (section "3.10 The char* string model")
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Hand-written versions of the classics, pointer style. */
static size_t my_strlen(const char *s)
{
    const char *p = s;
    while (*p)          /* stop at the '\0' byte */
        p++;
    return (size_t)(p - s);   /* O(n): pointer difference = length */
}

/* Copies at most cap-1 chars and ALWAYS terminates (like BSD strlcpy).
 * Returns strlen(src) so the caller can detect truncation (ret >= cap). */
static size_t my_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t len = my_strlen(src);
    if (cap) {
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

static void reverse_in_place(char *s)
{
    size_t n = my_strlen(s);
    if (n < 2)
        return;
    char *l = s, *r = s + n - 1;     /* r = last real char, NOT the '\0' */
    while (l < r) {                  /* two pointers walking toward each other */
        char t = *l;
        *l++ = *r;
        *r-- = t;
    }
}

static int my_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    /* compare as unsigned char, exactly like the real strcmp */
    return (unsigned char)*a - (unsigned char)*b;
}

/* Thin wrappers so that gcc cannot see the constant sizes at the call site
 * and warn: here we WANT to demonstrate truncation on purpose.           */
static void copy_n(char *dst, const char *src, size_t n) { strncpy(dst, src, n); }
static int format_frame(char *out, size_t cap, int seq) { return snprintf(out, cap, "frame-%d", seq); }

int main(void)
{
    /*
     *  const char *lit = "cam";        char arr[] = "cam";
     *
     *  lit (stack, 8 B)    .rodata (read-only)      arr (stack, 4 B, writable copy)
     *  +---------+        +----+----+----+----+     +----+----+----+----+
     *  | 0x5601..|──────► | 'c'| 'a'| 'm'| \0 |     | 'c'| 'a'| 'm'| \0 |
     *  +---------+        +----+----+----+----+     +----+----+----+----+
     */
    const char *lit = "cam";
    char arr[] = "cam";
    arr[0] = 'C';          /* fine: our own array */
    /* ((char *)lit)[0] = 'C';   UB: segfault, the page is read-only */
    assert(sizeof arr == 4 && my_strlen(lit) == 3);

    /* The terminator is a real byte you must budget for. */
    char small[4];
    size_t need = my_strlcpy(small, "camera", sizeof small);
    printf("strlcpy(\"camera\") into 4 bytes -> \"%s\" (needed %zu+1, truncated)\n", small, need);
    assert(strcmp(small, "cam") == 0 && need >= sizeof small);

    /* strncpy does NOT terminate when the source is too long — avoid it. */
    char nt[3];
    copy_n(nt, "abcdef", sizeof nt);  /* strncpy inside */            /* nt = 'a','b','c' — no '\0'! */
    assert(nt[2] == 'c');                        /* printing nt now would overrun */

    /* snprintf: the safe general tool; returns the length it WANTED to write. */
    char out[8];
    int wanted = format_frame(out, sizeof out, 12345);   /* snprintf inside */
    printf("snprintf wanted %d chars, got \"%s\"\n", wanted, out);
    assert(wanted == 11 && strlen(out) == 7);

    char word[] = "pointer";
    reverse_in_place(word);
    assert(strcmp(word, "retniop") == 0);

    assert(my_strcmp("abc", "abd") < 0 && my_strcmp("b", "a") > 0 && my_strcmp("x", "x") == 0);

    /* Embedded '\0': the string "ends" early even though the array is longer. */
    char two[] = "ab\0cd";
    assert(sizeof two == 6 && strlen(two) == 2);

    /* Walking a string with a pointer: count words. */
    const char *s = "  the  quick brown ";
    int words = 0, in = 0;
    for (const char *c = s; *c; c++) {
        if (*c != ' ' && !in) words++;
        in = (*c != ' ');
    }
    assert(words == 3);

    /* Array of strings vs 2-D char array. */
    const char *ptrs[] = {"a", "longer"};      /* 2 pointers, strings elsewhere */
    char grid[2][7] = {"a", "longer"};          /* 14 bytes, strings inside      */
    printf("sizeof ptrs = %zu (2 pointers), sizeof grid = %zu (2x7 chars)\n", sizeof ptrs, sizeof grid);
    assert(strcmp(ptrs[1], grid[1]) == 0);

    printf("All checks passed.\n");
    return 0;
}
