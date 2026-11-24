#ifndef OMNI_MUSIC_INDEX_H
#define OMNI_MUSIC_INDEX_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_PATH_CAPACITY 1024

typedef struct {
    FILE *file;
    uint32_t count;
    uint32_t data_start;
    uint32_t size;
} music_index_t;

/* Zero-initialize before opening. Each instance belongs to a single task. */
bool music_index_open(music_index_t *index, const char *filename);
void music_index_close(music_index_t *index);
bool music_index_get(music_index_t *index, uint32_t track, char path[MUSIC_PATH_CAPACITY]);

/* Sorted half-open interval of tracks beneath a relative directory. */
bool music_index_directory(music_index_t *index, const char *dir, uint32_t *first, uint32_t *end);
bool music_index_find(music_index_t *index, const char *relative_path, uint32_t *track);

typedef enum {
    MUSIC_TITLE_FOUND, MUSIC_TITLE_MISSING, MUSIC_TITLE_AMBIGUOUS,
    MUSIC_TITLE_INDEX_ERROR, MUSIC_TITLE_CANCELLED
} music_title_result_t;

/* Visit tracks in index order. A visitor returning false ends the scan successfully. */
music_title_result_t music_index_scan(music_index_t *index, uint32_t begin, uint32_t end,
                                      bool (*visit)(uint32_t, const char *, void *),
                                      bool (*keep_searching)(void *), void *context);
/* Filename match quality: 2 for a whole title, 1 for a substring, 0 otherwise. */
int music_name_match(const char *name, size_t length, const char *title, size_t wanted);

music_title_result_t music_index_find_title_in(music_index_t *index, const char *title, uint32_t *track,
                                               uint32_t begin, uint32_t limit,
                                               bool (*keep_searching)(void *), void *context);
music_title_result_t music_index_find_title(music_index_t *index, const char *title, uint32_t *track,
                                            bool (*keep_searching)(void *), void *context);

#ifdef __cplusplus
}
#endif

#endif
