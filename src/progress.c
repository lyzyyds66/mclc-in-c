#include "progress.h"

/*
 * The launcher runs at most one download/install job at a time, so a simple
 * process-global callback is enough. The GUI clears it when the worker ends.
 */
static MclcProgressCallback g_callback;
static void *g_user;
static int g_span_start;
static int g_span_end;
static int g_span_active;

void mclc_progress_set(MclcProgressCallback callback, void *user)
{
    g_callback = callback;
    g_user = user;
    g_span_active = 0;
}

void mclc_progress_clear(void)
{
    g_callback = NULL;
    g_user = NULL;
    g_span_active = 0;
}

void mclc_progress_begin_span(int start_percent, int end_percent)
{
    if (start_percent < 0) start_percent = 0;
    if (end_percent > 100) end_percent = 100;
    if (end_percent < start_percent) end_percent = start_percent;
    g_span_start = start_percent;
    g_span_end = end_percent;
    g_span_active = 1;
}

void mclc_progress_end_span(void)
{
    g_span_active = 0;
}

void mclc_progress_report_file(int percent, const char *message)
{
    if (g_span_active) {
        int mapped;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        mapped = g_span_start +
                 (g_span_end - g_span_start) * percent / 100;
        mclc_progress_report(mapped, message);
    } else {
        mclc_progress_report(percent, message);
    }
}

void mclc_progress_report(int percent, const char *message)
{
    if (g_callback) {
        g_callback(percent, message ? message : "", g_user);
    }
}
