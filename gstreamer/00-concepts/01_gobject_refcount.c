/*
 * 01_gobject_refcount.c — the GObject rules every GStreamer program lives by
 *
 * Concept : reference counting, FLOATING references (and who "sinks" them),
 *           properties (g_object_set/get, notify::), and signals. Getting
 *           these wrong is the #1 source of leaks and crashes in GStreamer
 *           code, so this program prints and ASSERTS every refcount.
 * Build   : make run P=gstreamer/00-concepts/01_gobject_refcount.c
 * Read    : README.md  (section "GObject in 5 minutes")
 *
 *   gst_element_factory_make()  -> refcount 1, FLOATING   ("nobody owns me yet")
 *   gst_bin_add(bin, e)         -> bin SINKS the floating ref: count stays 1,
 *                                  now OWNED by the bin (not by you!)
 *   gst_object_ref(e)           -> 2  (you take your own ref)
 *   gst_object_unref(bin)       -> bin drops its ref -> 1 (your ref)
 *   gst_object_unref(e)         -> 0  -> finalize, memory freed
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

#define RC(obj) ((int)GST_OBJECT_REFCOUNT_VALUE(obj))

/* A weak reference callback: GObject calls it while the object is being
 * finalized. We use it to PROVE that an object really died. */
static void on_finalized(gpointer data, GObject *where_the_object_was)
{
    (void)where_the_object_was;
    *(gboolean *)data = TRUE;
}

/* "notify::<property>" signal handler: called synchronously, in the thread
 * that changed the property, every time the property changes. */
static void on_notify(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    int *count = user_data;
    (*count)++;
    printf("  notify: %s.%s changed\n", GST_OBJECT_NAME(obj), pspec->name);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    /* ---------------------------------------------------------------
     * 1. A fresh element is FLOATING with refcount 1.
     *    "Floating" = a ref that nobody has claimed yet. The first
     *    container that takes it (gst_bin_add, gst_element_add_pad...)
     *    "sinks" it instead of adding a new ref. This lets C code write
     *    gst_bin_add(bin, gst_element_factory_make(...)) without a leak.
     * --------------------------------------------------------------- */
    GstElement *src = gst_element_factory_make("videotestsrc", "src");
    CHECK(src != NULL, "videotestsrc missing (gst-plugins-base)");
    printf("1. new element: refcount=%d floating=%d\n", RC(src),
           g_object_is_floating(src));
    CHECK(RC(src) == 1 && g_object_is_floating(src), "new element floating");

    gboolean src_dead = FALSE;
    g_object_weak_ref(G_OBJECT(src), on_finalized, &src_dead);

    /* ---------------------------------------------------------------
     * 2. Put it in a bin: the bin SINKS the floating ref. Refcount stays
     *    1, but the one reference now belongs to the bin. From here on
     *    YOU hold a *borrowed* pointer: valid only while the bin lives.
     * --------------------------------------------------------------- */
    GstElement *bin = gst_bin_new("mybin");        /* floating, rc=1 */
    gst_object_ref_sink(bin);   /* we claim the bin ourselves: not floating */
    CHECK(!g_object_is_floating(bin), "bin sunk");

    gst_bin_add(GST_BIN(bin), src);
    printf("2. after gst_bin_add: refcount=%d floating=%d (owned by bin)\n",
           RC(src), g_object_is_floating(src));
    CHECK(RC(src) == 1 && !g_object_is_floating(src), "bin sank the ref");
    CHECK(GST_OBJECT_PARENT(src) == GST_OBJECT(bin), "parent is the bin");

    /* ---------------------------------------------------------------
     * 3. Getter functions that return objects give you a NEW ref
     *    ("transfer full"): you must unref it. gst_bin_get_by_name is
     *    one; gst_element_get_static_pad and gst_element_get_bus too.
     * --------------------------------------------------------------- */
    GstElement *found = gst_bin_get_by_name(GST_BIN(bin), "src");
    printf("3. gst_bin_get_by_name: refcount=%d (bin's + ours)\n", RC(found));
    CHECK(found == src && RC(src) == 2, "get_by_name returns new ref");
    gst_object_unref(found);                /* forget this = leak forever */
    CHECK(RC(src) == 1, "back to the bin's single ref");

    GstPad *pad = gst_element_get_static_pad(src, "src");
    int pad_rc = RC(pad);
    printf("   gst_element_get_static_pad: pad refcount=%d\n", pad_rc);
    CHECK(pad_rc == 2, "element's ref + ours");
    gst_object_unref(pad);

    /* ---------------------------------------------------------------
     * 4. Properties: typed key/value settings, introspectable at runtime
     *    (that's how gst-inspect-1.0 lists them). Changes emit
     *    "notify::<name>".
     * --------------------------------------------------------------- */
    int notifications = 0;
    gulong hid = g_signal_connect(src, "notify::pattern",
                                  G_CALLBACK(on_notify), &notifications);
    /* Enum properties can be set by nick string with gst_util_set_object_arg,
     * or by number with g_object_set. 18 = "ball". */
    gst_util_set_object_arg(G_OBJECT(src), "pattern", "ball");
    g_object_set(src, "num-buffers", 10, "is-live", FALSE, NULL);

    gint nb = 0;
    gint pattern = -1;
    g_object_get(src, "num-buffers", &nb, "pattern", &pattern, NULL);
    printf("4. num-buffers=%d pattern=%d notifications=%d\n", nb, pattern,
           notifications);
    CHECK(nb == 10, "num-buffers property round-trips");
    CHECK(pattern == 18, "pattern=ball is enum value 18");
    CHECK(notifications == 1, "one notify::pattern signal");
    g_signal_handler_disconnect(src, hid);

    /* Strings from g_object_get are COPIES: you g_free them. */
    gchar *name = NULL;
    g_object_get(src, "name", &name, NULL);
    CHECK(strcmp(name, "src") == 0, "name property");
    g_free(name);

    /* ---------------------------------------------------------------
     * 5. Keep an element alive beyond its bin: take your own ref first.
     * --------------------------------------------------------------- */
    gst_object_ref(src);                         /* rc 2: bin + us */
    gst_object_unref(bin);                       /* bin dies, drops its ref */
    printf("5. after bin is gone: refcount=%d dead=%d\n", RC(src), src_dead);
    CHECK(RC(src) == 1 && !src_dead, "our ref keeps src alive");

    gst_object_unref(src);                       /* last ref -> finalize */
    printf("   after last unref: dead=%d\n", src_dead);
    CHECK(src_dead, "element finalized when refcount hit 0");

    /* ---------------------------------------------------------------
     * 6. Mini-objects (GstBuffer, GstCaps, GstMessage, GstEvent) are NOT
     *    GObjects: no floating refs, no signals, but the same refcount
     *    idea plus copy-on-write ("writable only when refcount == 1").
     * --------------------------------------------------------------- */
    GstCaps *caps = gst_caps_from_string("video/x-raw,width=320");
    CHECK(gst_caps_is_writable(caps), "fresh caps writable");
    GstCaps *caps2 = gst_caps_ref(caps);
    CHECK(!gst_caps_is_writable(caps), "shared caps are read-only");
    gst_caps_unref(caps2);
    gst_caps_unref(caps);
    printf("6. mini-objects: copy-on-write once shared\n");

    printf("OK: all refcount, property and signal rules verified\n");
    gst_deinit();
    return 0;
}
