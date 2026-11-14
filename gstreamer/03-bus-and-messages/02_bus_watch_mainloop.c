/*
 * 02_bus_watch_mainloop.c — event-driven apps: GMainLoop + bus watch
 *
 * Concept : real apps do not block in gst_bus_timed_pop; they run a
 *           GMainLoop and get a callback per message, plus timers (here:
 *           a position query every 100 ms). We also post our OWN
 *           application message from the streaming thread: the standard
 *           way to hand work from a streaming thread to the main thread.
 * Build   : make run P=gstreamer/03-bus-and-messages/02_bus_watch_mainloop.c
 *           ./build/gstreamer/03-bus-and-messages/02_bus_watch_mainloop --display
 * Read    : README.md  (section "Bus watch and GMainLoop")
 *
 *   streaming thread                         main thread (GMainLoop)
 *   identity "handoff" every 10th buffer ──► bus_cb: APPLICATION "progress"
 *   fakesink EOS ──────────────────────────► bus_cb: EOS -> quit loop
 *                                             timer: query position
 *
 *   videotestsrc ─► identity name=tap ─► videoconvert ─► fakesink sync=true
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

typedef struct {
    GMainLoop *loop;
    GstElement *pipeline;       /* borrowed */
    int app_msgs;               /* touched only in the main thread */
    int timer_ticks;
    gint64 last_pos;
    gboolean got_eos;
    GThread *main_thread;
    gboolean callback_in_main;
} App;

/* Runs in the STREAMING thread. Never touch UI/app state here; post a
 * message instead. gst_element_post_message takes ownership of msg. */
static void on_handoff(GstElement *identity, GstBuffer *buf, gpointer data)
{
    (void)data;
    guint64 n = GST_BUFFER_OFFSET(buf);       /* videotestsrc: frame number */
    if (n % 10 != 0)
        return;
    GstStructure *s = gst_structure_new("progress", "frame", G_TYPE_UINT64,
                                        n, NULL);
    gst_element_post_message(identity,
        gst_message_new_application(GST_OBJECT(identity), s));
}

/* Runs in the MAIN thread (the GMainContext the watch was added to).
 * Return TRUE to keep the watch. */
static gboolean bus_cb(GstBus *bus, GstMessage *m, gpointer data)
{
    (void)bus;
    App *app = data;
    app->callback_in_main = (g_thread_self() == app->main_thread);
    switch (GST_MESSAGE_TYPE(m)) {
    case GST_MESSAGE_APPLICATION: {
        const GstStructure *s = gst_message_get_structure(m);   /* none */
        if (gst_structure_has_name(s, "progress")) {
            guint64 f = 0;
            gst_structure_get_uint64(s, "frame", &f);
            printf("  app message: reached frame %" G_GUINT64_FORMAT "\n", f);
            app->app_msgs++;
        }
        break;
    }
    case GST_MESSAGE_ERROR: {
        GError *e = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(m, &e, &dbg);
        fprintf(stderr, "ERROR %s: %s\n", GST_OBJECT_NAME(m->src), e->message);
        g_error_free(e);
        g_free(dbg);
        g_main_loop_quit(app->loop);
        break;
    }
    case GST_MESSAGE_EOS:
        printf("  EOS -> quit main loop\n");
        app->got_eos = TRUE;
        g_main_loop_quit(app->loop);
        break;
    default:
        break;
    }
    return TRUE;
}

/* Timer in the main thread: a POSITION query walks to the sink. */
static gboolean on_timer(gpointer data)
{
    App *app = data;
    gint64 pos = 0;
    if (gst_element_query_position(app->pipeline, GST_FORMAT_TIME, &pos)) {
        app->last_pos = pos;
        app->timer_ticks++;
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    App app = { 0 };
    app.main_thread = g_thread_self();
    app.loop = g_main_loop_new(NULL, FALSE);
    /* sync=true on purpose: 30 frames at 30 fps = 1 s of real time, so the
     * 100 ms timer fires several times. */
    app.pipeline = gst_parse_launch(display
        ? "videotestsrc num-buffers=30 ! identity name=tap ! videoconvert ! autovideosink"
        : "videotestsrc num-buffers=30 ! video/x-raw,framerate=30/1"
          " ! identity name=tap ! videoconvert ! fakesink sync=true", NULL);
    CHECK(app.pipeline, "parse");

    GstElement *tap = gst_bin_get_by_name(GST_BIN(app.pipeline), "tap");
    g_signal_connect(tap, "handoff", G_CALLBACK(on_handoff), NULL);
    gst_object_unref(tap);

    GstBus *bus = gst_element_get_bus(app.pipeline);
    /* add_watch: the bus becomes a GSource in the default main context.
     * Returns a source id; one watch per bus. */
    guint watch_id = gst_bus_add_watch(bus, bus_cb, &app);
    gst_object_unref(bus);                 /* the watch keeps its own ref */
    guint timer_id = g_timeout_add(100, on_timer, &app);

    gst_element_set_state(app.pipeline, GST_STATE_PLAYING);
    g_main_loop_run(app.loop);             /* returns after quit() */

    g_source_remove(timer_id);
    g_source_remove(watch_id);
    gst_element_set_state(app.pipeline, GST_STATE_NULL);
    gst_object_unref(app.pipeline);
    g_main_loop_unref(app.loop);

    printf("app messages=%d timer ticks=%d last position=%" GST_TIME_FORMAT
           "\n", app.app_msgs, app.timer_ticks, GST_TIME_ARGS(app.last_pos));
    CHECK(app.got_eos, "EOS received");
    CHECK(app.app_msgs == 3, "frames 0, 10, 20 reported");
    CHECK(app.callback_in_main, "bus callback ran in the main thread");
    CHECK(app.timer_ticks >= 3, "position polled while playing");
    printf("OK: bus watch, app messages and timers in one main loop\n");
    gst_deinit();
    return 0;
}
