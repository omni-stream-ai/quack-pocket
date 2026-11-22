#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mp3_stream mp3_stream_t;

/* The caller owns the FILE and the PCM remains valid until the next read. */
mp3_stream_t *mp3_stream_open(FILE *file);
int mp3_stream_read(mp3_stream_t *stream, const int16_t **mono, int *rate);
void mp3_stream_close(mp3_stream_t *stream);

#ifdef __cplusplus
}
#endif
