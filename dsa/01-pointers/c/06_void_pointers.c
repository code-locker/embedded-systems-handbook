/*
 * 06_void_pointers.c — void*: an address with no type, the basis of generic C
 *
 * Concept : void* can hold the address of any object, but you cannot
 *           dereference it or do arithmetic on it: there is no element size.
 *           Generic C code (memcpy, qsort, malloc, a GStreamer "user_data")
 *           passes void* plus a SIZE, and works on bytes via unsigned char*.
 * Build   : make run P=dsa/01-pointers/c/06_void_pointers.c
 * Read    : ../README.md  (section "3.6 void*")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Generic swap: works for ANY type because the caller tells us the size.
 * O(size) bytes copied. Uses a small stack buffer, falls back to byte-wise. */
static void swap_any(void *a, void *b, size_t size)
{
    unsigned char *pa = a, *pb = b;     /* implicit void* -> T* is fine in C */
    for (size_t i = 0; i < size; i++) {
        unsigned char t = pa[i];
        pa[i] = pb[i];
        pb[i] = t;
    }
}

/* Generic linear search: returns pointer to the element or NULL. */
static void *find_any(const void *base, size_t n, size_t size, const void *key)
{
    const unsigned char *p = base;
    for (size_t i = 0; i < n; i++, p += size)          /* step by element size by hand */
        if (memcmp(p, key, size) == 0)
            return (void *)p;                           /* like bsearch: drops const */
    return NULL;
}

/* Callback with user data: the C way to pass context to generic code. */
typedef void (*visit_fn)(void *elem, void *user);

static void for_each(void *base, size_t n, size_t size, visit_fn fn, void *user)
{
    unsigned char *p = base;
    for (size_t i = 0; i < n; i++)
        fn(p + i * size, user);
}

static void add_to_total(void *elem, void *user)
{
    *(long *)user += *(int *)elem;   /* we KNOW the real types; the compiler does not */
}

struct sample { short id; double value; };

int main(void)
{
    int x = 1, y = 2;
    swap_any(&x, &y, sizeof x);
    assert(x == 2 && y == 1);

    struct sample s1 = {1, 1.5}, s2 = {2, 2.5};
    swap_any(&s1, &s2, sizeof s1);
    assert(s1.id == 2 && s2.value == 1.5);

    double vals[] = {1.0, 2.0, 3.0};
    double key = 2.0;
    double *hit = find_any(vals, 3, sizeof vals[0], &key);
    assert(hit == &vals[1]);

    int data[] = {1, 2, 3, 4};
    long total = 0;
    for_each(data, 4, sizeof data[0], add_to_total, &total);
    assert(total == 10);

    /* void* holds any object address; converting back to the ORIGINAL type is safe. */
    void *v = &x;
    int *back = v;
    assert(*back == 2);
    /* *v;      error: dereferencing 'void *' pointer      */
    /* v + 1;   GNU extension (steps 1 byte); -Wpedantic warns */

    /* malloc returns void*: no cast needed in C (needed in C++). */
    int *heap = malloc(4 * sizeof *heap);   /* sizeof *heap: stays right if the type changes */
    assert(heap);
    memset(heap, 0, 4 * sizeof *heap);
    assert(heap[3] == 0);
    free(heap);

    printf("swap_any, find_any, for_each with void*: all checks passed.\n");
    return 0;
}
