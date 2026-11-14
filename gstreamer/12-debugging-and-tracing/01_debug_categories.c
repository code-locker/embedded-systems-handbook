/*
 * 01_debug_categories.c — your own GST_DEBUG category, thresholds, and a
 *                         log hook that captures every debug line
 *
 * Concept : GStreamer logging is "category x level". Every element/library
 *           area has a named category (videotestsrc, GST_STATES, ...); you set
 *           a threshold per category (GST_DEBUG=2,mycat:6). Here we define
 *           our own category, change thresholds from code, and install a log
 *           function that counts lines, proving which ones got through.
 * Build   : make run P=gstreamer/12-debugging-and-tracing/01_debug_categories.c
 * Read    : README.md  (section "How it works: GST_DEBUG levels and categories")
 *
 *   GST_INFO_OBJECT(...) ──► level >= threshold(category)? ──no──► dropped
 *                                     │ yes                      (nearly free)
 *                                     ▼
 *                       every installed log function is called
 *                       (default one prints to stderr / GST_DEBUG_FILE,
 *                        ours counts lines into struct Counts)
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

/* The category variable. GST_CAT_DEFAULT makes GST_INFO() & co. use it. */
GST_DEBUG_CATEGORY_STATIC(my_cat);
#define GST_CAT_DEFAULT my_cat

typedef struct {
    gint mine_info;     /* INFO lines in our category                  */
    gint mine_debug;    /* DEBUG lines in our category                 */
    gint mine_log;      /* LOG lines in our category (must stay 0)     */
    gint states_lines;    /* lines from core category GST_STATES           */
    gint any;           /* everything that reached us                  */
} Counts;

/* Called from WHATEVER thread logged the line (streaming threads too),
 * so it must be thread-safe: we only use atomic increments. */
static void count_log(GstDebugCategory *cat, GstDebugLevel level,
                      const gchar *file, const gchar *func, gint line,
                      GObject *object, GstDebugMessage *message,
                      gpointer user_data)
{
    (void)file; (void)func; (void)line; (void)object; (void)message;
    Counts *c = user_data;
    g_atomic_int_inc(&c->any);
    const char *name = gst_debug_category_get_name(cat);
    if (strcmp(name, "mydemo") == 0) {
        if (level == GST_LEVEL_INFO)  g_atomic_int_inc(&c->mine_info);
        if (level == GST_LEVEL_DEBUG) g_atomic_int_inc(&c->mine_debug);
        if (level == GST_LEVEL_LOG)   g_atomic_int_inc(&c->mine_log);
    } else if (strcmp(name, "GST_STATES") == 0) {
        g_atomic_int_inc(&c->states_lines);
    }
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* name, color, description. Shows in `gst-launch-1.0 --gst-debug-help`. */
    GST_DEBUG_CATEGORY_INIT(my_cat, "mydemo", 0, "debug lesson demo");

    /* Keep the console quiet: remove the default stderr printer (unless
     * the user explicitly asked for GST_DEBUG output) and add ours. */
    static Counts counts;   /* static: outlives every logging thread */
    /* The names are wrapped in parentheses on purpose: gstinfo.h also
     * defines them as MACROS that compare a function pointer with
     * (void *), which -Wpedantic rejects. "(name)(args)" calls the real
     * function and skips the macro. NULL means "the default printer". */
    if (!g_getenv("GST_DEBUG"))
        (gst_debug_remove_log_function)(NULL);
    (gst_debug_add_log_function)(count_log, &counts, NULL);

    /* Equivalent of GST_DEBUG=1,mydemo:5,GST_STATES:4
     * level 5 = DEBUG, so INFO(4) and DEBUG(5) pass, LOG(6) does not. */
    gst_debug_set_default_threshold(GST_LEVEL_ERROR);
    gst_debug_set_threshold_for_name("mydemo", GST_LEVEL_DEBUG);
    gst_debug_set_threshold_for_name("GST_STATES", GST_LEVEL_INFO);
    CHECK(gst_debug_is_active(), "debug system disabled at build time?");

    GST_INFO("hello from INFO");          /* passes   */
    GST_DEBUG("hello from DEBUG");        /* passes   */
    GST_LOG("hello from LOG");            /* filtered */

    /* Run a pipeline so core categories produce output too. */
    const char *desc = display
        ? "videotestsrc num-buffers=30 ! videoconvert ! autovideosink"
        : "videotestsrc num-buffers=30 ! video/x-raw,width=160,height=120 "
          "! videoconvert ! fakesink sync=false";
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);   /* we own it */
    /* gst_parse_launch may return a (partial) pipeline AND set err. */
    CHECK(pipeline && !err, "parse failed");
    GST_INFO_OBJECT(pipeline, "pipeline created");  /* object-tagged line */

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);           /* we own a ref */
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND,
                          GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    /* Now silence our category completely and log again. */
    gst_debug_set_threshold_for_name("mydemo", GST_LEVEL_NONE);
    GST_ERROR("even ERROR is dropped at threshold NONE");

    (gst_debug_remove_log_function)(count_log);

    printf("mydemo INFO=%d DEBUG=%d LOG=%d, GST_STATES lines=%d, total=%d\n",
           counts.mine_info, counts.mine_debug, counts.mine_log,
           counts.states_lines, counts.any);
    CHECK(counts.mine_info == 2, "expected 2 INFO lines (plain + _OBJECT)");
    CHECK(counts.mine_debug == 1, "expected 1 DEBUG line");
    CHECK(counts.mine_log == 0, "LOG must be filtered at threshold DEBUG");
    CHECK(counts.states_lines > 0, "GST_STATES:4 should have logged something");

    printf("OK: thresholds filter per category; log hooks see every line\n");
    gst_deinit();
    return 0;
}
