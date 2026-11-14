/*
 * 03_zero_copy_wrap.c — hand YOUR memory to GStreamer without copying it
 *
 * Concept : gst_buffer_new_wrapped_full() makes a GstBuffer whose GstMemory
 *           points at bytes you own (a camera ring buffer, a DMA region, an
 *           mmap'd file, an OpenCV Mat). No memcpy. GStreamer calls your
 *           GDestroyNotify when the LAST reference is gone — only then may you
 *           reuse or free the bytes. We push 10 wrapped frames through
 *           appsrc ! appsink and prove the sink sees the very same pointers,
 *           and that every notify fired.
 * Build   : make run P=gstreamer/13-performance-zero-copy/03_zero_copy_wrap.c
 *           ./build/gstreamer/13-performance-zero-copy/03_zero_copy_wrap --display
 * Read    : README.md  (section "How it works: wrapping external memory")
 *
 *   your frames[i] (static array, you own it)
 *        │ gst_buffer_new_wrapped_full(..., frames[i], ..., &slot[i], on_free)
 *        ▼
 *   appsrc ───────────────────────────────► appsink ─► pull_sample: map.data == frames[i]
 *        (same bytes all the way; no element needed to write them)
 *   last unref anywhere ──► on_free(&slot[i])  => "slot i may be reused"
 */
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
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

#define W 64
#define H 48
#define N 10
#define FRAME_SIZE (W * H)          /* GRAY8: 1 byte per pixel, W%4==0 so
                                       the stride is exactly W */

/* "Your" memory: e.g. a driver ring buffer. Static so it outlives the
 * pipeline no matter what. In real code: allocate, and free it in
 * on_free (or mark the slot reusable). */
static guint8 frames[N][FRAME_SIZE];
static gint freed[N];              /* set by on_free, any thread */

static void on_free(gpointer user_data)
{
    gint *slot = user_data;
    g_atomic_int_set(slot, 1);     /* "this slot is free again" */
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* appsink in both cases: we need the pointers. With --display a tee
     * also shows the frames (that branch scales/converts => copies). */
    const char *desc = display
        ? "appsrc name=src format=time "
          "caps=video/x-raw,format=GRAY8,width=64,height=48,framerate=5/1 "
          "! tee name=t t. ! queue ! appsink name=sink sync=false "
          "t. ! queue ! videoconvert ! videoscale ! autovideosink"
        : "appsrc name=src format=time "
          "caps=video/x-raw,format=GRAY8,width=64,height=48,framerate=5/1 "
          "! appsink name=sink sync=false";
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);   /* we own it */
    CHECK(pipeline && !err, "parse failed");
    GstElement *src  = gst_bin_get_by_name(GST_BIN(pipeline), "src");  /* +1 */
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink"); /* +1 */

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    for (int i = 0; i < N; i++) {
        memset(frames[i], 16 + i * 20, FRAME_SIZE);     /* recognisable */
        /* flags 0 = writable memory; maxsize, offset, size; user_data and
         * notify are what make it zero-copy AND safe. */
        GstBuffer *buf = gst_buffer_new_wrapped_full(
            0, frames[i], FRAME_SIZE, 0, FRAME_SIZE, &freed[i], on_free);
        GST_BUFFER_PTS(buf) = gst_util_uint64_scale(i, GST_SECOND, 5);
        GST_BUFFER_DURATION(buf) = GST_SECOND / 5;
        /* push_buffer takes OUR reference (transfer full). */
        CHECK(gst_app_src_push_buffer(GST_APP_SRC(src), buf) == GST_FLOW_OK,
              "push");
    }
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    int got = 0, same_ptr = 0;
    for (;;) {
        GstSample *s = gst_app_sink_pull_sample(GST_APP_SINK(sink)); /* +1 */
        if (!s) break;                                    /* EOS */
        GstBuffer *b = gst_sample_get_buffer(s);          /* transfer none */
        GstMapInfo m;
        CHECK(gst_buffer_map(b, &m, GST_MAP_READ), "map");
        if (m.data == frames[got]) same_ptr++;
        CHECK(m.data[0] == (guint8)(16 + got * 20), "content");
        gst_buffer_unmap(b, &m);
        gst_sample_unref(s);   /* drops the sample's ref; with no tee this
                                  is the last one -> on_free runs now */
        got++;
    }
    printf("received %d frames, %d with the exact pointer we pushed\n",
           got, same_ptr);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(pipeline);

    int notified = 0;
    for (int i = 0; i < N; i++) notified += g_atomic_int_get(&freed[i]);
    printf("destroy-notify fired for %d/%d frames\n", notified, N);

    CHECK(got == N, "all frames arrived");
    CHECK(same_ptr == N, "zero-copy: appsink must see our own memory");
    CHECK(notified == N, "every wrapped buffer must be released");

    printf("OK: zero copies from your memory to the sink\n");
    gst_deinit();
    return 0;
}
