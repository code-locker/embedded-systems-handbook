/*
 * 02_refcount_mistakes.cpp — watch reference counts move, see a leak happen
 *
 * Concept : The GObject refcount is a plain integer you can print with
 *           GST_OBJECT_REFCOUNT_VALUE(). We walk through the rules that cause
 *           90% of GStreamer leaks and crashes, printing and asserting the
 *           count at every step:
 *             1. factory_make returns a FLOATING ref; gst_bin_add sinks it
 *             2. get_static_pad / get_bus / get_by_name each add +1
 *             3. setting a caps property does NOT take your caps ref
 *             4. one forgotten unref keeps an object alive forever (leak),
 *                proven with a GObject weak pointer that is cleared only on
 *                finalize; then we fix it and see it finalized.
 *           The program leaves NO leak behind: every "mistake" is undone.
 * Build   : make run P=gstreamer/09-cpp-raii-wrapper/02_refcount_mistakes.cpp
 * Read    : README.md  (section "The refcount rules")
 *
 *   pipeline ──owns──► src (videotestsrc) ─► sink (fakesink)
 *   us       ──extra ref (the "leak")──► src
 */
#include <gst/gst.h>
#include <cstdio>
#include <cstdlib>

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", msg,        \
                         __FILE__, __LINE__);                              \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

static int rc(gpointer obj) { return GST_OBJECT_REFCOUNT_VALUE(obj); }

static void show(const char *step, gpointer obj)
{
    std::printf("  %-52s refcount=%d%s\n", step, rc(obj),
                g_object_is_floating(obj) ? "  (floating)" : "");
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    // (no --display here: this lesson is about counting, not pictures)

    std::printf("1. floating references\n");
    GstElement *pipeline = gst_pipeline_new("p");     // floating ref
    // Sink it so we hold a normal ref: the pipeline has no parent to do it.
    gst_object_ref_sink(pipeline);
    show("pipeline after ref_sink (we own it)", pipeline);
    CHECK(rc(pipeline) == 1 && !g_object_is_floating(pipeline), "pipeline");

    GstElement *src = gst_element_factory_make("videotestsrc", "src");
    GstElement *sink = gst_element_factory_make("fakesink", "sink");
    show("src after factory_make", src);
    CHECK(rc(src) == 1 && g_object_is_floating(src), "floating");

    // Weak pointer: GObject sets 'src_alive' to NULL when src is finalized.
    // That is our leak detector.
    gpointer src_alive = src;
    g_object_add_weak_pointer(G_OBJECT(src), &src_alive);

    // gst_bin_add "takes ownership": it sinks the floating ref. The count
    // stays 1, but that 1 now belongs to the pipeline, not to us.
    gst_bin_add_many(GST_BIN(pipeline), src, sink, nullptr);
    show("src after gst_bin_add (owned by pipeline)", src);
    CHECK(rc(src) == 1 && !g_object_is_floating(src), "sunk by bin");
    // MISTAKE (not executed): gst_object_unref(src) here would destroy the
    // pipeline's ref -> pipeline later uses freed memory -> crash.
    CHECK(gst_element_link(src, sink), "link");
    g_object_set(src, "num-buffers", 10, nullptr);
    g_object_set(sink, "sync", FALSE, nullptr);

    std::printf("2. getters that return a NEW reference (transfer full)\n");
    GstPad *pad = gst_element_get_static_pad(src, "src");
    show("pad after get_static_pad", pad);
    int pad_base = rc(pad) - 1;               // the element's own ref(s)
    GstPad *pad2 = gst_element_get_static_pad(src, "src");
    show("pad after a 2nd get_static_pad", pad);
    CHECK(rc(pad) == pad_base + 2, "each call adds +1");
    gst_object_unref(pad2);
    gst_object_unref(pad);
    std::printf("  %-52s refcount=%d\n", "pad after both unrefs", rc(pad));
    CHECK(rc(pad) == pad_base, "back to the element's own ref");
    // (pad is still alive: src owns it. We just must not use our pointer
    //  as if we owned it any more.)

    GstBus *bus1 = gst_element_get_bus(pipeline);
    int bus_base = rc(bus1) - 1;
    GstBus *bus2 = gst_element_get_bus(pipeline);
    show("bus after two get_bus calls", bus1);
    CHECK(rc(bus1) == bus_base + 2, "get_bus adds +1 per call");
    gst_object_unref(bus2);

    GstElement *by_name = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    show("src after gst_bin_get_by_name", src);
    CHECK(by_name == src && rc(src) == 2, "get_by_name adds +1");
    gst_object_unref(by_name);
    CHECK(rc(src) == 1, "released");

    std::printf("3. setting a property does NOT steal your caps ref\n");
    GstElement *cf = gst_element_factory_make("capsfilter", nullptr);
    gst_object_ref_sink(cf);
    GstCaps *caps = gst_caps_from_string("video/x-raw,width=64,height=48");
    std::printf("  %-52s refcount=%d\n", "caps after from_string",
                GST_MINI_OBJECT_REFCOUNT_VALUE(caps));
    g_object_set(cf, "caps", caps, nullptr);   // capsfilter takes its OWN ref
    std::printf("  %-52s refcount=%d\n", "caps after g_object_set",
                GST_MINI_OBJECT_REFCOUNT_VALUE(caps));
    CHECK(GST_MINI_OBJECT_REFCOUNT_VALUE(caps) == 2, "setter took its own ref");
    gst_caps_unref(caps);      // MUST still drop ours -> otherwise leak
    gst_object_unref(cf);      // finalizes cf, which drops its caps ref

    std::printf("4. one forgotten unref = a leak\n");
    GstElement *leaked = GST_ELEMENT(gst_object_ref(src));  // "forgot" this
    show("src with our extra ref", src);
    CHECK(rc(src) == 2, "extra ref");

    // Run the pipeline so it is a real, used object.
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstMessage *m = gst_bus_timed_pop_filtered(
        bus1, 10 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    CHECK(m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS, "EOS");
    gst_message_unref(m);
    gst_object_unref(bus1);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);      // pipeline finalized: drops its src ref

    std::printf("  after pipeline unref: src %s (refcount=%d)\n",
                src_alive ? "STILL ALIVE -> LEAK" : "finalized",
                src_alive ? rc(src) : 0);
    CHECK(src_alive != nullptr, "our extra ref must keep src alive");
    CHECK(rc(src) == 1, "exactly our leaked ref is left");
    CHECK(GST_OBJECT_PARENT(src) == nullptr, "orphaned: no parent any more");

    gst_object_unref(leaked);        // the FIX
    std::printf("  after the fix (unref):  src %s\n",
                src_alive ? "still alive" : "finalized");
    CHECK(src_alive == nullptr, "src must be finalized now");

    std::printf("OK: every count matched the rules; no reference left behind\n");
    gst_deinit();
    return 0;
}
