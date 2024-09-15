#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

#include "log.h"

int main (int argc, char *argv[])
{
    int err;
    int dir;
    FILE *fp;
    char *buffer;
    char *device;
    char *pcm_file;
    int channels = 1;
    int buffer_frames = 128;
    unsigned int rate = 44100;
    snd_pcm_t *capture_handle;
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;

    if (argc < 2) {
        fprintf(stderr, "%s hw:0 2 record.pcm\n", argv[0]);
        fprintf(stderr, "%s hw:Camera 1 record.pcm\n", argv[0]);
        fprintf(stderr, "ffplay -ar 44100 -ac 1 -f s16le -i record.pcm\n");
        exit(1);
    }

    device   = argv[1];
    channels = atoi(argv[2]);
    pcm_file = argv[3];

    if ((fp = fopen(pcm_file, "wb")) == NULL) {
        log_err("cannot open audio file for recording\n");
        exit(1);
    }

    log_info("audio interface opened");
    if ((err = snd_pcm_open(&capture_handle, device, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        log_err("cannot open audio device %s (%s)\n", device, snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
        log_err("cannot allocate hardware parameter structure (%s)\n", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_any(capture_handle, hw_params)) < 0) {
        log_err("cannot initialize hardware parameter structure (%s)\n", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_set_access(capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        log_err("cannot set access type (%s)\n", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_set_format(capture_handle, hw_params, format)) < 0) {
        log_err("cannot set sample format (%s)\n",  snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_set_rate_near(capture_handle, hw_params, &rate, &dir)) < 0) {
        log_err("cannot set sample rate (%s)\n", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params_set_channels(capture_handle, hw_params, channels)) < 0) {
        log_err("cannot set channel count (%s)\n", snd_strerror(err));
        exit(1);
    }

    if ((err = snd_pcm_hw_params(capture_handle, hw_params)) < 0) {
        log_err("cannot set parameters (%s)\n", snd_strerror(err));
        exit(1);
    }

#if 1
    snd_pcm_uframes_t frames;
    unsigned int buffer_time, period_time;

    snd_pcm_hw_params_get_buffer_time_max(hw_params, &buffer_time, &dir);
    log_info("max_buffer_time %dus", buffer_time);

    if (buffer_time > 500000) buffer_time = 500000;
    snd_pcm_hw_params_set_buffer_time_near(capture_handle, hw_params, &buffer_time, &dir);

    snd_pcm_hw_params_get_period_time(hw_params, &period_time, &dir);
    log_info("period_time %d", period_time);

    period_time = 26315;
    snd_pcm_hw_params_set_period_time_near(capture_handle, hw_params, &period_time, &dir);

    snd_pcm_hw_params_get_period_size(hw_params, &frames, &dir);
    log_info("sample %ld", frames);
#endif

    snd_pcm_hw_params_free(hw_params);

    log_info("audio interface prepared");
    if ((err = snd_pcm_prepare(capture_handle)) < 0) {
        log_err("cannot prepare audio interface for use (%s)\n", snd_strerror(err));
        exit(1);
    }

    buffer = malloc(buffer_frames * snd_pcm_format_width(format) / 8 * channels);
    log_info("buffer_frames %d width %d channels %d", buffer_frames, snd_pcm_format_width(format), channels);

    while (1) {
        err = snd_pcm_readi(capture_handle, buffer, buffer_frames);
        if (err == -EPIPE) {
            err = snd_pcm_prepare(capture_handle);
            if (err < 0){
                log_err("snd_pcm_prepare failed, error(%s)", snd_strerror(err));
                exit(1);
            }
        } else if (err < 0) {
            log_err("snd_pcm_readi failed, error(%s)", snd_strerror(err));
            exit(1);
        } else if (err != buffer_frames) {
            log_err("read from audio interface failed (%s)", snd_strerror(err));
            exit(1);
        }

        fwrite(buffer, (buffer_frames), sizeof(short), fp);
        fprintf(stdout, "recording...\r");
    }

    free(buffer);

    log_info("audio interface closed");
    snd_pcm_drain(capture_handle);
    snd_pcm_close(capture_handle);

    fclose(fp);
    exit(0);

    return 0;
}
