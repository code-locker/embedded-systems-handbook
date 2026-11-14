/*
 * 01_caps_basics.c — caps as data: parse, inspect, intersect, subset, fixate
 *
 * Concept : GstCaps is a list of GstStructures; each structure is a media
 *           type name plus fields. A field can be a single value (fixed) or
 *           a range / list (unfixed). Negotiation is just set algebra on
 *           these: intersect what both sides accept, then fixate (pick one
 *           value per field). No pipeline here: caps are plain data.
 * Build   : make run P=gstreamer/05-caps-and-negotiation/01_caps_basics.c
 * Read    : README.md  (sections "2. Mental model" and "3. How it works")
 *
 *   src can do:  video/x-raw, format={I420,RGB}, width=[1,4096], height=[1,4096]
 *   sink wants:  video/x-raw, format=RGB,        width=[320,1920], height=240
 *                       ∩ (intersect)
 *   common:      video/x-raw, format=RGB,        width=[320,1920], height=240
 *                       fixate  (ranges -> nearest to a preferred / lowest value)
 *   fixed:       video/x-raw, format=RGB,        width=320,        height=240
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

static void show(const char *label, const GstCaps *caps)
{
    gchar *s = gst_caps_to_string(caps);                 /* own: g_free */
    printf("%-10s %s\n", label, s);
    g_free(s);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    /* 1. Parse. gst_caps_from_string returns a new caps (refcount 1): own. */
    GstCaps *src = gst_caps_from_string(
        "video/x-raw, format=(string){ I420, RGB }, "
        "width=(int)[ 1, 4096 ], height=(int)[ 1, 4096 ], "
        "framerate=(fraction)[ 0/1, 120/1 ]");
    GstCaps *sink = gst_caps_from_string(
        "video/x-raw, format=RGB, width=[ 320, 1920 ], height=240, "
        "framerate=30/1");
    CHECK(src && sink, "parsed");
    show("src:", src);
    show("sink:", sink);
    CHECK(!gst_caps_is_fixed(src), "src caps are NOT fixed (lists/ranges)");

    /* 2. Inspect one structure. Structure belongs to the caps: borrowed. */
    const GstStructure *s0 = gst_caps_get_structure(sink, 0);
    CHECK(gst_structure_has_name(s0, "video/x-raw"), "media type");
    gint h = 0;
    CHECK(gst_structure_get_int(s0, "height", &h) && h == 240, "height=240");
    CHECK(g_str_equal(gst_structure_get_string(s0, "format"), "RGB"), "RGB");
    /* width is a range: get_int fails on it, read the GValue instead. */
    gint w = 0;
    CHECK(!gst_structure_get_int(s0, "width", &w), "range is not an int");
    const GValue *wv = gst_structure_get_value(s0, "width");
    CHECK(GST_VALUE_HOLDS_INT_RANGE(wv), "width holds an int range");
    printf("width range: %d..%d\n", gst_value_get_int_range_min(wv),
           gst_value_get_int_range_max(wv));

    /* 3. Intersect: what both sides accept. New caps, we own it. */
    GstCaps *common = gst_caps_intersect(src, sink);
    show("common:", common);
    CHECK(!gst_caps_is_empty(common), "they can talk");
    CHECK(gst_caps_is_subset(common, src) && gst_caps_is_subset(common, sink),
          "intersection is a subset of both");

    /* 4. Incompatible: sink only takes GRAY8 -> empty intersection.
     *    In a pipeline this is the famous "not-negotiated" error. */
    GstCaps *gray = gst_caps_from_string("video/x-raw, format=GRAY8");
    GstCaps *none = gst_caps_intersect(gray, sink);
    show("gray∩sink:", none);
    CHECK(gst_caps_is_empty(none), "no common format");
    CHECK(!gst_caps_can_intersect(gray, sink), "can_intersect agrees");

    /* 5. Fixate: pick one value from every range/list. gst_caps_fixate takes
     *    ownership (transfer full) and returns the fixed caps. Elements may
     *    first steer it, e.g. "prefer width nearest to 640". */
    GstCaps *steer = gst_caps_make_writable(gst_caps_ref(common));
    GstStructure *ws = gst_caps_get_structure(steer, 0);  /* writable now */
    gst_structure_fixate_field_nearest_int(ws, "width", 640);
    GstCaps *fixed = gst_caps_fixate(steer);              /* steer consumed */
    show("fixed:", fixed);
    CHECK(gst_caps_is_fixed(fixed), "fixed");
    const GstStructure *fs = gst_caps_get_structure(fixed, 0);
    CHECK(gst_structure_get_int(fs, "width", &w) && w == 640,
          "width fixated to 640");

    /* 6. Caps are refcounted and copy-on-write: a 2nd ref makes them
     *    read-only; make_writable copies if refcount > 1. */
    GstCaps *alias = gst_caps_ref(fixed);
    CHECK(!gst_caps_is_writable(fixed), "shared caps are read-only");
    gst_caps_unref(alias);
    CHECK(gst_caps_is_writable(fixed), "sole owner may write again");

    /* 7. Building caps in code (no string parsing). */
    GstCaps *built = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "RGB",
        "width", G_TYPE_INT, 640, "height", G_TYPE_INT, 240,
        "framerate", GST_TYPE_FRACTION, 30, 1, NULL);
    CHECK(gst_caps_is_equal(built, fixed), "built by hand == fixated");

    gst_caps_unref(built);
    gst_caps_unref(fixed);
    gst_caps_unref(none);
    gst_caps_unref(gray);
    gst_caps_unref(common);
    gst_caps_unref(sink);
    gst_caps_unref(src);
    printf("OK: caps algebra (intersect, subset, fixate) behaves as expected\n");
    gst_deinit();
    return 0;
}
