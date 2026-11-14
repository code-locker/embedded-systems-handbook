/*
 * 03_pad_availability.c — ALWAYS, SOMETIMES and REQUEST pads, from the templates
 *
 * Concept : Every element factory publishes "pad templates": blueprints that
 *           say direction (SRC/SINK), availability (ALWAYS/SOMETIMES/REQUEST)
 *           and caps. This program prints them for a few factories (the same
 *           info as the "Pad Templates" section of gst-inspect-1.0) and then
 *           requests and releases a pad on a tee, checking refcounts.
 * Build   : make run P=gstreamer/04-dynamic-pads/03_pad_availability.c
 * Read    : README.md  (section "2. Mental model": the three kinds of pads)
 *
 *    ALWAYS        SOMETIMES                 REQUEST
 *   ┌───────┐     ┌──────────────┐          ┌─────────┐
 *  ►│sink src│►  ►│sink   video_%u│► (later) ►│sink src_0│►  (you ask: request_pad)
 *   └───────┘     │       audio_%u│►          │     src_1│►
 *   identity      └──────────────┘          └─────────┘
 *                 matroskademux               tee
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

static const char *presence_name(GstPadPresence p)
{
    switch (p) {
    case GST_PAD_ALWAYS:    return "ALWAYS";
    case GST_PAD_SOMETIMES: return "SOMETIMES";
    case GST_PAD_REQUEST:   return "REQUEST";
    }
    return "?";
}

/* Prints templates and returns a bitmask of presences seen (1<<presence). */
static unsigned print_templates(const char *factory_name)
{
    /* gst_element_factory_find: transfer full -> gst_object_unref. A factory
     * is cheap to inspect: no element instance is created. */
    GstElementFactory *f = gst_element_factory_find(factory_name);
    CHECK(f != NULL, "factory exists (is the plugin installed?)");
    unsigned seen = 0;
    printf("%-14s\n", factory_name);
    /* The list and its templates are owned by the factory: transfer none. */
    const GList *l = gst_element_factory_get_static_pad_templates(f);
    for (; l; l = l->next) {
        GstStaticPadTemplate *t = l->data;
        GstCaps *caps = gst_static_caps_get(&t->static_caps);   /* own */
        gchar *cs = gst_caps_to_string(caps);
        if (strlen(cs) > 50)
            strcpy(cs + 47, "...");                       /* keep it short */
        printf("   %-4s %-12s %-10s %s\n",
               t->direction == GST_PAD_SRC ? "SRC" : "SINK",
               t->name_template, presence_name(t->presence), cs);
        g_free(cs);
        gst_caps_unref(caps);
        seen |= 1u << t->presence;
    }
    gst_object_unref(f);
    return seen;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    /* Part 1: templates. */
    unsigned m;
    m = print_templates("identity");
    CHECK(m == (1u << GST_PAD_ALWAYS), "identity has only ALWAYS pads");
    m = print_templates("matroskademux");
    CHECK(m & (1u << GST_PAD_SOMETIMES), "demuxer has SOMETIMES src pads");
    m = print_templates("tee");
    CHECK(m & (1u << GST_PAD_REQUEST), "tee has REQUEST src pads");
    m = print_templates("uridecodebin");
    CHECK(m & (1u << GST_PAD_SOMETIMES), "uridecodebin src pads are SOMETIMES");

    /* Part 2: request a pad from tee, then release it.
     *   tee (floating) -> we sink it with ref_sink so WE own it (no bin). */
    GstElement *tee = gst_element_factory_make("tee", "t");
    gst_object_ref_sink(tee);                 /* floating -> owned by us */
    CHECK(tee->numsrcpads == 0, "tee starts with no src pads");

    /* GStreamer >= 1.20: gst_element_request_pad_simple (old name:
     * gst_element_get_request_pad). Returns transfer FULL: one ref for us,
     * and the element holds its own ref because the pad is now its child. */
    GstPad *p0 = gst_element_request_pad_simple(tee, "src_%u");
    GstPad *p1 = gst_element_request_pad_simple(tee, "src_%u");
    CHECK(p0 && p1, "got two request pads");
    printf("requested %s and %s; tee now has %d src pads\n",
           GST_PAD_NAME(p0), GST_PAD_NAME(p1), tee->numsrcpads);
    CHECK(tee->numsrcpads == 2, "two src pads after requesting");
    printf("refcount of %s: %d (1 tee + 1 ours)\n", GST_PAD_NAME(p0),
           GST_OBJECT_REFCOUNT_VALUE(p0));
    CHECK(GST_OBJECT_REFCOUNT_VALUE(p0) == 2, "refcount 2");

    /* Static pad lookup on a request pad name fails: request pads are not
     * static pads. The sink pad of a tee is ALWAYS, so it works: */
    GstPad *sink = gst_element_get_static_pad(tee, "sink");   /* own */
    CHECK(sink != NULL, "tee sink is an always pad");
    gst_object_unref(sink);

    /* The correct give-back sequence: release (element drops its ref),
     * then unref (we drop ours). Forgetting either one = leaked pad. */
    gst_element_release_request_pad(tee, p0);
    CHECK(GST_OBJECT_REFCOUNT_VALUE(p0) == 1, "only our ref left");
    gst_object_unref(p0);
    gst_element_release_request_pad(tee, p1);
    gst_object_unref(p1);
    CHECK(tee->numsrcpads == 0, "all request pads released");

    gst_object_unref(tee);
    printf("OK: templates show availability; request pads given back cleanly\n");
    gst_deinit();
    return 0;
}
