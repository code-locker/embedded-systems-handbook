/*
 * 02_buffer_pool.c — GstBufferPool: allocate once, recycle forever
 *
 * Concept : malloc+free of a 1080p frame 30 times a second wastes CPU and
 *           fragments memory; DMA/contiguous memory (CMA, dmabuf) is even
 *           more expensive to allocate. A buffer pool preallocates N buffers;
 *           when the last ref to a pool buffer is dropped it goes BACK to the
 *           pool, not to free(). Part 1 drives a pool by hand; part 2 proves
 *           a real pipeline recycles the same few memory blocks for 100 frames.
 * Build   : make run P=gstreamer/13-performance-zero-copy/02_buffer_pool.c
 *           ./build/gstreamer/13-performance-zero-copy/02_buffer_pool --display
 * Read    : README.md  (section "How it works: buffer pools and the allocation query")
 *
 *           acquire                       push ──► ... ──► sink
 *   ┌pool──────────────┐ ─────────► buf ───────────────────────┐
 *   │ [buf][buf][buf]  │                                      │ last unref
 *   │  free list       │ ◄────────────────────────────────────┘ (release_buffer)
 *   └──────────────────┘   min..max buffers, size, allocator, options (video meta)
 */
#include <gst/gst.h>
#include <gst/video/video.h>
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

static gpointer first_mem_ptr(GstBuffer *buf)
{
    GstMapInfo m;
    CHECK(gst_buffer_map(buf, &m, GST_MAP_READ), "map");
    gpointer p = m.data;
    gst_buffer_unmap(buf, &m);
    return p;
}

/* ---------------- Part 1: a pool by hand --------------------------- */
static void pool_by_hand(void)
{
    GstCaps *caps = gst_caps_from_string(
        "video/x-raw,format=I420,width=320,height=240,framerate=30/1");
    GstVideoInfo vi;
    CHECK(gst_video_info_from_caps(&vi, caps), "video info");

    /* A GstVideoBufferPool knows video: it can add GstVideoMeta (strides,
     * plane offsets) and alignment/padding. Plain gst_buffer_pool_new()
     * would also work for opaque bytes. */
    GstBufferPool *pool = gst_video_buffer_pool_new();     /* we own it */
    GstStructure *cfg = gst_buffer_pool_get_config(pool);  /* we own a copy */
    /* caps, size per buffer, min buffers, max buffers (0 = unlimited) */
    gst_buffer_pool_config_set_params(cfg, caps, (guint)GST_VIDEO_INFO_SIZE(&vi),
                                      2, 3);
    gst_buffer_pool_config_add_option(cfg, GST_BUFFER_POOL_OPTION_VIDEO_META);
    CHECK(gst_buffer_pool_set_config(pool, cfg), "set_config"); /* takes cfg */
    CHECK(gst_buffer_pool_set_active(pool, TRUE), "activate");   /* allocates min */

    GstBuffer *b1 = NULL, *b2 = NULL, *b3 = NULL, *b4 = NULL;
    CHECK(gst_buffer_pool_acquire_buffer(pool, &b1, NULL) == GST_FLOW_OK, "b1");
    gpointer p1 = first_mem_ptr(b1);
    printf("pool: acquired b1 (%zu bytes, video meta: %s)\n",
           gst_buffer_get_size(b1),
           gst_buffer_get_video_meta(b1) ? "yes" : "no");
    CHECK(gst_buffer_get_size(b1) == GST_VIDEO_INFO_SIZE(&vi), "size");
    CHECK(gst_buffer_get_video_meta(b1) != NULL, "video meta option honoured");

    /* Dropping the last ref returns it to the pool (b1 is now invalid). */
    gst_buffer_unref(b1);
    CHECK(gst_buffer_pool_acquire_buffer(pool, &b2, NULL) == GST_FLOW_OK, "b2");
    CHECK(gst_buffer_pool_acquire_buffer(pool, &b3, NULL) == GST_FLOW_OK, "b3");
    gpointer p2 = first_mem_ptr(b2), p3 = first_mem_ptr(b3);
    printf("pool: after release, re-acquired memory %p == first %p ? %s\n",
           p2, p1, (p2 == p1 || p3 == p1) ? "yes (recycled)" : "no");
    CHECK(p2 == p1 || p3 == p1, "released buffer must be reused");

    /* max=3: one more is allowed, then the pool is exhausted. A real
     * element would BLOCK here until a sink releases one: that is
     * back-pressure (and the source of "pipeline frozen" bugs when a
     * sink holds every buffer). DONTWAIT turns the block into EOS. */
    CHECK(gst_buffer_pool_acquire_buffer(pool, &b4, NULL) == GST_FLOW_OK, "b4");
    GstBufferPoolAcquireParams params = { .flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT };
    GstBuffer *b5 = NULL;
    GstFlowReturn fr = gst_buffer_pool_acquire_buffer(pool, &b5, &params);
    printf("pool: 4th acquire with max=3 and DONTWAIT -> %s\n",
           gst_flow_get_name(fr));
    CHECK(fr == GST_FLOW_EOS && b5 == NULL, "exhausted pool must refuse");

    gst_buffer_unref(b2);
    gst_buffer_unref(b3);
    gst_buffer_unref(b4);
    gst_buffer_pool_set_active(pool, FALSE);   /* frees the buffers */
    gst_object_unref(pool);
    gst_caps_unref(caps);
}

/* ---------------- Part 2: a pipeline recycles ---------------------- */
static GHashTable *seen;        /* distinct data pointers seen at the sink */
static gint frames;

/* Buffer probe on fakesink's sink pad (streaming thread). */
static GstPadProbeReturn on_buffer(GstPad *pad, GstPadProbeInfo *info,
                                   gpointer user_data)
{
    (void)pad; (void)user_data;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);   /* transfer none */
    g_hash_table_add(seen, first_mem_ptr(buf));         /* only this thread */
    frames++;
    return GST_PAD_PROBE_OK;
}

static void pipeline_recycles(gboolean display)
{
    const char *desc = display
        ? "videotestsrc num-buffers=100 ! video/x-raw,format=I420,width=640,height=480 "
          "! videoconvert ! autovideosink name=sink"
        : "videotestsrc num-buffers=100 ! video/x-raw,format=I420,width=640,height=480 "
          "! fakesink name=sink sync=false";
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);   /* we own it */
    CHECK(pipeline && !err, "parse failed");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink"); /* +1 */
    GstPad *pad = gst_element_get_static_pad(sink, "sink");            /* +1 */
    seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_buffer, NULL, NULL);
    gst_object_unref(pad);
    gst_object_unref(sink);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND,
                          GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    guint distinct = g_hash_table_size(seen);
    printf("pipeline: %d I420 640x480 frames (450 KiB each) used only %u distinct memory "
           "blocks (the source's pool)\n", frames, distinct);
    CHECK(frames == 100, "all frames arrived");
    CHECK(distinct < 10, "a pool must recycle a handful of buffers");
    g_hash_table_destroy(seen);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    pool_by_hand();
    pipeline_recycles(display);

    printf("OK: pool buffers are recycled, never freed per frame\n");
    gst_deinit();
    return 0;
}
