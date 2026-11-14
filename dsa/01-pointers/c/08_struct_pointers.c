/*
 * 08_struct_pointers.c — p->field, offsetof, and the kernel's container_of
 *
 * Concept : p->f is exactly (*p).f: go to the struct, then to the field at
 *           its fixed byte offset. Because offsets are fixed, you can also go
 *           BACKWARDS: from a pointer to a member, subtract offsetof() to get
 *           the enclosing struct. That is container_of(), the trick behind
 *           every intrusive list in the Linux kernel.
 * Build   : make run P=dsa/01-pointers/c/08_struct_pointers.c
 * Read    : ../README.md  (section "3.8 Pointers to structs")
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct list_head { struct list_head *next, *prev; };

struct camera_frame {
    uint32_t seq;               /* offset 0  */
    uint16_t width, height;     /* offset 4, 6 */
    struct list_head node;      /* offset 8  — the "hook" that links frames */
    uint64_t timestamp_ns;      /* offset 24 */
};

/* container_of: from &obj->member back to &obj. Kernel version adds type checks. */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

static void list_init(struct list_head *h) { h->next = h->prev = h; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{
    n->prev = h->prev;
    n->next = h;
    h->prev->next = n;
    h->prev = n;
}

/* Pass big structs by pointer (8 bytes) instead of by value (a full copy). */
static uint32_t area(const struct camera_frame *f)
{
    return (uint32_t)f->width * f->height;   /* f->width == (*f).width */
}

int main(void)
{
    struct camera_frame fr = {.seq = 7, .width = 1920, .height = 1080, .timestamp_ns = 123};
    struct camera_frame *p = &fr;

    /*
     *  p ──► +0  +--------------+ seq = 7
     *        +4  | w=1920|h=1080|
     *        +8  | node.next    |  <── &p->node   (a struct list_head*)
     *        +16 | node.prev    |
     *        +24 | timestamp_ns |
     *            +--------------+   sizeof = 32
     *  container_of(&p->node, struct camera_frame, node)
     *      = (char*)&p->node - 8  = p
     */
    printf("offsetof: seq=%zu width=%zu height=%zu node=%zu timestamp=%zu, sizeof=%zu\n",
           offsetof(struct camera_frame, seq), offsetof(struct camera_frame, width),
           offsetof(struct camera_frame, height), offsetof(struct camera_frame, node),
           offsetof(struct camera_frame, timestamp_ns), sizeof(struct camera_frame));

    assert(p->seq == (*p).seq);
    assert(area(p) == 1920u * 1080u);
    p->seq++;                         /* modifies fr: there is one object */
    assert(fr.seq == 8);

    struct list_head *hook = &p->node;
    struct camera_frame *back = container_of(hook, struct camera_frame, node);
    assert(back == p);
    printf("&fr = %p, &fr.node = %p, container_of(...) = %p\n", (void *)p, (void *)hook, (void *)back);

    /* An intrusive list of frames: the list code knows nothing about frames. */
    struct list_head queue;
    list_init(&queue);
    struct camera_frame frames[3] = {{.seq = 10}, {.seq = 11}, {.seq = 12}};
    for (int i = 0; i < 3; i++)
        list_add_tail(&frames[i].node, &queue);

    uint32_t expect = 10;
    for (struct list_head *it = queue.next; it != &queue; it = it->next) {
        struct camera_frame *f = container_of(it, struct camera_frame, node);
        printf("  queued frame seq=%u\n", f->seq);
        assert(f->seq == expect++);
    }

    /* Array of structs + pointer arithmetic: p+1 jumps sizeof(struct). */
    struct camera_frame *fp = frames;
    assert((fp + 2)->seq == 12 && fp[1].seq == 11);

    /* Chained arrows: p->a->b. Each -> is one memory load. */
    struct list_head *second = queue.next->next;
    assert(container_of(second, struct camera_frame, node)->seq == 11);

    printf("All checks passed.\n");
    return 0;
}
