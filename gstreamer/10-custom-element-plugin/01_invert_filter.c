/*
 * 01_invert_filter.c — write your own element: an in-place GRAY8 inverter
 *
 * Concept : a custom filter element built on GstBaseTransform. We define a
 *           GObject type (MyInvert), give it pad templates, metadata, one
 *           property ("invert"), implement set_caps + transform_ip, register
 *           it with gst_plugin_register_static() so it lives INSIDE this
 *           program (no .so needed), then use it by name in a pipeline.
 * Build   : make run P=gstreamer/10-custom-element-plugin/01_invert_filter.c
 *           ./build/gstreamer/10-custom-element-plugin/01_invert_filter --display
 * Read    : README.md  (sections "How it works" and "Building it as a real
 *           .so plugin with meson")
 *
 *   videotestsrc pattern=white ─► capsfilter GRAY8 ─► myinvert ─► appsink
 *        235 235 235 ...                             20 20 20 ... (we check)
 *
 *   Inside myinvert (GstBaseTransform does all the pad work for us):
 *
 *        sink pad ──► [ set_caps(): learn width/height/stride once ]
 *                     [ transform_ip(): buf[i] = 255 - buf[i]       ] ──► src pad
 *                     [ passthrough if invert=FALSE: never called   ]
 */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/base/gstbasetransform.h>
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
 *  Part 1 — the element (this is what would go into a plugin .so)
 * ====================================================================== */

/* G_DECLARE_FINAL_TYPE writes the boilerplate declarations:
 *   typedef struct _MyInvert MyInvert;  MyInvertClass;  my_invert_get_type();
 *   MY_INVERT(obj) cast macro,  MY_IS_INVERT(obj) check macro.
 * FINAL = nobody may subclass it, so the instance struct can be private. */
#define MY_TYPE_INVERT (my_invert_get_type())
G_DECLARE_FINAL_TYPE(MyInvert, my_invert, MY, INVERT, GstBaseTransform)

struct _MyInvert {
    GstBaseTransform parent;   /* MUST be first: C "inheritance" by layout */
    gboolean invert;           /* the property                             */
    GstVideoInfo info;         /* filled in set_caps: width/height/stride  */
    guint frames;              /* how many frames we actually touched      */
};

/* G_DEFINE_TYPE writes my_invert_get_type() (registers the type once, on
 * first use) and a static pointer my_invert_parent_class. */
G_DEFINE_TYPE(MyInvert, my_invert, GST_TYPE_BASE_TRANSFORM)

enum { PROP_0, PROP_INVERT };

/* Pad templates: "what can ever flow through my pads". Always pads, one
 * sink, one src, both only GRAY8. Caps negotiation intersects these with
 * the neighbours' caps; if the intersection is empty -> not-negotiated. */
static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE("GRAY8")));
static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE("GRAY8")));

/* set_property / get_property: GObject routes g_object_set(e,"invert",..)
 * here. Properties can be changed from the app thread while the streaming
 * thread runs transform_ip, so we take the object lock. */
static void my_invert_set_property(GObject *obj, guint id, const GValue *v,
                                   GParamSpec *pspec)
{
    MyInvert *self = MY_INVERT(obj);
    switch (id) {
    case PROP_INVERT:
        GST_OBJECT_LOCK(self);
        self->invert = g_value_get_boolean(v);
        GST_OBJECT_UNLOCK(self);
        /* Passthrough = "output buffer IS the input buffer, don't call
         * transform_ip at all". Zero cost when the filter is switched off. */
        gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self),
                                           !self->invert);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void my_invert_get_property(GObject *obj, guint id, GValue *v,
                                   GParamSpec *pspec)
{
    MyInvert *self = MY_INVERT(obj);
    switch (id) {
    case PROP_INVERT:
        GST_OBJECT_LOCK(self);
        g_value_set_boolean(v, self->invert);
        GST_OBJECT_UNLOCK(self);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

/* Called (streaming thread) once caps are agreed, before the first buffer
 * and again whenever caps change. Parse ONCE here, not per frame. */
static gboolean my_invert_set_caps(GstBaseTransform *trans, GstCaps *incaps,
                                   GstCaps *outcaps)
{
    (void)outcaps; /* in == out for this filter */
    MyInvert *self = MY_INVERT(trans);
    if (!gst_video_info_from_caps(&self->info, incaps))
        return FALSE; /* -> not-negotiated error */
    GST_INFO_OBJECT(self, "negotiated %dx%d stride %d",
                    GST_VIDEO_INFO_WIDTH(&self->info),
                    GST_VIDEO_INFO_HEIGHT(&self->info),
                    GST_VIDEO_INFO_PLANE_STRIDE(&self->info, 0));
    return TRUE;
}

/* The work. In-place: we modify the buffer we were given. BaseTransform
 * already made it writable (copying only if someone else holds a ref).
 * Cost: O(width*height) per frame, no allocation, no copy. */
static GstFlowReturn my_invert_transform_ip(GstBaseTransform *trans,
                                            GstBuffer *buf)
{
    MyInvert *self = MY_INVERT(trans);
    GstVideoFrame frame;

    /* gst_video_frame_map gives per-plane pointers + strides. Rows may be
     * padded (stride > width): never assume data is width*height packed. */
    if (!gst_video_frame_map(&frame, &self->info, buf, GST_MAP_READWRITE))
        return GST_FLOW_ERROR;

    guint8 *row = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const int w = GST_VIDEO_FRAME_WIDTH(&frame);
    const int h = GST_VIDEO_FRAME_HEIGHT(&frame);

    for (int y = 0; y < h; y++, row += stride)
        for (int x = 0; x < w; x++)
            row[x] = (guint8)(255 - row[x]);

    gst_video_frame_unmap(&frame);
    self->frames++;
    return GST_FLOW_OK;
}

/* class_init: runs ONCE per type. Fill the vtable (vfuncs), install
 * properties, add pad templates and metadata. */
static void my_invert_class_init(MyInvertClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *bt_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = my_invert_set_property;
    gobject_class->get_property = my_invert_get_property;

    g_object_class_install_property(
        gobject_class, PROP_INVERT,
        g_param_spec_boolean("invert", "Invert",
                             "Invert pixels (FALSE = passthrough)", TRUE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                 GST_PARAM_CONTROLLABLE));

    gst_element_class_add_static_pad_template(element_class, &sink_tmpl);
    gst_element_class_add_static_pad_template(element_class, &src_tmpl);

    /* Shown by gst-inspect-1.0 myinvert. Klass string is used by autopluggers
     * (decodebin looks for "Decoder", etc.). */
    gst_element_class_set_static_metadata(
        element_class, "Gray inverter", "Filter/Effect/Video",
        "Inverts GRAY8 pixels in place (course example)", "course");

    bt_class->set_caps = GST_DEBUG_FUNCPTR(my_invert_set_caps);
    bt_class->transform_ip = GST_DEBUG_FUNCPTR(my_invert_transform_ip);
    /* PITFALL: transform_ip_on_passthrough defaults to TRUE, i.e. BaseTransform
     * STILL calls transform_ip in passthrough mode (meant for "analysis"
     * elements that only read the buffer). We modify data, so turn it off,
     * otherwise invert=false would keep inverting (on a buffer that may be
     * shared and read-only!). */
    bt_class->transform_ip_on_passthrough = FALSE;
    /* We do not implement transform(): in-place is enough because output
     * caps == input caps and size is unchanged. */
}

/* init: runs for EVERY new instance. Set defaults. */
static void my_invert_init(MyInvert *self)
{
    self->invert = TRUE;
    self->frames = 0;
    gst_video_info_init(&self->info);
}

/* plugin_init: the plugin's entry point. Registers element factories. */
static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "myinvert", GST_RANK_NONE,
                                MY_TYPE_INVERT);
}

/* ======================================================================
 *  Part 2 — the application that uses it
 * ====================================================================== */

/* Run "videotestsrc pattern=<pattern> ! GRAY8 ! myinvert invert=<inv> !
 * appsink", pull 5 frames, return the first pixel of the last frame. */
static int run_once(const char *pattern, gboolean invert, guint *frames_seen)
{
    gchar *desc = g_strdup_printf(
        "videotestsrc pattern=%s num-buffers=5 ! "
        "video/x-raw,format=GRAY8,width=64,height=48 ! "
        "myinvert name=inv invert=%s ! appsink name=out sync=false",
        pattern, invert ? "true" : "false");
    GError *err = NULL;
    GstElement *pipe = gst_parse_launch(desc, &err); /* we own it */
    g_free(desc);
    CHECK(pipe != NULL, err ? err->message : "parse failed");

    /* gst_bin_get_by_name returns a NEW ref -> must unref. */
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipe), "out");
    GstElement *inv = gst_bin_get_by_name(GST_BIN(pipe), "inv");
    CHECK(gst_element_set_state(pipe, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "PLAYING failed");

    int last_pixel = -1, n = 0;
    GstSample *s;
    /* pull_sample blocks until a frame or EOS (NULL). We own each sample. */
    while ((s = gst_app_sink_pull_sample(GST_APP_SINK(sink))) != NULL) {
        GstBuffer *b = gst_sample_get_buffer(s); /* borrowed from sample */
        GstMapInfo map;
        CHECK(gst_buffer_map(b, &map, GST_MAP_READ), "map");
        last_pixel = map.data[0];
        gst_buffer_unmap(b, &map);
        gst_sample_unref(s);
        n++;
    }
    CHECK(n == 5, "expected 5 frames");
    *frames_seen = MY_INVERT(inv)->frames; /* frames transform_ip touched */

    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(inv);
    gst_object_unref(sink);
    gst_object_unref(pipe);
    return last_pixel;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* Register our "plugin" from inside the program. Same effect as dropping
     * a libgstmyinvert.so into GST_PLUGIN_PATH, but the registry is not
     * involved: the factory exists only in this process. */
    CHECK(gst_plugin_register_static(GST_VERSION_MAJOR, GST_VERSION_MINOR,
                                     "myinvert", "course invert filter",
                                     plugin_init, "1.0", "LGPL", "course",
                                     "course", "local"),
          "plugin registration failed");

    /* Factory lookup works now, exactly like for built-in elements. */
    GstElementFactory *f = gst_element_factory_find("myinvert"); /* ref */
    CHECK(f != NULL, "factory myinvert not found");
    printf("factory 'myinvert': klass=%s\n",
           gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS));
    gst_object_unref(f);

    guint touched = 0;

    /* First run with invert=false (passthrough) to learn the source value.
     * Surprise: videotestsrc "white" in GRAY8 is 235, not 255, and black is
     * 16: video uses "limited range" (16..235) unless caps say otherwise. */
    const int white = run_once("white", FALSE, &touched);
    printf("white  + invert=false -> pixel %3d, transform_ip calls %u "
           "(passthrough)\n", white, touched);
    CHECK(touched == 0, "passthrough must skip transform_ip");
    CHECK(white > 200, "white should be bright");

    const int black = run_once("black", FALSE, &touched);
    printf("black  + invert=false -> pixel %3d\n", black);
    CHECK(black < 50, "black should be dark");

    int p = run_once("white", TRUE, &touched);
    printf("white  + invert=true  -> pixel %3d (= 255 - %d), transform_ip "
           "calls %u\n", p, white, touched);
    CHECK(p == 255 - white, "white should be inverted");
    CHECK(touched == 5, "transform_ip should run once per frame");

    p = run_once("black", TRUE, &touched);
    printf("black  + invert=true  -> pixel %3d (= 255 - %d)\n", p, black);
    CHECK(p == 255 - black, "black should be inverted");

    if (display) {
        GstElement *v = gst_parse_launch(
            "videotestsrc num-buffers=150 ! video/x-raw,format=GRAY8 ! "
            "myinvert ! videoconvert ! autovideosink", NULL);
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

    printf("OK: custom element registered, inverts, and passthrough works\n");
    gst_deinit();
    return 0;
}
