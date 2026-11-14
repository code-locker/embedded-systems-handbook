/*
 * 04_first_pipeline.cpp — program 01 in C++17, and the enum-cast gotcha
 *
 * Concept : same 6-step skeleton, but ownership is held by std::unique_ptr
 *           with custom deleters, so every early return still cleans up.
 *           GStreamer flag enums OR'ed together are int in C++ and need an
 *           explicit static_cast back to the enum type.
 * Build   : make run P=gstreamer/02-first-pipeline/04_first_pipeline.cpp
 *           ./build/gstreamer/02-first-pipeline/04_first_pipeline --display
 * Read    : README.md  (section "C vs C++"), then lesson 09 for the full wrapper
 *
 *   videotestsrc num-buffers=60 ──► videoconvert ──► fakesink
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

// Deleters: one per "how do I release this" rule.
struct PipelineDeleter {        // must go to NULL BEFORE the last unref
    void operator()(GstElement *e) const {
        gst_element_set_state(e, GST_STATE_NULL);
        gst_object_unref(e);
    }
};
struct ObjectDeleter  { void operator()(gpointer o) const { gst_object_unref(o); } };
struct MessageDeleter { void operator()(GstMessage *m) const { gst_message_unref(m); } };

using PipelinePtr = std::unique_ptr<GstElement, PipelineDeleter>;
using BusPtr      = std::unique_ptr<GstBus, ObjectDeleter>;
using MessagePtr  = std::unique_ptr<GstMessage, MessageDeleter>;

static bool run(bool display)
{
    const std::string desc = std::string("videotestsrc num-buffers=60 ! videoconvert ! ")
        + (display ? "autovideosink" : "fakesink sync=false");

    GError *err = nullptr;
    PipelinePtr pipeline(gst_parse_launch(desc.c_str(), &err));
    if (err) {                       // can be set even if a pipeline came back
        std::fprintf(stderr, "parse error: %s\n", err->message);
        g_error_free(err);
        return false;                // unique_ptr cleans up the partial pipeline
    }

    if (gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE)
        return false;

    BusPtr bus(gst_element_get_bus(pipeline.get()));

    // In C: GST_MESSAGE_EOS | GST_MESSAGE_ERROR is fine (enums are ints).
    // In C++: enum | enum -> int, and int does NOT implicitly convert back
    // to GstMessageType -> compile error. Cast explicitly:
    const auto filter =
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    MessagePtr msg(gst_bus_timed_pop_filtered(bus.get(), 10 * GST_SECOND, filter));

    if (!msg) {
        std::fprintf(stderr, "timeout\n");
        return false;
    }
    std::printf("got %s\n", GST_MESSAGE_TYPE_NAME(msg.get()));
    return GST_MESSAGE_TYPE(msg.get()) == GST_MESSAGE_EOS;
    // msg, bus, pipeline released here in reverse order: msg, bus, pipeline.
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    const bool display = argc > 1 && std::strcmp(argv[1], "--display") == 0;
    CHECK(run(display), "pipeline reached EOS");
    std::printf("OK: C++ RAII version reached EOS, nothing to free by hand\n");
    gst_deinit();
    return 0;
}
