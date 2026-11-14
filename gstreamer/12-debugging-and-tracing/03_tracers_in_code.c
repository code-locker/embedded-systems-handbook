/*
 * 03_tracers_in_code.c — turn on the latency + stats tracers from code and
 *                        read their records
 *
 * Concept : Tracers are plugins that hook into the core (pad pushes, state
 *           changes, object creation...) and emit structured records on the
 *           debug category GST_TRACER at level TRACE (7). They are enabled
 *           with GST_TRACERS=... which is read inside gst_init(), so set it
 *           BEFORE gst_init(). We capture the records with a log function
 *           and assert the latency tracer measured our source->sink path.
 * Build   : make run P=gstreamer/12-debugging-and-tracing/03_tracers_in_code.c
 * Read    : README.md  (section "How it works: tracers")
 *
 *   videotestsrc ─► queue ─► videoconvert ─► fakesink
 *       │ t0: buffer leaves src pad                 │ t1: buffer reaches sink
 *       └──────────────── latency = t1 - t0 ────────┘
 *   record:  latency, src-element=videotestsrc0, sink-element=fakesink0,
 *            time=(guint64)123456, ts=..., ...
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
    gint latency_records;     /* "latency, ..." lines                  */
    gint stats_records;       /* any other tracer record (stats, ...)  */
    gint printed;             /* how many sample lines we echoed       */
} TraceCounts;

/* Runs in the streaming thread that pushed the buffer. Atomics only. */
static void on_log(GstDebugCategory *cat, GstDebugLevel level,
                   const gchar *file, const gchar *func, gint line,
                   GObject *object, GstDebugMessage *message, gpointer data)
{
    (void)level; (void)file; (void)func; (void)line; (void)object;
    if (strcmp(gst_debug_category_get_name(cat), "GST_TRACER") != 0)
        return;
    TraceCounts *tc = data;
    const gchar *text = gst_debug_message_get(message);   /* transfer none */
    if (g_str_has_prefix(text, "latency,")) {
        g_atomic_int_inc(&tc->latency_records);
        /* print the first 2 so the learner sees the format */
        if (g_atomic_int_add(&tc->printed, 1) < 2)
            printf("  record: %.150s...\n", text);
    } else {
        g_atomic_int_inc(&tc->stats_records);
    }
}

int main(int argc, char *argv[])
{
    /* Same as: GST_TRACERS="latency;stats" GST_DEBUG=GST_TRACER:7
     * The latency tracer measures source->sink by default; its flags
     * parameter adds per-element numbers: latency(flags=pipeline+element) */
    g_setenv("GST_TRACERS", "latency(flags=pipeline+element);stats", TRUE);
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    static TraceCounts tc;    /* static: outlives the streaming threads */
    if (!g_getenv("GST_DEBUG"))
        (gst_debug_remove_log_function)(NULL);   /* see 01: () skips macro */
    (gst_debug_add_log_function)(on_log, &tc, NULL);
    gst_debug_set_threshold_for_name("GST_TRACER", GST_LEVEL_TRACE);

    const char *desc = display
        ? "videotestsrc num-buffers=60 ! queue ! videoconvert ! autovideosink"
        : "videotestsrc num-buffers=60 ! video/x-raw,width=320,height=240 "
          "! queue ! videoconvert ! fakesink sync=false";
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);   /* we own it */
    CHECK(pipeline && !err, "parse failed");

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);           /* we own a ref */
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND,
                          GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    (gst_debug_remove_log_function)(on_log);
    printf("latency records: %d, other tracer records: %d\n",
           tc.latency_records, tc.stats_records);
    CHECK(tc.latency_records > 0, "latency tracer produced nothing");
    CHECK(tc.stats_records > 0, "stats tracer produced nothing");

    printf("OK: tracers enabled from code, records captured\n");
    gst_deinit();
    return 0;
}
