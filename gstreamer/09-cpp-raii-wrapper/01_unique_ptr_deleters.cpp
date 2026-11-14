/*
 * 01_unique_ptr_deleters.cpp — let C++ unref GStreamer objects for you
 *
 * Concept : Every GStreamer "get"/"new"/"make" that returns a full reference
 *           must be paired with exactly one unref. In C that is a manual
 *           discipline; every early return is a potential leak. In C++ we
 *           wrap each pointer in std::unique_ptr with a custom deleter that
 *           calls the right unref function, so the destructor does it on
 *           every path (normal return, early return, exception).
 *           Also shown: why flag enums need static_cast in C++.
 * Build   : make run P=gstreamer/09-cpp-raii-wrapper/01_unique_ptr_deleters.cpp
 *           ./build/gstreamer/09-cpp-raii-wrapper/01_unique_ptr_deleters --display
 * Read    : README.md  (sections "How it works" and "C vs C++")
 *
 *   videotestsrc num-buffers=40 ─► capsfilter ─► videoconvert ─► fakesink
 *
 *   ElementPtr ──owns 1 ref──► GstPipeline ──owns──► children
 *   BusPtr     ──owns 1 ref──► GstBus
 *   MessagePtr ──owns 1 ref──► GstMessage   (each is unreffed in its dtor)
 */
#include <gst/gst.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", msg,        \
                         __FILE__, __LINE__);                              \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

// ---- deleters: one tiny struct per unref function --------------------------
// A stateless struct (not a function pointer) as the deleter keeps
// sizeof(unique_ptr) == sizeof(raw pointer): zero overhead.
struct GstObjectDeleter  { void operator()(gpointer p) const { gst_object_unref(p); } };
struct GstMessageDeleter { void operator()(GstMessage *m) const { gst_message_unref(m); } };
struct GstCapsDeleter    { void operator()(GstCaps *c) const { gst_caps_unref(c); } };
struct GErrorDeleter     { void operator()(GError *e) const { g_error_free(e); } };
struct GFreeDeleter      { void operator()(gpointer p) const { g_free(p); } };

using ElementPtr = std::unique_ptr<GstElement, GstObjectDeleter>;
using BusPtr     = std::unique_ptr<GstBus, GstObjectDeleter>;
using PadPtr     = std::unique_ptr<GstPad, GstObjectDeleter>;
using MessagePtr = std::unique_ptr<GstMessage, GstMessageDeleter>;
using CapsPtr    = std::unique_ptr<GstCaps, GstCapsDeleter>;
using ErrorPtr   = std::unique_ptr<GError, GErrorDeleter>;
using GCharPtr   = std::unique_ptr<gchar, GFreeDeleter>;

static_assert(sizeof(ElementPtr) == sizeof(GstElement *),
              "stateless deleter costs nothing");

// A pipeline must be set to NULL before its last unref (threads stopped,
// devices closed). Encode that rule in the deleter itself.
struct PipelineDeleter {
    void operator()(GstElement *p) const {
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
    }
};
using PipelinePtr = std::unique_ptr<GstElement, PipelineDeleter>;

static PipelinePtr parse(const std::string &desc)
{
    GError *raw = nullptr;
    // gst_parse_launch returns a full (non-floating) ref -> PipelinePtr owns it.
    PipelinePtr p(gst_parse_launch(desc.c_str(), &raw));
    ErrorPtr err(raw);                       // freed automatically, if set
    if (!p) std::fprintf(stderr, "parse: %s\n", err ? err->message : "?");
    return p;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    const bool display = (argc > 1 && std::strcmp(argv[1], "--display") == 0);

    {   // inner scope: every smart pointer is destroyed before gst_deinit()
        PipelinePtr pipeline = parse(
            std::string("videotestsrc num-buffers=40"
                        " ! video/x-raw,format=I420,width=320,height=240"
                        " ! videoconvert name=conv ! ") +
            (display ? "autovideosink" : "fakesink sync=false"));
        CHECK(pipeline, "pipeline");

        // gst_bin_get_by_name: transfer full -> wrap immediately.
        ElementPtr conv(gst_bin_get_by_name(GST_BIN(pipeline.get()), "conv"));
        PadPtr sinkpad(gst_element_get_static_pad(conv.get(), "sink"));
        CHECK(sinkpad, "pad");

        CHECK(gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) !=
                  GST_STATE_CHANGE_FAILURE, "PLAYING");

        BusPtr bus(gst_element_get_bus(pipeline.get()));

        // C++ RULE: GST_MESSAGE_EOS | GST_MESSAGE_ERROR is an int in C++
        // (enum | enum -> int), and int does NOT convert implicitly back to
        // GstMessageType. C allows it; C++ needs an explicit cast.
        const auto mask =
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        MessagePtr msg(gst_bus_timed_pop_filtered(bus.get(), 10 * GST_SECOND, mask));
        CHECK(msg, "timeout");

        if (GST_MESSAGE_TYPE(msg.get()) == GST_MESSAGE_ERROR) {
            GError *e = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(msg.get(), &e, &dbg);
            ErrorPtr err(e);          // both freed whatever happens next
            GCharPtr debug(dbg);
            std::fprintf(stderr, "ERROR: %s\n", err->message);
            return 1;                 // early return: NO leak, NO missing NULL
        }

        // Caps are refcounted too (gst_pad_get_current_caps: transfer full).
        CapsPtr caps(gst_pad_get_current_caps(sinkpad.get()));
        CHECK(caps, "caps");
        GCharPtr str(gst_caps_to_string(caps.get()));
        std::printf("negotiated on conv.sink: %s\n", str.get());
        const GstStructure *s = gst_caps_get_structure(caps.get(), 0); // borrowed
        int w = 0, h = 0;
        CHECK(gst_structure_get_int(s, "width", &w) &&
              gst_structure_get_int(s, "height", &h), "size fields");
        CHECK(w == 320 && h == 240, "caps size");
        CHECK(std::strcmp(gst_structure_get_string(s, "format"), "I420") == 0,
              "caps format");
        std::printf("got %s\n", GST_MESSAGE_TYPE_NAME(msg.get()));
        // End of scope, destroyed in REVERSE order of construction:
        // str, caps, msg, bus, sinkpad, conv, pipeline (-> NULL, unref).
    }
    std::printf("OK: all objects released by destructors, pipeline set to NULL first\n");
    gst_deinit();
    return 0;
}
