#include "music_index.h"

#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static uint32_t read_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

bool music_index_open(music_index_t *index, const char *filename)
{
    if (index->file) {
        return false;
    }

    FILE *file = fopen(filename, "rb");
    if (!file) {
        return false;
    }

    struct stat info;
    unsigned char header[12];
    if (fstat(fileno(file), &info) != 0 || info.st_size < (off_t)sizeof(header) ||
        info.st_size > INT32_MAX ||
        fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "OMI1", 4) != 0) {
        fclose(file);
        return false;
    }

    uint32_t count = read_le32(header + 4);
    uint64_t data_start = 12ULL + 4ULL * count;
    if (data_start > (uint32_t)info.st_size || read_le32(header + 8) != data_start ||
        (count == 0 && data_start != (uint32_t)info.st_size)) {
        fclose(file);
        return false;
    }

    *index = (music_index_t){
        .file = file,
        .count = count,
        .data_start = (uint32_t)data_start,
        .size = (uint32_t)info.st_size,
    };
    return true;
}

void music_index_close(music_index_t *index)
{
    if (index->file) {
        fclose(index->file);
        *index = (music_index_t){0};
    }
}

bool music_index_get(music_index_t *index, uint32_t track, char path[MUSIC_PATH_CAPACITY])
{
    unsigned char offsets[8];
    if (!index->file || track >= index->count ||
        fseek(index->file, 8L + 4L * track, SEEK_SET) != 0 ||
        fread(offsets, 1, sizeof(offsets), index->file) != sizeof(offsets)) {
        return false;
    }

    uint32_t start = read_le32(offsets);
    uint32_t end = read_le32(offsets + 4);
    if (start < index->data_start || end > index->size || end <= start ||
        end - start >= MUSIC_PATH_CAPACITY ||
        (track == index->count - 1 && end != index->size)) {
        return false;
    }

    size_t length = end - start;
    if (fseek(index->file, start, SEEK_SET) != 0 ||
        fread(path, 1, length, index->file) != length) {
        return false;
    }

    path[length] = '\0';
    const char *segment = path;
    for (size_t i = 0; i <= length; ++i) {
        if (i != length && ((unsigned char)path[i] < 32 || path[i] == '\\' || path[i] == ':')) {
            return false;
        }
        if (i == length || path[i] == '/') {
            size_t size = path + i - segment;
            if (size == 0 || (size == 1 && segment[0] == '.') ||
                (size == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            segment = path + i + 1;
        }
    }
    return true;
}

bool music_index_directory(music_index_t *index, const char *dir, uint32_t *first, uint32_t *end)
{
    *first = 0;
    *end = index->count;
    if (!*dir) return true;
    char prefix[MUSIC_PATH_CAPACITY], candidate[MUSIC_PATH_CAPACITY];
    int length = snprintf(prefix, sizeof(prefix), "%s/", dir);
    if (length < 0 || (size_t)length >= sizeof(prefix)) return false;
    for (unsigned upper = 0; upper <= 1; ++upper) {
        uint32_t lo = *first, hi = index->count;
        while (lo < hi) {
            uint32_t mid = lo + (hi-lo)/2;
            if (!music_index_get(index, mid, candidate)) return false;
            int cmp = strncmp(candidate, prefix, length);
            if (cmp < 0 || (upper && cmp == 0)) lo = mid + 1;
            else hi = mid;
        }
        if (upper) *end = lo;
        else *first = lo;
    }
    return true;
}

bool music_index_find(music_index_t *index, const char *relative_path, uint32_t *track)
{
    char candidate[MUSIC_PATH_CAPACITY];
    uint32_t first = 0;
    uint32_t end = index->count;
    while (first < end) {
        uint32_t middle = first + (end - first) / 2;
        if (!music_index_get(index, middle, candidate)) {
            return false;
        }
        int order = strcmp(candidate, relative_path);
        if (order == 0) {
            *track = middle;
            return true;
        }
        if (order < 0) {
            first = middle + 1;
        } else {
            end = middle;
        }
    }
    return false;
}

music_title_result_t music_index_find_title(music_index_t *index, const char *title, uint32_t *track,
                                            bool (*keep_searching)(void *), void *context)
{
    return music_index_find_title_in(index, title, track, 0, index->count, keep_searching, context);
}

music_title_result_t music_index_find_title_in(music_index_t *index, const char *title, uint32_t *track,
                                               uint32_t begin, uint32_t limit,
                                               bool (*keep_searching)(void *), void *context)
{
    if (begin > limit || limit > index->count) return MUSIC_TITLE_INDEX_ERROR;
    size_t wanted = strlen(title);
    if (wanted > 4 && !strcasecmp(title + wanted - 4, ".mp3")) wanted -= 4;
    if (!wanted || !index->file) return MUSIC_TITLE_MISSING;

    enum { batch_size = 128 };
    unsigned char offsets[4 * (batch_size + 1)];
    char path[MUSIC_PATH_CAPACITY];
    bool partial = false;
    bool partial_ambiguous = false;
    bool matched = false;
    uint32_t match = 0;
    for (uint32_t first = begin; first < limit; first += batch_size) {
        if (keep_searching && !keep_searching(context)) return MUSIC_TITLE_CANCELLED;
        uint32_t count = limit - first;
        if (count > batch_size) count = batch_size;
        size_t bytes = 4 * (count + 1);
        if (fseek(index->file, 8L + first * 4L, SEEK_SET) ||
            fread(offsets, 1, bytes, index->file) != bytes) return MUSIC_TITLE_INDEX_ERROR;
        uint32_t start = read_le32(offsets);
        if (start < index->data_start || start >= index->size || fseek(index->file, start, SEEK_SET))
            return MUSIC_TITLE_INDEX_ERROR;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t end = read_le32(offsets + 4 * (i + 1));
            size_t length = end - start;
            if (end <= start || end > index->size || length >= sizeof(path) ||
                fread(path, 1, length, index->file) != length) return MUSIC_TITLE_INDEX_ERROR;
            start = end;
            path[length] = 0;
            char *slash = strrchr(path, '/');
            const char *name = slash ? slash + 1 : path;
            size_t len = strlen(name);
            if (len < 4 || strcasecmp(name + len - 4, ".mp3")) continue;
            size_t stem = len - 4;
            if (stem == wanted && !strncasecmp(name, title, wanted)) {
                if (matched) return MUSIC_TITLE_AMBIGUOUS;
                matched = true;
                match = first + i;
            } else if (!matched && !partial_ambiguous && wanted < stem) {
                for (size_t pos = 0; pos <= stem - wanted; ++pos) {
                    if (!strncasecmp(name + pos, title, wanted)) {
                        partial_ambiguous = partial;
                        partial = true;
                        match = first + i;
                        break;
                    }
                }
            }
        }
    }
    if (!matched && partial_ambiguous) return MUSIC_TITLE_AMBIGUOUS;
    if (!matched && !partial) return MUSIC_TITLE_MISSING;
    *track = match;
    return MUSIC_TITLE_FOUND;
}
