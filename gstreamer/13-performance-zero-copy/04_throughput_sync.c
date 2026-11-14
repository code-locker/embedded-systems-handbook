/*
 * 04_throughput_sync.c — measure two classic performance knobs:
 *                        sink sync=true/false and an avoidable videoconvert
 *
 * Concept : (1) A sink with sync=true WAITS until each buffer's running time
 *           arrives on the clock: throughput = framerate, by design. For
 *           offline processing (transcoding, analytics on files) or for
 *           measuring raw speed, sync=false lets data flow as fast as the CPU
 *           allows. (2) videoconvert in passthrough costs ~nothing, but a real
 *           conversion (I420 -> RGBx) touches every pixel on the CPU — the #1
 *           hidden cost on an embedded board. We time all four cases.
 * Build   : make run P=gstreamer/13-performance-zero-copy/04_throughput_sync.c
 *           make OPT=1 SAN=0 ... for realistic numbers (ASan slows pixel loops)
 * Read    : README.md  (sections "sync=false" and "Avoid videoconvert")
 *
 *   A  videotestsrc(30fps) ─► fakesink sync=true    ~ N/30 s   (clock-paced)
 *   B  videotestsrc(30fps) ─► fakesink sync=false   ~ CPU speed
 *   C  appsrc I420 ─► videoconvert(passthrough I420) ─► fakesink
 *   D  appsrc I420 ─► videoconvert(I420->RGBx: per-pixel math) ─► fakesink
 *      (appsrc pushes the SAME prefilled frame again and again, so the
 *       source costs ~0 and only videoconvert's work is measured)
 */
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
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

/* Run a pipeline to EOS; return wall-clock seconds from PLAYING to EOS. */
static double run_timed(const char *desc)
{
    GError *err = NULL;
    GstElement *p = gst_parse_launch(desc, &err);          /* we own it */
    CHECK(p && !err, "parse failed");
    /* Preroll first so we time steady-state flow, not plugin loading. */
    gst_element_set_state(p, GST_STATE_PAUSED);
    gst_element_get_state(p, NULL, NULL, 5 * GST_SECOND);

    gint64 t0 = g_get_monotonic_time();                    /* µs */
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND,
                          GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    gint64 t1 = g_get_monotonic_time();
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return (double)(t1 - t0) / 1e6;
}

/* Push the same 1280x720 I420 frame `n` times through
 * appsrc ! videoconvert ! <out_caps> ! fakesink; return seconds. */
static double run_convert(const char *out_format, int n)
{
    gchar *desc = g_strdup_printf(
        "appsrc name=src format=time "
        "caps=video/x-raw,format=I420,width=1280,height=720,framerate=30/1 "
        "! videoconvert ! video/x-raw,format=%s ! fakesink sync=false",
        out_format);
    GError *err = NULL;
    GstElement *p = gst_parse_launch(desc, &err);          /* we own it */
    g_free(desc);
    CHECK(p && !err, "parse failed");
    GstElement *src = gst_bin_get_by_name(GST_BIN(p), "src");   /* +1 */
    g_object_set(src, "max-buffers", (guint64)4, "block", TRUE, NULL);

    GstBuffer *frame = gst_buffer_new_and_alloc(1280 * 720 * 3 / 2);
    gst_buffer_memset(frame, 0, 128, 1280 * 720 * 3 / 2);  /* mid grey */

    gst_element_set_state(p, GST_STATE_PLAYING);
    gint64 t0 = g_get_monotonic_time();
    for (int i = 0; i < n; i++)
        /* push_buffer takes a ref: give it an extra one, keep ours.
         * Read-only sharing is safe: nobody writes to it (passthrough
         * forwards it, conversion writes into a NEW output buffer). */
        gst_app_src_push_buffer(GST_APP_SRC(src), gst_buffer_ref(frame));
    gst_app_src_end_of_stream(GST_APP_SRC(src));
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND,
                          GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    gint64 t1 = g_get_monotonic_time();
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_buffer_unref(frame);
    gst_object_unref(src);
    gst_object_unref(p);
    return (double)(t1 - t0) / 1e6;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);
    const char *sink = display ? "autovideosink" : "fakesink";
    gchar *d;

    /* ---- sync=true vs sync=false (15 frames at 30 fps = 0.5 s) ---- */
    d = g_strdup_printf("videotestsrc num-buffers=15 ! "
        "video/x-raw,format=I420,width=160,height=120,framerate=30/1 ! "
        "videoconvert ! %s sync=true", sink);
    double a = run_timed(d);
    g_free(d);
    double b = run_timed("videotestsrc num-buffers=15 ! "
        "video/x-raw,format=I420,width=160,height=120,framerate=30/1 ! "
        "fakesink sync=false");
    printf("A sync=true : 15 frames in %.3f s (%5.0f fps)  <- paced by the clock\n",
           a, 15 / a);
    printf("B sync=false: 15 frames in %.3f s (%5.0f fps)  <- as fast as possible\n",
           b, 15 / b);
    /* First frame renders at running time 0, last at 14/30 s ≈ 0.47 s */
    CHECK(a > 0.4, "sync=true must take ~0.47 s for 15 frames at 30 fps");
    CHECK(b < a / 2, "sync=false must be much faster");

    /* ---- passthrough vs real conversion (20 frames 1280x720) ------ */
    double c = run_convert("I420", 20);
    double e = run_convert("RGBx", 20);
    printf("C videoconvert passthrough I420->I420: %.3f s\n", c);
    printf("D videoconvert convert     I420->RGBx: %.3f s  (%.0fx slower,"
           " %.1f ms per 720p frame)\n", e, e / c, e * 1000 / 20);
    CHECK(e > 2 * c, "a real conversion must cost far more than passthrough");

    printf("OK: sync paces to the clock; conversions cost CPU per pixel\n");
    gst_deinit();
    return 0;
}
