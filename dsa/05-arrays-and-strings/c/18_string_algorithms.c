/*
 * 18_string_algorithms.c — palindrome, anagram, reverse words, KMP search
 *
 * Concept : Four classic string problems and the technique behind each:
 *             palindrome    -> two pointers moving inwards, O(n), O(1) space
 *             anagram       -> counting array of 256 bytes, O(n), O(1) space
 *             reverse words -> reverse all, then reverse each word, in place
 *             KMP search    -> precompute how far the pattern can "fall back"
 *                              on a mismatch, so the text pointer NEVER moves
 *                              backwards: O(n + m) instead of O(n * m).
 * Build   : make run P=dsa/05-arrays-and-strings/c/18_string_algorithms.c
 * Read    : ../notes/strings.md  (section "String algorithms")
 */
#include <ctype.h>
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

/* ------------------------------------------------------------ palindrome --
 * Two pointers, skipping anything that isn't a letter/digit, comparing
 * case-insensitively. O(n) time, O(1) space.
 *   "A man, a plan, a canal: Panama"
 *    i->                         <-j    compare, move both inwards
 * isalnum/tolower need an unsigned char value (negative char = UB).     */
static bool is_palindrome(const char *s)
{
    size_t i = 0, j = strlen(s);
    while (i < j) {
        unsigned char a = (unsigned char)s[i], b = (unsigned char)s[j - 1];
        if (!isalnum(a)) { i++; continue; }
        if (!isalnum(b)) { j--; continue; }
        if (tolower(a) != tolower(b))
            return false;
        i++;
        j--;
    }
    return true;
}

/* -------------------------------------------------------------- anagram --
 * Same multiset of bytes? Count up for a, down for b; all zero => anagram.
 * O(n + 256) time, O(256) = O(1) space. (Sorting both is O(n log n).)    */
static bool is_anagram(const char *a, const char *b)
{
    int count[256] = {0};
    size_t na = strlen(a), nb = strlen(b);
    if (na != nb)
        return false;
    for (size_t i = 0; i < na; i++) {
        count[(unsigned char)a[i]]++;
        count[(unsigned char)b[i]]--;
    }
    for (int c = 0; c < 256; c++)
        if (count[c] != 0)
            return false;
    return true;
}

/* -------------------------------------------------------- reverse words --
 *   "the quick fox"
 *   reverse whole   : "xof kciuq eht"
 *   reverse each word: "fox quick the"
 * O(n) time, O(1) space — the same trick rotates an array in place.     */
static void reverse_range(char *a, size_t i, size_t j) /* [i, j) */
{
    while (i + 1 < j) {
        char t = a[i];
        a[i++] = a[j - 1];
        a[--j] = t;
    }
}

static void reverse_words(char *s)
{
    size_t n = strlen(s);
    reverse_range(s, 0, n);
    for (size_t i = 0; i < n;) {
        while (i < n && s[i] == ' ')
            i++;
        size_t start = i;
        while (i < n && s[i] != ' ')
            i++;
        reverse_range(s, start, i);
    }
}

/* ------------------------------------------------------------------ KMP --
 * fail[i] = length of the longest PROPER prefix of pat[0..i] that is also
 * a suffix of pat[0..i]. ("Proper" = not the whole thing.)
 *
 *   pat :   a  b  a  b  c  a  b  a  b  d
 *   i   :   0  1  2  3  4  5  6  7  8  9
 *   fail:   0  0  1  2  0  1  2  3  4  0
 *                     ^              ^
 *   i=3: "abab" -> "ab" is both prefix and suffix -> 2
 *   i=8: "ababcabab" -> "abab" -> 4
 *
 * Why it helps: we matched pat[0..j) against the text and then mismatched.
 * The last fail[j-1] characters of what we matched are ALSO the first
 * fail[j-1] characters of pat, so we can continue as if we had matched
 * only those — no need to move the text pointer back.
 *
 *   text: a b a b a b c a b a b d
 *   pat : a b a b c              mismatch at j=4 ('c' vs 'a')
 *               a b a b c        shift: j = fail[3] = 2, keep text index
 *
 * Building fail is the same algorithm run on the pattern against itself.
 * Both loops: the "while" fallbacks are paid for by earlier increments of
 * j, so total work is O(m) to build + O(n) to search.                 */
static void kmp_build(const char *pat, size_t m, size_t *fail)
{
    if (m == 0)
        return;
    fail[0] = 0;
    size_t k = 0; /* length of current matched prefix */
    for (size_t i = 1; i < m; i++) {
        while (k > 0 && pat[i] != pat[k])
            k = fail[k - 1]; /* fall back to a shorter border */
        if (pat[i] == pat[k])
            k++;
        fail[i] = k;
    }
}

/* Stores up to max match positions in out[], returns total matches.
 * Overlapping matches are reported ("aa" in "aaaa" -> 0, 1, 2).        */
static size_t kmp_search(const char *text, const char *pat, size_t *out,
                         size_t max)
{
    size_t n = strlen(text), m = strlen(pat), found = 0;
    if (m == 0 || m > n)
        return 0;
    size_t *fail = malloc(m * sizeof *fail); /* we own it, freed below */
    CHECK(fail);
    kmp_build(pat, m, fail);
    size_t j = 0; /* chars of pat matched so far */
    for (size_t i = 0; i < n; i++) { /* i only ever moves forward */
        while (j > 0 && text[i] != pat[j])
            j = fail[j - 1];
        if (text[i] == pat[j])
            j++;
        if (j == m) {
            if (found < max)
                out[found] = i + 1 - m;
            found++;
            j = fail[j - 1]; /* keep going: allow overlaps */
        }
    }
    free(fail);
    return found;
}

/* Naive search for cross-checking. O(n*m) worst case. */
static size_t naive_search(const char *text, const char *pat, size_t *out,
                           size_t max)
{
    size_t n = strlen(text), m = strlen(pat), found = 0;
    if (m == 0 || m > n)
        return 0;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(text + i, pat, m) == 0) {
            if (found < max)
                out[found] = i;
            found++;
        }
    return found;
}

static uint32_t rng = 12345;
static uint32_t next_rand(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 16;
}

int main(void)
{
    puts("== palindrome (two pointers) ==");
    const char *pals[] = {"racecar", "A man, a plan, a canal: Panama", "", "x",
                          "No 'x' in Nixon"};
    for (size_t i = 0; i < sizeof pals / sizeof pals[0]; i++) {
        printf("  yes: \"%s\"\n", pals[i]);
        CHECK(is_palindrome(pals[i]));
    }
    CHECK(!is_palindrome("beaglebone"));
    CHECK(!is_palindrome("ab"));

    puts("== anagram (counting) ==");
    CHECK(is_anagram("listen", "silent"));
    CHECK(is_anagram("dormitory", "dirtyroom"));
    CHECK(!is_anagram("rat", "car"));
    CHECK(!is_anagram("aab", "abb"));
    puts("  listen/silent, dormitory/dirtyroom: yes; rat/car: no");

    puts("== reverse words in place ==");
    char sentence[] = "boot rom spl uboot linux";
    reverse_words(sentence);
    printf("  \"%s\"\n", sentence);
    CHECK(strcmp(sentence, "linux uboot spl rom boot") == 0);

    puts("== KMP failure function ==");
    const char *pat = "ababcababd";
    size_t fail[10];
    kmp_build(pat, 10, fail);
    size_t expect_fail[10] = {0, 0, 1, 2, 0, 1, 2, 3, 4, 0};
    printf("  pat : ");
    for (int i = 0; i < 10; i++)
        printf("%2c", pat[i]);
    printf("\n  fail: ");
    for (int i = 0; i < 10; i++)
        printf("%2zu", fail[i]);
    putchar('\n');
    CHECK(memcmp(fail, expect_fail, sizeof fail) == 0);

    puts("== KMP search ==");
    size_t pos[16];
    size_t k = kmp_search("abababcababd ababcababd", pat, pos, 16);
    printf("  found %zu match(es), first at %zu\n", k, pos[0]);
    CHECK(k == 2 && pos[0] == 2 && pos[1] == 13);
    k = kmp_search("aaaa", "aa", pos, 16); /* overlapping */
    CHECK(k == 3 && pos[0] == 0 && pos[1] == 1 && pos[2] == 2);

    puts("== KMP vs naive on 3000 random small-alphabet texts ==");
    /* alphabet {a,b} maximises partial matches: the hard case for naive */
    for (int t = 0; t < 3000; t++) {
        char text[64], p[8];
        size_t n = next_rand() % 63, m = 1 + next_rand() % 6;
        for (size_t i = 0; i < n; i++)
            text[i] = (char)('a' + next_rand() % 2);
        text[n] = '\0';
        for (size_t i = 0; i < m; i++)
            p[i] = (char)('a' + next_rand() % 2);
        p[m] = '\0';
        size_t a[64], b[64];
        size_t ka = kmp_search(text, p, a, 64), kb = naive_search(text, p, b, 64);
        CHECK(ka == kb);
        CHECK(memcmp(a, b, ka * sizeof a[0]) == 0);
    }
    puts("  all agree");

    puts("\nall checks passed");
    return 0;
}
