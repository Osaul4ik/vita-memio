/*
 * results.h - the results.txt file: append a block, clear it, read it back for
 * the viewer. Plain stdio, so it runs on the console and in the host tests.
 */
#ifndef VITAMEMIO_TESTER_RESULTS_H
#define VITAMEMIO_TESTER_RESULTS_H

#include <stddef.h>

#include "../include/vitamemio.h"

#define RESULTS_PATH      VM_DATA_DIR "/results.txt"
#define RESULTS_LINE_MAX  90
#define RESULTS_LINES_MAX 700

/* Append `text`; creates the file. 0 on success. */
int results_append(const char *path, const char *text);

/* Delete the file. 0 on success, and also when there was none. */
int results_clear(const char *path);

/* Size in bytes, or -1 when the file does not exist. */
long results_size(const char *path);

/* Load the last RESULTS_LINES_MAX lines, wrapped to RESULTS_LINE_MAX columns.
 * Returns the number of lines (0 when empty or missing). */
int results_load(const char *path, char lines[][RESULTS_LINE_MAX], int max);

#endif /* VITAMEMIO_TESTER_RESULTS_H */
