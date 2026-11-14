/*
 * 01_buffer_memory_anatomy.c — what a GstBuffer really is: a refcounted
 *                              header pointing at refcounted GstMemory blocks
 *
 * Concept : A GstBuffer holds timestamps/flags/metas plus a list of up to 16
 *           GstMemory. Copying a buffer copies the HEADER and shares the
 *           memory (cheap). Writing needs writability: make_writable() copies
 *           the header if someone else holds it; mapping a SHARED memory for
 *           WRITE copies the bytes (copy-on-write). Knowing exactly when a
 *           copy happens is the whole zero-copy game.
 * Build   : make run P=gstreamer/13-performance-zero-copy/01_buffer_memory_anatomy.c
 * Read    : README.md  (section "How it works: buffer anatomy")
 *
 *     GstBuffer A (ref 1)            GstBuffer B = gst_buffer_copy(A)
 *     pts, dts, duration, flags      pts, dts, ... (copied fields)
 *     mem[0] ─────────┐   ┌───────── mem[0]
 *     mem[1] ──────┐  │   │  ┌────── mem[1]
 *                  ▼  ▼   ▼  ▼
 *            GstMemory m1 (ref 2)   GstMemory m0 (ref 2)
 *            bytes 'B'...           bytes 'A'...     <- ONE copy of the pixels
 */
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", msg, __FILE__,   \
                    __LINE__);                                             \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

/* Data pointer of memory #idx without copying (map READ of one memory). */
static guint8 *mem_data(GstBuffer *buf, guint idx)
{
    GstMemory *m = gst_buffer_peek_memory(buf, idx);   /* transfer none */
    GstMapInfo info;
    CHECK(gst_memory_map(m, &info, GST_MAP_READ), "map memory");
    guint8 *p = info.data;
    gst_memory_unmap(m, &info);
    return p;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    (void)argc; (void)argv;     /* no --display: nothing to show here */

    /* ---- 1. Build a buffer from two memory blocks ------------------- */
    GstBuffer *a = gst_buffer_new();                       /* we own it */
    GstMemory *m0 = gst_allocator_alloc(NULL, 1000, NULL); /* default sysmem */
    GstMemory *m1 = gst_allocator_alloc(NULL, 500, NULL);
    gst_buffer_append_memory(a, m0);  /* transfer FULL: buffer now owns m0 */
    gst_buffer_append_memory(a, m1);
    gst_buffer_memset(a, 0, 'A', 1000);
    gst_buffer_memset(a, 1000, 'B', 500);
    GST_BUFFER_PTS(a) = 40 * GST_MSECOND;

    printf("buffer A: %u memories, total %zu bytes\n",
           gst_buffer_n_memory(a), gst_buffer_get_size(a));
    CHECK(gst_buffer_n_memory(a) == 2, "two memories");
    CHECK(gst_buffer_get_size(a) == 1500, "size is the sum");

    /* ---- 2. Mapping a multi-memory buffer MERGES (= copies!) -------- */
    GstMapInfo map;
    guint8 *before = mem_data(a, 0);          /* where block #0 lives now */
    CHECK(gst_buffer_map(a, &map, GST_MAP_READ), "map buffer");
    gboolean merged_is_copy = (map.data != before);
    printf("map of whole buffer: data %s old memory #0 -> %s\n",
           merged_is_copy ? "!=" : "==",
           merged_is_copy ? "1500 bytes were COPIED into a new block" : "no copy");
    CHECK(merged_is_copy, "mapping 2 memories as one must merge (copy)");
    CHECK(map.data[999] == 'A' && map.data[1000] == 'B', "merged content");
    gst_buffer_unmap(a, &map);
    /* The merged block REPLACES the two old ones (a READ map is allowed to
     * do this), so the next map is free. Lesson: a buffer split over N
     * memories costs a copy the first time someone maps it whole. Video
     * code maps per plane with gst_video_frame_map() to avoid that. */
    printf("after map: buffer A now has %u memory block(s)\n",
           gst_buffer_n_memory(a));
    CHECK(gst_buffer_n_memory(a) == 1, "merged block replaced the two");

    /* ---- 3. gst_buffer_copy shares the memory ----------------------- */
    guint8 *a_ptr = mem_data(a, 0);
    GstBuffer *b = gst_buffer_copy(a);           /* new header, we own it */
    guint8 *b_ptr = mem_data(b, 0);
    GstMemory *shared = gst_buffer_peek_memory(a, 0);
    printf("copy B: same bytes pointer? %s, memory refcount=%d, "
           "pts copied=%" GST_TIME_FORMAT "\n",
           a_ptr == b_ptr ? "yes" : "no",
           GST_MINI_OBJECT_REFCOUNT_VALUE(shared),
           GST_TIME_ARGS(GST_BUFFER_PTS(b)));
    CHECK(a_ptr == b_ptr, "gst_buffer_copy must not copy the bytes");
    CHECK(GST_MINI_OBJECT_REFCOUNT_VALUE(shared) == 2, "memory shared by 2");
    CHECK(GST_BUFFER_PTS(b) == GST_BUFFER_PTS(a), "metadata copied");

    /* ---- 4. Copy-on-write when writing shared memory --------------- */
    CHECK(gst_buffer_is_writable(b), "B header has refcount 1 -> writable");
    CHECK(!gst_memory_is_writable(shared) ||
          GST_MINI_OBJECT_REFCOUNT_VALUE(shared) > 1,
          "memory is shared");
    CHECK(gst_buffer_map(b, &map, GST_MAP_WRITE), "map B for write");
    gboolean cow = (map.data != a_ptr);
    map.data[0] = 'Z';
    gst_buffer_unmap(b, &map);
    printf("map B WRITE: %s; A[0]='%c' B[0]='%c'\n",
           cow ? "memory was COPIED first (copy-on-write)" : "in place",
           mem_data(a, 0)[0], mem_data(b, 0)[0]);
    CHECK(cow, "writing shared memory must copy it");
    CHECK(mem_data(a, 0)[0] == 'A', "A untouched");
    CHECK(mem_data(b, 0)[0] == 'Z', "B changed");

    /* ---- 5. make_writable: ref'd twice -> copies header ----------- */
    GstBuffer *c = gst_buffer_ref(a);     /* now A has refcount 2 */
    CHECK(!gst_buffer_is_writable(c), "refcount 2 -> not writable");
    c = gst_buffer_make_writable(c);      /* consumes our ref, returns copy */
    CHECK(c != a, "make_writable returned a new header");
    CHECK(GST_MINI_OBJECT_REFCOUNT_VALUE(a) == 1, "A back to 1 ref");
    printf("make_writable on a shared buffer: new header %p (A=%p), "
           "bytes still shared: %s\n", (void *)c, (void *)a,
           mem_data(c, 0) == a_ptr ? "yes" : "no");
    CHECK(mem_data(c, 0) == a_ptr, "header copy, memory still shared");

    /* ---- 6. Sub-buffer / region: zero-copy slice ------------------- */
    GstBuffer *slice = gst_buffer_copy_region(a, GST_BUFFER_COPY_MEMORY,
                                              100, 200);
    CHECK(gst_buffer_get_size(slice) == 200, "slice size");
    CHECK(mem_data(slice, 0) == a_ptr + 100,
          "region shares memory with an offset (no bytes copied)");
    printf("copy_region(100, 200): points at A+100, no copy\n");

    gst_buffer_unref(slice);
    gst_buffer_unref(c);
    gst_buffer_unref(b);
    gst_buffer_unref(a);     /* last ref: memories freed with it */

    printf("OK: copy shares, write-map copies, make_writable copies header\n");
    gst_deinit();
    return 0;
}
