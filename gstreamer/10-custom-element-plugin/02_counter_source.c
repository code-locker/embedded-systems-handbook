/*
 * 02_counter_source.c — write your own SOURCE element on GstPushSrc
 *
 * Concept : a source has no sink pad: it invents data. GstPushSrc (on top
 *           of GstBaseSrc) gives you the streaming thread, num-buffers, EOS,
 *           seeking skeleton and state handling; you implement:
 *             fixate()   pick concrete caps from a range (default 64x48@30)
 *             set_caps() remember what was agreed
 *             create()   allocate + fill + TIMESTAMP one buffer
 *           Each frame is filled with (frame_number % 256), so the app can
 *           prove ordering, and PTS = n * frame_duration.
 * Build   : make run P=gstreamer/10-custom-element-plugin/02_counter_source.c
 *           ./build/gstreamer/10-custom-element-plugin/02_counter_source --display
 * Read    : README.md  (section "Sources: GstBaseSrc / GstPushSrc")
 *
 *   mycounter ──► [capsfilter?] ──► appsink
 *     │
 *     └─ src pad template: GRAY8, width [16,1920], height [16,1080],
 *                          framerate [1/1, 120/1]   (RANGES, not values)
 *
 *   Negotiation:  template ∩ downstream = still a range
 *                 └─► fixate(): choose nearest to 64x48@30  ─► set_caps()
 */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/base/gstpushsrc.h>
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

/* ======================================================================
 *  The element
 * ====================================================================== */
#define MY_TYPE_COUNTER (my_counter_get_type())
G_DECLARE_FINAL_TYPE(MyCounter, my_counter, MY, COUNTER, GstPushSrc)

struct _MyCounter {
    GstPushSrc parent;
    GstVideoInfo info;   /* agreed format, from set_caps          */
    guint64 n;           /* next frame number                     */
};

G_DEFINE_TYPE(MyCounter, my_counter, GST_TYPE_PUSH_SRC)

static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format=GRAY8, "
                    "width=[16,1920], height=[16,1080], "
                    "framerate=[1/1,120/1]"));

/* fixate: caps arriving here may still contain ranges/lists. We must
 * return FIXED caps (one value per field). Nearest-to-default lets a
 * downstream capsfilter (width=32) win, but picks 64 when nobody cares. */
static GstCaps *my_counter_fixate(GstBaseSrc *src, GstCaps *caps)
{
    caps = gst_caps_make_writable(caps); /* takes ownership of caps */
    caps = gst_caps_truncate(caps);      /* keep only first structure */
    GstStructure *s = gst_caps_get_structure(caps, 0); /* borrowed */
    gst_structure_fixate_field_nearest_int(s, "width", 64);
    gst_structure_fixate_field_nearest_int(s, "height", 48);
    gst_structure_fixate_field_nearest_fraction(s, "framerate", 30, 1);
    /* parent fixates any remaining fields (none here) */
    return GST_BASE_SRC_CLASS(my_counter_parent_class)->fixate(src, caps);
}

static gboolean my_counter_set_caps(GstBaseSrc *src, GstCaps *caps)
{
    MyCounter *self = MY_COUNTER(src);
    return gst_video_info_from_caps(&self->info, caps);
}

/* start: called on READY->PAUSED. Reset per-stream state here, NOT in
 * init(), so the element can be stopped and restarted. */
static gboolean my_counter_start(GstBaseSrc *src)
{
    MY_COUNTER(src)->n = 0;
    return TRUE;
}

/* create: called in a loop by the streaming thread. We return a NEW
 * buffer; ownership passes to GstBaseSrc which pushes it downstream. */
static GstFlowReturn my_counter_create(GstPushSrc *src, GstBuffer **out)
{
    MyCounter *self = MY_COUNTER(src);
    const GstVideoInfo *vi = &self->info;

    GstBuffer *buf = gst_buffer_new_allocate(NULL, GST_VIDEO_INFO_SIZE(vi),
                                             NULL);
    if (!buf)
        return GST_FLOW_ERROR;
    gst_buffer_memset(buf, 0, (guint8)(self->n % 256),
                      GST_VIDEO_INFO_SIZE(vi));

    /* Timestamps: without them sinks cannot sync and muxers break.
     * PTS(n) = n * 1s * fps_d / fps_n, computed with gst_util_uint64_scale
     * to avoid overflow and rounding drift (never accumulate += duration). */
    const gint fn = GST_VIDEO_INFO_FPS_N(vi), fd = GST_VIDEO_INFO_FPS_D(vi);
    GST_BUFFER_PTS(buf) = gst_util_uint64_scale(self->n, fd * GST_SECOND, fn);
    GST_BUFFER_DURATION(buf) =
        gst_util_uint64_scale(self->n + 1, fd * GST_SECOND, fn) -
        GST_BUFFER_PTS(buf);
    GST_BUFFER_OFFSET(buf) = self->n;   /* frame number, for video */
    self->n++;

    *out = buf;
    return GST_FLOW_OK;
}

static void my_counter_class_init(MyCounterClass *klass)
{
    GstElementClass *ec = GST_ELEMENT_CLASS(klass);
    GstBaseSrcClass *bsc = GST_BASE_SRC_CLASS(klass);
    GstPushSrcClass *psc = GST_PUSH_SRC_CLASS(klass);

    gst_element_class_add_static_pad_template(ec, &src_tmpl);
    gst_element_class_set_static_metadata(
        ec, "Frame counter source", "Source/Video",
        "Produces GRAY8 frames filled with the frame number", "course");

    bsc->fixate = GST_DEBUG_FUNCPTR(my_counter_fixate);
    bsc->set_caps = GST_DEBUG_FUNCPTR(my_counter_set_caps);
    bsc->start = GST_DEBUG_FUNCPTR(my_counter_start);
    psc->create = GST_DEBUG_FUNCPTR(my_counter_create);
}

static void my_counter_init(MyCounter *self)
{
    /* Sources default to BYTES format; video sources speak TIME so that
     * segments, seeking and position queries are in nanoseconds. */
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    /* Not live: we produce as fast as downstream accepts (like a file).
     * A camera would call gst_base_src_set_live(TRUE). */
    gst_base_src_set_live(GST_BASE_SRC(self), FALSE);
    gst_video_info_init(&self->info);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "mycounter", GST_RANK_NONE,
                                MY_TYPE_COUNTER);
}

/* ======================================================================
 *  The application
 * ====================================================================== */

/* Runs mycounter with an optional caps filter; checks every frame. */
static void run_and_check(const char *filter, int want_w, int want_h,
                          int want_fps)
{
    gchar *desc = g_strdup_printf(
        "mycounter num-buffers=10 %s ! appsink name=out sync=false", filter);
    GError *err = NULL;
    GstElement *pipe = gst_parse_launch(desc, &err);   /* owned */
    CHECK(pipe != NULL, err ? err->message : "parse");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipe), "out"); /* ref */
    CHECK(gst_element_set_state(pipe, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "PLAYING");

    guint64 i = 0;
    GstSample *s;
    while ((s = gst_app_sink_pull_sample(GST_APP_SINK(sink))) != NULL) {
        GstCaps *caps = gst_sample_get_caps(s);       /* borrowed */
        GstStructure *st = gst_caps_get_structure(caps, 0);
        int w = 0, h = 0, fn = 0, fd = 1;
        gst_structure_get_int(st, "width", &w);
        gst_structure_get_int(st, "height", &h);
        gst_structure_get_fraction(st, "framerate", &fn, &fd);
        if (i == 0)
            printf("  %-40s -> %dx%d @ %d/%d\n", desc, w, h, fn, fd);
        CHECK(w == want_w && h == want_h && fn == want_fps && fd == 1,
              "caps not as expected");

        GstBuffer *b = gst_sample_get_buffer(s);      /* borrowed */
        GstClockTime want_pts = gst_util_uint64_scale(i, GST_SECOND, fn);
        CHECK(GST_BUFFER_PTS(b) == want_pts, "PTS wrong");
        GstMapInfo m;
        CHECK(gst_buffer_map(b, &m, GST_MAP_READ), "map");
        CHECK(m.size == (gsize)(w * h), "GRAY8 size = w*h for these widths");
        CHECK(m.data[0] == i % 256 && m.data[m.size - 1] == i % 256,
              "frame content must equal frame number");
        gst_buffer_unmap(b, &m);
        gst_sample_unref(s);
        i++;
    }
    CHECK(i == 10, "num-buffers=10 must give exactly 10 frames then EOS");

    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipe);
    g_free(desc);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    CHECK(gst_plugin_register_static(GST_VERSION_MAJOR, GST_VERSION_MINOR,
                                     "mycounter", "course counter source",
                                     plugin_init, "1.0", "LGPL", "course",
                                     "course", "local"),
          "register");

    printf("negotiation results:\n");
    /* Nobody constrains -> our fixate() defaults win. */
    run_and_check("", 64, 48, 30);
    /* Downstream fixes width/framerate -> fixate only picks height. */
    run_and_check("! video/x-raw,width=32,framerate=10/1", 32, 48, 10);
    /* A range downstream -> nearest to our default inside it. */
    run_and_check("! video/x-raw,width=[100,200],height=120", 100, 120, 30);

    if (display) {
        GstElement *v = gst_parse_launch(
            "mycounter num-buffers=256 ! video/x-raw,width=320,height=240 ! "
            "videoconvert ! autovideosink", NULL);
        CHECK(v != NULL, "display pipeline");
        gst_element_set_state(v, GST_STATE_PLAYING);
        GstBus *bus = gst_element_get_bus(v);
        GstMessage *m = gst_bus_timed_pop_filtered(
            bus, 20 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        if (m) gst_message_unref(m);
        gst_object_unref(bus);
        gst_element_set_state(v, GST_STATE_NULL);
        gst_object_unref(v);
    }

    printf("OK: source fixates caps, timestamps frames, counts correctly\n");
    gst_deinit();
    return 0;
}
