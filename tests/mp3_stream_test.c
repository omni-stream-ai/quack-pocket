#include "mp3_stream.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef void *lame_t;
extern lame_t lame_init(void);
extern int lame_set_in_samplerate(lame_t, int);
extern int lame_set_out_samplerate(lame_t, int);
extern int lame_set_num_channels(lame_t, int);
extern int lame_init_params(lame_t);
extern int lame_encode_buffer(lame_t, short *, short *, int, unsigned char *, int);
extern int lame_encode_flush(lame_t, unsigned char *, int);
extern int lame_close(lame_t);

static void test(int hz, int channels) {
    FILE *file = tmpfile();
    assert(file);
    const unsigned char header[] = {'I','D','3',4,0,0,0,1,0,0};
    fwrite(header, 1, sizeof(header), file);
    fseek(file, 16384, SEEK_CUR);
    lame_t encoder = lame_init();
    assert(encoder);
    lame_set_in_samplerate(encoder, hz);
    lame_set_out_samplerate(encoder, hz);
    lame_set_num_channels(encoder, channels);
    assert(lame_init_params(encoder) == 0);
    short left[1024], right[1024];
    unsigned char out[8192];
    for (int frame = 0; frame < 100; ++frame) {
        for (int i = 0; i < 1024; ++i) {
            left[i] = 12000 * sin(2*M_PI*440*(frame*1024+i)/hz);
            right[i] = left[i]/2;
        }
        int n = lame_encode_buffer(encoder, left, right, 1024, out, sizeof(out));
        assert(n >= 0);
        assert(fwrite(out, 1, n, file) == (size_t)n);
    }
    int n = lame_encode_flush(encoder, out, sizeof(out));
    assert(n >= 0);
    fwrite(out, 1, n, file);
    lame_close(encoder);
    rewind(file);
    mp3_stream_t *s = mp3_stream_open(file);
    assert(s);
    int count, rate, total = 0, peak = 0;
    const int16_t *mono;
    long bookmark = 0;
    int remaining = 0;
    while ((count = mp3_stream_read(s, &mono, &rate)) > 0) {
        assert(rate == hz);
        total += count;
        if (bookmark) remaining += count;
        else if (total >= 24000) bookmark = mp3_stream_position(s);
        for (int i=0; i<count; ++i) if (abs(mono[i])>peak) peak = abs(mono[i]);
    }
    assert(count == 0 && total >= 102400 && total < 106000 && peak > 5000 && peak < 16000);
    printf("passed %d Hz, %d channel(s), %d decoded samples, peak %d\n",hz,channels,total,peak);
    mp3_stream_close(s);
    rewind(file);
    s = mp3_stream_open(file);
    assert(s && bookmark > 16384 && fseek(file,bookmark,SEEK_SET) == 0);
    int resumed = 0;
    while ((count = mp3_stream_read(s,&mono,&rate)) > 0) resumed += count;
    assert(count == 0 && resumed <= remaining && remaining - resumed < 4608);
    mp3_stream_close(s);
    fclose(file);
}
int main(void) {
    FILE *empty = tmpfile();
    assert(empty);
    errno = 0;
    assert(mp3_stream_open(empty) == NULL && errno == ENODATA);
    fclose(empty);
    test(44100, 2);
    test(16000, 1);
    test(48000, 2);
}
