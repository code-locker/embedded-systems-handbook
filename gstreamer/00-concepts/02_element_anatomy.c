/*
 * 02_element_anatomy.c — a mini gst-inspect-1.0: look inside an element
 *
 * Concept : every element comes from a FACTORY (registered by a plugin).
 *           The factory knows metadata (klass, rank) and PAD TEMPLATES
 *           (which pads may exist, their direction, availability and caps).
 *           The element instance has PROPERTIES, discoverable at runtime.
 * Build   : make run P=gstreamer/00-concepts/02_element_anatomy.c
 *           ./build/gstreamer/00-concepts/02_element_anatomy tee
 * Read    : README.md  (sections "Elements, pads, caps" and "Registry")
 *
 *   registry ──► plugin "videotestsrc" ──► factory "videotestsrc"
 *                                             │ metadata, rank
 *                                             │ pad templates: src (ALWAYS)
 *                                             ▼
 *                                   element instance + properties
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

static const char *dir_name(GstPadDirection d)
{
    return d == GST_PAD_SRC ? "SRC" : d == GST_PAD_SINK ? "SINK" : "?";
}

static const char *presence_name(GstPadPresence p)
{
    switch (p) {
    case GST_PAD_ALWAYS:    return "ALWAYS";    /* exists from creation   */
    case GST_PAD_SOMETIMES: return "SOMETIMES"; /* appears at runtime     */
    case GST_PAD_REQUEST:   return "REQUEST";   /* you ask for it         */
    }
    return "?";
}

/* Prints the anatomy of one element; returns the number of pad templates. */
static guint inspect(const char *factory_name)
{
    /* gst_element_factory_find: transfer full -> unref when done. */
    GstElementFactory *f = gst_element_factory_find(factory_name);
    if (!f) {
        printf("%s: not installed\n", factory_name);
        return 0;
    }
    printf("== %s ==\n", factory_name);
    printf("  long name : %s\n",
           gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_LONGNAME));
    printf("  klass     : %s\n",
           gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS));
    printf("  rank      : %u\n",
           gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(f)));

    /* Pad templates: the "sockets" the element CAN have. transfer none:
     * the list belongs to the factory, do not free it. */
    const GList *l = gst_element_factory_get_static_pad_templates(f);
    guint n = 0;
    for (; l; l = l->next, n++) {
        GstStaticPadTemplate *t = l->data;
        GstCaps *caps = gst_static_caps_get(&t->static_caps); /* full */
        gchar *s = gst_caps_to_string(caps);
        printf("  pad %-10s %-4s %-9s caps: %.60s%s\n", t->name_template,
               dir_name(t->direction), presence_name(t->presence), s,
               strlen(s) > 60 ? "..." : "");
        g_free(s);
        gst_caps_unref(caps);
    }

    /* Properties live on the instance's class. Create one to list them. */
    GstElement *e = gst_element_factory_create(f, NULL);   /* floating */
    gst_object_ref_sink(e);                                /* own it   */
    guint np = 0;
    GParamSpec **props =
        g_object_class_list_properties(G_OBJECT_GET_CLASS(e), &np);
    printf("  %u properties:", np);
    for (guint i = 0; i < np && i < 8; i++)
        printf(" %s", props[i]->name);
    printf("%s\n", np > 8 ? " ..." : "");
    g_free(props);              /* the array is ours, the specs are not */
    gst_object_unref(e);
    gst_object_unref(f);
    return n;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    if (argc > 1) {                     /* inspect what the user asks for */
        return inspect(argv[1]) > 0 ? 0 : 1;
    }

    /* One of each pad availability, all in gst-plugins-base / core. */
    CHECK(inspect("videotestsrc") == 1, "videotestsrc: 1 template (src)");
    CHECK(inspect("videoconvert") == 2, "videoconvert: sink + src");
    CHECK(inspect("tee") == 2, "tee: sink + src_%u");
    CHECK(inspect("oggdemux") == 2, "oggdemux: sink + src_%08x");

    /* Verify availability precisely on a live tee instance. */
    GstElement *tee = gst_element_factory_make("tee", NULL);
    gst_object_ref_sink(tee);
    GstPadTemplate *t =
        gst_element_get_pad_template(tee, "src_%u");   /* transfer none */
    CHECK(t && GST_PAD_TEMPLATE_PRESENCE(t) == GST_PAD_REQUEST,
          "tee src pads are REQUEST pads");
    CHECK(tee->numsrcpads == 0, "no src pads until you request one");
    GstPad *p = gst_element_request_pad_simple(tee, "src_%u");   /* full */
    CHECK(p && tee->numsrcpads == 1, "requested pad now exists");
    gst_element_release_request_pad(tee, p);    /* give it back ...       */
    gst_object_unref(p);                        /* ... and drop our ref   */
    gst_object_unref(tee);

    /* How many element factories does this machine know? */
    GList *all = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_ANY, GST_RANK_NONE);
    guint count = g_list_length(all);
    gst_plugin_feature_list_free(all);
    printf("registry knows %u element factories\n", count);
    CHECK(count > 50, "plugins-base/good installed");

    printf("OK: factories, pad templates and properties inspected\n");
    gst_deinit();
    return 0;
}
