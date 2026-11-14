/*
 * 05_appsrc_to_appsink.cpp — C++17: your frames in, processed frames out
 *
 * Concept : The full "GStreamer as a processing engine inside my app"
 *           pattern. A producer thread pushes RGB frames into appsrc;
 *           GStreamer converts them to GRAY8 and downscales by 2; a consumer
 *           thread pulls the results from appsink. Every GStreamer object is
 *           held by a std::unique_ptr with a custom deleter, so no early
 *           return can leak a ref. Flag enums need static_cast in C++.
 * Build   : make run P=gstreamer/06-appsrc-appsink/05_appsrc_to_appsink.cpp
 *           ./build/gstreamer/06-appsrc-appsink/05_appsrc_to_appsink --display
 * Read    : README.md  (section "5. C vs C++")
 *
 *   std::thread producer          GStreamer threads                        std::thread consumer
 *   RGB 320x240 ─► appsrc ─► videoconvert ─► videoscale ─► GRAY8 160x120 ─► appsink ─► pull, check
 *    (solid colour per frame)                                                  (luma of that colour)
 */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", msg,        \
                         __FILE__, __LINE__);                              \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

// ---- RAII: one deleter per GStreamer "unref" function ---------------------
// unique_ptr<T, D> calls D()(ptr) when it goes out of scope. Each deleter is
// an empty struct, so the unique_ptr is exactly one pointer in size.
struct ObjectUnref  { void operator()(gpointer p) const { gst_object_unref(p); } };
struct SampleUnref  { void operator()(GstSample *s) const { gst_sample_unref(s); } };
struct MessageUnref { void operator()(GstMessage *m) const { gst_message_unref(m); } };
struct CapsUnref    { void operator()(GstCaps *c) const { gst_caps_unref(c); } };

using ElementPtr = std::unique_ptr<GstElement, ObjectUnref>;
using BusPtr     = std::unique_ptr<GstBus, ObjectUnref>;
using SamplePtr  = std::unique_ptr<GstSample, SampleUnref>;
using MessagePtr = std::unique_ptr<GstMessage, MessageUnref>;
using CapsPtr    = std::unique_ptr<GstCaps, CapsUnref>;

// The pipeline must reach NULL before its last unref. A tiny guard owns the
// pipeline and does both, in the right order, even on early exit.
class Pipeline {
public:
    explicit Pipeline(GstElement *p) : p_(p) {}
    ~Pipeline() { if (p_) gst_element_set_state(p_.get(), GST_STATE_NULL); }
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;
    GstElement *get() const { return p_.get(); }
    // gst_bin_get_by_name is transfer full -> wrap immediately.
    ElementPtr by_name(const char *n) const {
        return ElementPtr(gst_bin_get_by_name(GST_BIN(p_.get()), n));
    }
private:
    ElementPtr p_;
};

constexpr int kW = 320, kH = 240, kFps = 30, kFrames = 60;

struct Rgb { guint8 r, g, b; };
static Rgb colour_of(int n) {        // a different solid colour per frame
    return Rgb{static_cast<guint8>(n * 4), static_cast<guint8>(255 - n * 4), 128};
}
// GRAY8 caps default to full range (0..255) with BT.601 weights, so
// videoconvert computes Y = 0.299 R + 0.587 G + 0.114 B. (YUV formats like
// I420 default to limited range 16..235 - a classic "why is my black 16?".)
static int expected_luma(Rgb c) {
    return static_cast<int>(0.299 * c.r + 0.587 * c.g + 0.114 * c.b + 0.5);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    const bool display = argc > 1 && std::strcmp(argv[1], "--display") == 0;

    std::string desc =
        "appsrc name=in format=time ! videoconvert ! videoscale ! "
        "video/x-raw,format=GRAY8,width=160,height=120 ! ";
    desc += display ? "tee name=t ! queue ! videoconvert ! autovideosink "
                      "t. ! queue ! appsink name=out max-buffers=8"
                    : "appsink name=out sync=false max-buffers=8";

    GError *err = nullptr;
    Pipeline pipe(gst_parse_launch(desc.c_str(), &err));
    CHECK(pipe.get() && !err, "parse");
    ElementPtr in = pipe.by_name("in");
    ElementPtr out = pipe.by_name("out");

    CapsPtr caps(gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "RGB", "width", G_TYPE_INT, kW,
        "height", G_TYPE_INT, kH, "framerate", GST_TYPE_FRACTION, kFps, 1,
        nullptr));
    gst_app_src_set_caps(GST_APP_SRC(in.get()), caps.get());  // takes its own ref
    g_object_set(in.get(), "block", TRUE,
                 "max-bytes", static_cast<guint64>(4 * kW * kH * 3), nullptr);

    CHECK(gst_element_set_state(pipe.get(), GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "PLAYING");

    // ---- producer thread: our "camera" --------------------------------
    std::thread producer([&] {
        for (int n = 0; n < kFrames; ++n) {
            // std::vector -> GstBuffer: here we COPY (gst_buffer_fill).
            // Lesson 13 shows wrapping memory without a copy.
            std::vector<guint8> rgb(kW * kH * 3);
            const Rgb c = colour_of(n);
            for (size_t i = 0; i < rgb.size(); i += 3) {
                rgb[i] = c.r; rgb[i + 1] = c.g; rgb[i + 2] = c.b;
            }
            GstBuffer *buf = gst_buffer_new_allocate(nullptr, rgb.size(), nullptr);
            gst_buffer_fill(buf, 0, rgb.data(), rgb.size());
            GST_BUFFER_PTS(buf) = gst_util_uint64_scale(n, GST_SECOND, kFps);
            GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(1, GST_SECOND, kFps);
            // transfer full: appsrc owns buf now. Blocks when queue is full.
            if (gst_app_src_push_buffer(GST_APP_SRC(in.get()), buf) != GST_FLOW_OK)
                return;  // pipeline stopping (flushing)
        }
        gst_app_src_end_of_stream(GST_APP_SRC(in.get()));
    });

    // ---- consumer thread: our "algorithm" -----------------------------
    std::atomic<int> received{0};
    std::atomic<bool> pixels_ok{true};
    std::thread consumer([&] {
        for (;;) {
            SamplePtr s(gst_app_sink_try_pull_sample(GST_APP_SINK(out.get()),
                                                     5 * GST_SECOND));
            if (!s)
                break;  // EOS or timeout
            GstBuffer *b = gst_sample_get_buffer(s.get());     // borrowed
            GstMapInfo m;
            if (!gst_buffer_map(b, &m, GST_MAP_READ)) { pixels_ok = false; break; }
            const int n = received.load();
            const int want = expected_luma(colour_of(n));
            const int got = m.data[60 * 160 + 80];             // centre pixel
            if (std::abs(got - want) > 2 || m.size != 160 * 120) {
                std::fprintf(stderr, "frame %d: luma %d, expected %d\n", n, got, want);
                pixels_ok = false;
            }
            gst_buffer_unmap(b, &m);
            ++received;
            // s goes out of scope -> gst_sample_unref, every iteration.
        }
    });

    producer.join();
    consumer.join();

    // Bus check: EOS, not ERROR. timed_pop_filtered takes a GstMessageType
    // FLAGS value. In C, "GST_MESSAGE_EOS | GST_MESSAGE_ERROR" is an int that
    // silently converts back to the enum; in C++ int -> enum has no implicit
    // conversion, so we must static_cast. Same for GstMapFlags, GstSeekFlags,
    // GstPadProbeType ...
    BusPtr bus(gst_element_get_bus(pipe.get()));
    MessagePtr msg(gst_bus_timed_pop_filtered(bus.get(), 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)));
    CHECK(msg && GST_MESSAGE_TYPE(msg.get()) == GST_MESSAGE_EOS, "EOS on bus");

    std::printf("pushed %d RGB frames, received %d GRAY8 160x120 frames\n",
                kFrames, received.load());
    CHECK(received == kFrames, "frame count");
    CHECK(pixels_ok, "converted luma matches BT.601 full-range of our colour");
    std::printf("OK: frames in -> convert+scale -> frames out, no manual unrefs\n");
    // Destruction order (reverse of declaration): msg, bus, caps, out, in,
    // then pipe (set NULL, unref). gst_deinit must run after all of them,
    // so we leave it to the OS here rather than calling it with live refs.
    return 0;
}
