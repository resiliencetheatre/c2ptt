/*
 * pttkey_rec - PTT-controlled Codec2 file recorder
 *
 * Copyright (C) 2025 Resilience Theatre
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 *
 * sudo apt install libcodec2-dev libasound2-dev
 * 
 * Ini file template:
 *
 * [pttkey]
 * keyboard_device=/dev/input/event0
 * ptt_down_type = 1
 * ptt_down_code = 100
 * ptt_down_value = 1
 * ptt_up_type = 1
 * ptt_up_code = 100
 * ptt_up_value = 0
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <linux/input.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <codec2/codec2.h>

#include "log.h"
#include "ini.h"

#define DEVICE      "default"   /* ALSA capture device, "default" or "plughw:0,0" */
#define SAMPLE_RATE 8000
#define CHANNELS    1

struct c2_header {
    char     magic[4];           // "C2BF"
    uint32_t mode;               // Codec2 mode (e.g. CODEC2_MODE_1300)
    uint32_t bits_per_frame;
    uint32_t samples_per_frame;
};

static int record_frame(snd_pcm_t *handle, short *buf, int frames)
{
    int got = 0;
    while (got < frames) {
        int r = snd_pcm_readi(handle, buf + got, frames - got);
        if (r == -EPIPE) {
            // overrun
            snd_pcm_prepare(handle);
            continue;
        } else if (r < 0) {
            fprintf(stderr, "snd_pcm_readi error: %s\n", snd_strerror(r));
            return r;
        }
        got += r;
    }
    return 0;
}

/* Create a timestamped filename like rec-YYYYMMDD-HHMMSS.c2 */
static void make_timestamp_filename(char *buf, size_t len)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "rec-%Y%m%d-%H%M%S.c2", &tm);
}

int main(int argc, char *argv[])
{
    int opt;
    log_set_level(LOG_INFO);
    log_set_quiet(1);

    while ((opt = getopt(argc, argv, "dlh")) != -1) {
        switch (opt) {
        case 'h':
            fprintf(stderr, "\npttkey_rec\n");
            fprintf(stderr, "\n Options: -l enable logging\n");
            fprintf(stderr, "          -d set log level to debug\n\n");
            exit(0);
            break;
        case 'l':
            log_set_quiet(0);
            break;
        case 'd':
            log_set_level(LOG_DEBUG);
            break;
        default:
            break;
        }
    }

    int state = 0;
    int rcode = 0;
    char keyboard_name[256] = "Unknown";
    char *keyboard_device = NULL;
    char *ptt_down_command = NULL; /* unused but kept for ini compatibility */
    char *ptt_up_command   = NULL; /* unused but kept for ini compatibility */
    char *ptt_down_type, *ptt_down_code, *ptt_down_value;
    char *ptt_up_type,   *ptt_up_code,   *ptt_up_value;

    /* Read ini-file */
    ini_t *config = ini_load("pttkey.ini");
    if (config == NULL) {
        log_error("[%d] Cannot find pttkey.ini, aborting.", getpid());
        exit(1);
    }
    ini_sget(config, "pttkey", "keyboard_device", NULL, &keyboard_device);
    ini_sget(config, "pttkey", "ptt_down_command", NULL, &ptt_down_command);
    ini_sget(config, "pttkey", "ptt_up_command", NULL, &ptt_up_command);
    ini_sget(config, "pttkey", "ptt_down_type", NULL, &ptt_down_type);
    ini_sget(config, "pttkey", "ptt_down_code", NULL, &ptt_down_code);
    ini_sget(config, "pttkey", "ptt_down_value", NULL, &ptt_down_value);
    ini_sget(config, "pttkey", "ptt_up_type", NULL, &ptt_up_type);
    ini_sget(config, "pttkey", "ptt_up_code", NULL, &ptt_up_code);
    ini_sget(config, "pttkey", "ptt_up_value", NULL, &ptt_up_value);

    if (!keyboard_device) {
        log_error("[%d] keyboard_device not set in pttkey.ini", getpid());
        ini_free(config);
        exit(1);
    }

    /* Open keyboard */
    int keyboard_fd = open(keyboard_device, O_RDONLY | O_NONBLOCK);
    if (keyboard_fd == -1) {
        log_error("[%d] Failed to open keyboard %s. Are you in input group?",
                  getpid(), keyboard_device);
        ini_free(config);
        exit(1);
    }
    rcode = ioctl(keyboard_fd, EVIOCGNAME(sizeof(keyboard_name)), keyboard_name);
    log_debug("[%d] Monitoring %s (%d)", getpid(), keyboard_name, rcode);
    struct input_event keyboard_event;

    log_info("[%d] Keyboard open %s", getpid(), keyboard_device);

    /* ---- Codec2 init ---- */
    int mode = CODEC2_MODE_2400;
    struct CODEC2 *c2 = codec2_create(mode);
    if (!c2) {
        log_error("[%d] codec2_create failed", getpid());
        close(keyboard_fd);
        ini_free(config);
        exit(1);
    }

    int samples_per_frame = codec2_samples_per_frame(c2);
    int bits_per_frame    = codec2_bits_per_frame(c2);
    int bytes_per_frame   = (bits_per_frame + 7) / 8;

    short *speech = malloc(samples_per_frame * sizeof(short));
    unsigned char *bits = malloc(bytes_per_frame);
    if (!speech || !bits) {
        log_error("[%d] malloc failed", getpid());
        codec2_destroy(c2);
        close(keyboard_fd);
        ini_free(config);
        exit(1);
    }

    /* ---- ALSA capture init ---- */
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params;
    int err;

    if ((err = snd_pcm_open(&pcm, DEVICE, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        log_error("[%d] snd_pcm_open (capture) failed: %s", getpid(), snd_strerror(err));
        free(speech);
        free(bits);
        codec2_destroy(c2);
        close(keyboard_fd);
        ini_free(config);
        exit(1);
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm, params);
    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, params, CHANNELS);

    int enable_resample = 1;
    snd_pcm_hw_params_set_rate_resample(pcm, params, enable_resample);

    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, params, &rate, NULL);
    if (rate != SAMPLE_RATE) {
        log_error("[%d] ALSA could not set requested sample rate %d Hz, got %u Hz",
                  getpid(), SAMPLE_RATE, rate);
        log_error("[%d] This will break Codec2 timing, aborting.", getpid());
        free(speech);
        free(bits);
        codec2_destroy(c2);
        snd_pcm_close(pcm);
        close(keyboard_fd);
        ini_free(config);
        exit(1);
    }

    snd_pcm_uframes_t period_size = samples_per_frame;
    snd_pcm_hw_params_set_period_size_near(pcm, params, &period_size, NULL);

    if ((err = snd_pcm_hw_params(pcm, params)) < 0) {
        log_error("[%d] snd_pcm_hw_params (capture) failed: %s", getpid(), snd_strerror(err));
        free(speech);
        free(bits);
        codec2_destroy(c2);
        snd_pcm_close(pcm);
        close(keyboard_fd);
        ini_free(config);
        exit(1);
    }

    log_info("[%d] ALSA capture using sample rate: %u Hz", getpid(), rate);
    log_info("[%d] Codec2: samples/frame=%d, bits/frame=%d, bytes/frame=%d",
             getpid(), samples_per_frame, bits_per_frame, bytes_per_frame);

    /* ---- Recording state ---- */
    bool recording = false;
    FILE *fout = NULL;
    long frames_recorded = 0;

    while (1) {
        /* Read all available keyboard events (non-blocking) */
        while (read(keyboard_fd, &keyboard_event, sizeof(keyboard_event)) != -1) {
            /* PTT down */
            if (state == 0 &&
                keyboard_event.type  == atoi(ptt_down_type) &&
                keyboard_event.code  == atoi(ptt_down_code) &&
                keyboard_event.value == atoi(ptt_down_value)) {

                log_info("[%d] PTT down", getpid());
                state = 1;

                if (!recording) {
                    /* FLUSH ALSA CAPTURE BUFFER */
                    snd_pcm_drop(pcm);      // stop and drop all pending frames
                    snd_pcm_prepare(pcm);   // restart in a clean state

                    char filename[64];
                    make_timestamp_filename(filename, sizeof(filename));
                    fout = fopen(filename, "wb");
                    if (!fout) {
                        log_error("[%d] fopen(%s) failed: %s",
                                  getpid(), filename, strerror(errno));
                    } else {
                        struct c2_header hdr;
                        memcpy(hdr.magic, "C2BF", 4);
                        hdr.mode              = (uint32_t)mode;
                        hdr.bits_per_frame    = (uint32_t)bits_per_frame;
                        hdr.samples_per_frame = (uint32_t)samples_per_frame;

                        if (fwrite(&hdr, sizeof(hdr), 1, fout) != 1) {
                            log_error("[%d] Failed to write header to %s",
                                      getpid(), filename);
                            fclose(fout);
                            fout = NULL;
                        } else {
                            recording = true;
                            frames_recorded = 0;
                            log_info("[%d] Started recording to %s", getpid(), filename);
                        }
                    }
                }
            }

            /* PTT release */
            if (state == 1 &&
                keyboard_event.type  == atoi(ptt_up_type) &&
                keyboard_event.code  == atoi(ptt_up_code) &&
                keyboard_event.value == atoi(ptt_up_value)) {

                log_info("[%d] PTT up", getpid());
                state = 0;

                if (recording && fout) {
                    fflush(fout);
                    fclose(fout);
                    fout = NULL;
                    recording = false;
                    log_info("[%d] Stopped recording, frames=%ld", getpid(), frames_recorded);

                    /* Optional: reset ALSA  */
                    snd_pcm_drop(pcm);
                    snd_pcm_prepare(pcm);
                }
            }
        }

        if (recording && fout) {
            /* Record one Codec2 frame worth of audio */
            if ((err = record_frame(pcm, speech, samples_per_frame)) < 0) {
                log_error("[%d] record_frame failed, stopping recording", getpid());
                fflush(fout);
                fclose(fout);
                fout = NULL;
                recording = false;
                continue;
            }

            codec2_encode(c2, bits, speech);

            if (fwrite(bits, bytes_per_frame, 1, fout) != 1) {
                log_error("[%d] fwrite failed, stopping recording", getpid());
                fflush(fout);
                fclose(fout);
                fout = NULL;
                recording = false;
                continue;
            }

            frames_recorded += samples_per_frame;
        } else {
            usleep(10000); // 10 ms
        }
    }

    /* Not really reached */
    log_info("[%d] Exiting", getpid());
    rcode = ioctl(keyboard_fd, EVIOCGRAB, 1);
    (void)rcode;
    close(keyboard_fd);
    snd_pcm_close(pcm);
    codec2_destroy(c2);
    free(speech);
    free(bits);
    ini_free(config);
    return 0;
}
