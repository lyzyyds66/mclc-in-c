#ifndef MCLC_PROGRESS_H
#define MCLC_PROGRESS_H

#include <stddef.h>

typedef void (*MclcProgressCallback)(int percent, const char *message,
                                     void *user);

void mclc_progress_set(MclcProgressCallback callback, void *user);
void mclc_progress_clear(void);
void mclc_progress_report(int percent, const char *message);

/* Map per-file 0..100 progress into an overall stage range. */
void mclc_progress_begin_span(int start_percent, int end_percent);
void mclc_progress_end_span(void);
void mclc_progress_report_file(int percent, const char *message);

#endif /* MCLC_PROGRESS_H */
