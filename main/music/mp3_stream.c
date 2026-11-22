#include "mp3_stream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3/minimp3.h"

struct mp3_stream {
    FILE *file;
    mp3dec_t decoder;
    int16_t samples[MINIMP3_MAX_SAMPLES_PER_FRAME];
    unsigned char compressed[8192];
    size_t offset;
    size_t available;
    bool decoded;
};

mp3_stream_t *mp3_stream_open(FILE *file)
{
    unsigned char header[10];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        if (!ferror(file)) errno = ENODATA;
        return NULL;
    }

    long audio_start = 0;
    if (memcmp(header, "ID3", 3) == 0) {
        if (header[3] < 2 || header[3] > 4) {
            errno = ENOTSUP;
            return NULL;
        }
        for (unsigned i = 6; i < sizeof(header); ++i) {
            if (header[i] & 0x80) {
                errno = EINVAL;
                return NULL;
            }
            audio_start = (audio_start << 7) | header[i];
        }
        audio_start += sizeof(header);
        if (header[3] == 4 && (header[5] & 0x10)) {
            audio_start += 10;
        }
    }
    if (fseek(file, audio_start, SEEK_SET) != 0) {
        return NULL;
    }
    mp3_stream_t *stream = calloc(1, sizeof(*stream));
    if (stream) {
        stream->file = file;
        mp3dec_init(&stream->decoder);
    }
    return stream;
}

int mp3_stream_read(mp3_stream_t *stream, const int16_t **mono, int *rate)
{
    size_t skipped = 0;
    while (skipped < 65536) {
        if (stream->available < sizeof(stream->compressed) / 2 && !feof(stream->file)) {
            memmove(stream->compressed, stream->compressed + stream->offset, stream->available);
            stream->offset = 0;
            stream->available += fread(stream->compressed + stream->available, 1,
                                       sizeof(stream->compressed) - stream->available, stream->file);
            if (ferror(stream->file)) {
                return -1;
            }
        }
        if (stream->available == 0) {
            return stream->decoded ? 0 : -1;
        }

        mp3dec_frame_info_t info = {0};
        int count = mp3dec_decode_frame(&stream->decoder, stream->compressed + stream->offset,
                                        (int)stream->available, stream->samples, &info);
        if (info.frame_bytes <= 0 || (size_t)info.frame_bytes > stream->available) {
            return -1;
        }
        stream->available -= info.frame_bytes;
        stream->offset += info.frame_bytes;
        if (count == 0) {
            skipped += info.frame_bytes;
            continue;
        }
        if (info.channels == 2) {
            for (int i = 0; i < count; ++i) {
                stream->samples[i] = ((int)stream->samples[2*i] + stream->samples[2*i+1]) / 2;
            }
        }
        *mono = stream->samples;
        *rate = info.hz;
        stream->decoded = true;
        return count;
    }
    return -1;
}

long mp3_stream_position(mp3_stream_t *stream)
{
    long end = ftell(stream->file);
    return end < 0 ? end : end - (long)stream->available;
}

void mp3_stream_close(mp3_stream_t *stream)
{
    free(stream);
}
