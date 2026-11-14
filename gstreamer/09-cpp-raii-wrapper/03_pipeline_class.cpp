/*
 * 03_pipeline_class.cpp — a small, move-only RAII Pipeline class
 *
 * Concept : Wrap the whole "parse -> PLAYING -> bus loop -> NULL -> unref"
 *           life cycle in one C++ class:
 *             - constructor parses (throws on error), destructor sets NULL
 *               and unrefs: impossible to forget either
 *             - move-only (copy deleted): exactly one owner of the pipeline
 *             - on(GST_MESSAGE_xxx, lambda): per-message-type handlers
 *             - onSample("appsink", lambda): appsink frames into std::function
 *           Then use it to count frames and check their caps.
 * Build   : make run P=gstreamer/09-cpp-raii-wrapper/03_pipeline_class.cpp
 *           ./build/gstreamer/09-cpp-raii-wrapper/03_pipeline_class --display
 * Read    : README.md  (section "An RAII Pipeline class")
 *
 *   videotestsrc num-buffers=30 ─► capsfilter(RGB 64x48) ─► tee ─► queue ─► appsink "out"
 *                                                            └─► queue ─► videoconvert ─► autovideosink
 *                                                                (only with --display)
 */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", msg,        \
                         __FILE__, __LINE__);                              \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

namespace gst {

struct ObjectUnref  { void operator()(gpointer p) const { gst_object_unref(p); } };
struct MessageUnref { void operator()(GstMessage *m) const { gst_message_unref(m); } };
struct SampleUnref  { void operator()(GstSample *s) const { gst_sample_unref(s); } };
using Bus     = std::unique_ptr<GstBus, ObjectUnref>;
using Element = std::unique_ptr<GstElement, ObjectUnref>;
using Message = std::unique_ptr<GstMessage, MessageUnref>;
using Sample  = std::unique_ptr<GstSample, SampleUnref>;

class Pipeline {
public:
    using MessageHandler = std::function<void(GstMessage *)>;
    using SampleHandler  = std::function<void(GstSample *)>;

    explicit Pipeline(const std::string &desc)
    {
        GError *err = nullptr;
        pipe_ = gst_parse_launch(desc.c_str(), &err);   // full ref, ours
        // TRAP: for a missing element ("no element foo") parse_launch may
        // return a NON-NULL partial pipeline AND set err. Treat any error as
        // failure, and unref the partial pipeline so it does not leak.
        if (!pipe_ || err) {
            std::string what = err ? err->message : "parse failed";
            g_clear_error(&err);
            if (pipe_) gst_object_unref(std::exchange(pipe_, nullptr));
            throw std::runtime_error(what);
        }
    }

    ~Pipeline() { reset(); }

    // Copy would mean two owners of one ref -> double unref. Forbid it.
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;

    // Move transfers ownership: the source is left empty (nullptr).
    Pipeline(Pipeline &&o) noexcept
        : pipe_(std::exchange(o.pipe_, nullptr)),
          handlers_(std::move(o.handlers_)),
          sample_handlers_(std::move(o.sample_handlers_)) {}
    Pipeline &operator=(Pipeline &&o) noexcept
    {
        if (this != &o) {
            reset();
            pipe_ = std::exchange(o.pipe_, nullptr);
            handlers_ = std::move(o.handlers_);
            sample_handlers_ = std::move(o.sample_handlers_);
        }
        return *this;
    }

    GstElement *get() const { return pipe_; }

    Element element(const char *name) const
    {
        return Element(gst_bin_get_by_name(GST_BIN(pipe_), name));
    }

    Pipeline &on(GstMessageType type, MessageHandler h)
    {
        handlers_[type] = std::move(h);
        return *this;
    }

    // The handler runs in the appsink's STREAMING THREAD. Keep it short and
    // make any shared state thread-safe (std::atomic, mutex).
    Pipeline &onSample(const char *appsink_name, SampleHandler h)
    {
        Element sink = element(appsink_name);
        if (!sink) throw std::runtime_error("no appsink named " + std::string(appsink_name));
        // unique_ptr gives the handler a stable address for the C callback;
        // the map keeps it alive as long as the Pipeline.
        auto holder = std::make_unique<SampleHandler>(std::move(h));
        GstAppSinkCallbacks cb{};
        // C callback: must be a plain function; a captureless lambda converts.
        cb.new_sample = [](GstAppSink *s, gpointer ud) -> GstFlowReturn {
            Sample smp(gst_app_sink_pull_sample(s));
            if (!smp) return GST_FLOW_EOS;
            (*static_cast<SampleHandler *>(ud))(smp.get());
            return GST_FLOW_OK;
        };
        gst_app_sink_set_callbacks(GST_APP_SINK(sink.get()), &cb, holder.get(), nullptr);
        sample_handlers_[appsink_name] = std::move(holder);
        return *this;
    }

    // Runs until EOS or ERROR (or timeout). Returns true on EOS.
    bool run(GstClockTime timeout = 10 * GST_SECOND)
    {
        if (gst_element_set_state(pipe_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            return false;
        Bus bus(gst_element_get_bus(pipe_));
        // Listen to what we have handlers for, plus the two that end the run.
        // In C++ the OR of GstMessageType flags is an int: cast it back.
        int mask = GST_MESSAGE_EOS | GST_MESSAGE_ERROR;
        for (const auto &kv : handlers_) mask |= kv.first;
        const auto filter = static_cast<GstMessageType>(mask);

        for (;;) {
            Message msg(gst_bus_timed_pop_filtered(bus.get(), timeout, filter));
            if (!msg) return false;                     // timeout
            const GstMessageType t = GST_MESSAGE_TYPE(msg.get());
            auto it = handlers_.find(t);
            if (it != handlers_.end()) it->second(msg.get());
            if (t == GST_MESSAGE_EOS) return true;
            if (t == GST_MESSAGE_ERROR) return false;
        }   // msg unreffed at the end of each iteration, even on return
    }

private:
    void reset()
    {
        if (pipe_) {
            gst_element_set_state(pipe_, GST_STATE_NULL);   // stop threads first
            gst_object_unref(pipe_);
            pipe_ = nullptr;
        }
    }

    GstElement *pipe_ = nullptr;
    std::map<GstMessageType, MessageHandler> handlers_;
    std::map<std::string, std::unique_ptr<SampleHandler>> sample_handlers_;
};

} // namespace gst

static gst::Pipeline make_pipeline(bool display)
{
    std::string desc =
        "videotestsrc num-buffers=30 ! video/x-raw,format=RGB,width=64,height=48"
        " ! tee name=t t. ! queue ! appsink name=out sync=false";
    if (display) desc += " t. ! queue ! videoconvert ! autovideosink";
    return gst::Pipeline(desc);       // moved out (or elided): one owner
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    const bool display = (argc > 1 && std::strcmp(argv[1], "--display") == 0);

    std::atomic<int> frames{0};
    std::atomic<bool> caps_ok{true};
    int state_changes = 0;
    bool eos = false;
    {
        gst::Pipeline p = make_pipeline(display);
        gst::Pipeline moved = std::move(p);              // ownership moves
        CHECK(p.get() == nullptr && moved.get() != nullptr, "move semantics");

        moved.on(GST_MESSAGE_STATE_CHANGED, [&](GstMessage *m) {
                 // only count the pipeline's own transitions
                 if (GST_MESSAGE_SRC(m) == GST_OBJECT(moved.get())) state_changes++;
             })
             .on(GST_MESSAGE_ERROR, [](GstMessage *m) {
                 GError *e = nullptr;
                 gst_message_parse_error(m, &e, nullptr);
                 std::fprintf(stderr, "ERROR: %s\n", e->message);
                 g_error_free(e);
             })
             .onSample("out", [&](GstSample *s) {   // streaming thread!
                 GstVideoInfo vi;
                 if (!gst_video_info_from_caps(&vi, gst_sample_get_caps(s)) ||
                     GST_VIDEO_INFO_WIDTH(&vi) != 64 ||
                     GST_VIDEO_INFO_FORMAT(&vi) != GST_VIDEO_FORMAT_RGB)
                     caps_ok = false;
                 frames++;
             });

        try {
            gst::Pipeline bad("this-element-does-not-exist ! fakesink");
            CHECK(false, "constructor should have thrown");
        } catch (const std::runtime_error &e) {
            std::printf("expected failure caught: %s\n", e.what());
        }

        eos = moved.run();
        // ~Pipeline here: NULL + unref, for 'moved' (p is empty: no-op).
    }
    std::printf("frames=%d state_changes=%d eos=%d\n", frames.load(),
                state_changes, eos);
    CHECK(eos, "EOS");
    CHECK(frames == 30, "frame count");
    CHECK(caps_ok, "caps of samples");
    CHECK(state_changes >= 3, "NULL->READY->PAUSED->PLAYING");
    std::printf("OK: RAII pipeline ran, handlers fired, everything released\n");
    gst_deinit();
    return 0;
}
