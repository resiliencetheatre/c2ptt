/*
 * play_spool - monitor for Codec2 .c2 files, play them, then delete
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
 * sudo apt install libcodec2-dev libasound2-dev
 * 
 * Usage: ./play_spool [directory]
 * 
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <alsa/asoundlib.h>
#include <codec2/codec2.h>

#define DEVICE "default"      // you can also try "plughw:0,0"
#define SAMPLE_RATE 8000
#define CHANNELS 1

struct c2_header {
    char     magic[4];           // "C2BF"
    uint32_t mode;
    uint32_t bits_per_frame;
    uint32_t samples_per_frame;
};

static int play_frame(snd_pcm_t *handle, short *buf, int frames) {
    int done = 0;
    while (done < frames) {
        int r = snd_pcm_writei(handle, buf + done, frames - done);
        if (r == -EPIPE) {
            // underrun
            snd_pcm_prepare(handle);
            continue;
        } else if (r < 0) {
            fprintf(stderr, "snd_pcm_writei error: %s\n", snd_strerror(r));
            return r;
        }
        done += r;
    }
    return 0;
}

/* Play one .c2 file end-to-end, then return 0 on success, non-zero on error */
static int play_c2_file(const char *filename) {
    int ret = 0;
    FILE *fin = NULL;
    snd_pcm_t *handle = NULL;
    struct CODEC2 *c2 = NULL;
    short *speech = NULL;
    unsigned char *bits = NULL;

    fin = fopen(filename, "rb");
    if (!fin) {
        perror("fopen");
        return 1;
    }

    // ---- Read header ----
    struct c2_header hdr;
    if (fread(&hdr, sizeof(hdr), 1, fin) != 1) {
        fprintf(stderr, "Failed to read header from %s\n", filename);
        ret = 1;
        goto done;
    }

    if (memcmp(hdr.magic, "C2BF", 4) != 0) {
        fprintf(stderr, "Invalid file format (bad magic) in %s\n", filename);
        ret = 1;
        goto done;
    }

    int mode              = (int)hdr.mode;
    int bits_per_frame    = (int)hdr.bits_per_frame;
    int samples_per_frame = (int)hdr.samples_per_frame;
    int bytes_per_frame   = (bits_per_frame + 7) / 8;

    printf("Playing file: %s\n", filename);
    printf("  mode: %d\n", mode);
    printf("  bits/frame: %d, bytes/frame: %d\n", bits_per_frame, bytes_per_frame);
    printf("  samples/frame: %d @ %d Hz\n", samples_per_frame, SAMPLE_RATE);

    // ---- Codec2 init ----
    c2 = codec2_create(mode);
    if (!c2) {
        fprintf(stderr, "codec2_create failed\n");
        ret = 1;
        goto done;
    }

    speech = malloc(samples_per_frame * sizeof(short));
    bits   = malloc(bytes_per_frame);
    if (!speech || !bits) {
        fprintf(stderr, "malloc failed\n");
        ret = 1;
        goto done;
    }

    // ---- ALSA playback init ----
    snd_pcm_hw_params_t *params;
    int err;

    if ((err = snd_pcm_open(&handle, DEVICE, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "snd_pcm_open (playback) failed: %s\n", snd_strerror(err));
        ret = 1;
        goto done;
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle, params);
    snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(handle, params, CHANNELS);

    int enable_resample = 1;
    snd_pcm_hw_params_set_rate_resample(handle, params, enable_resample);

    unsigned int rate = SAMPLE_RATE; // 8000
    snd_pcm_hw_params_set_rate_near(handle, params, &rate, NULL);

    if (rate != SAMPLE_RATE) {
        fprintf(stderr,
                "Error: ALSA could not set requested sample rate %d Hz, got %u Hz\n",
                SAMPLE_RATE, rate);
        fprintf(stderr, "This will break Codec2 timing, aborting.\n");
        ret = 1;
        goto done;
    }

    snd_pcm_uframes_t period_size = samples_per_frame;
    snd_pcm_hw_params_set_period_size_near(handle, params, &period_size, NULL);

    if ((err = snd_pcm_hw_params(handle, params)) < 0) {
        fprintf(stderr, "snd_pcm_hw_params (playback) failed: %s\n", snd_strerror(err));
        ret = 1;
        goto done;
    }

    printf("Playback using sample rate: %u Hz\n", rate);

    // ---- Playback loop ----
    for (;;) {
        size_t n = fread(bits, bytes_per_frame, 1, fin);
        if (n != 1) {
            if (feof(fin)) {
                break;
            } else {
                perror("fread");
                ret = 1;
                break;
            }
        }

        codec2_decode(c2, speech, bits);

        if ((err = play_frame(handle, speech, samples_per_frame)) < 0) {
            fprintf(stderr, "play_frame failed\n");
            ret = 1;
            break;
        }
    }

    snd_pcm_drain(handle);

done:
    if (handle) {
        snd_pcm_close(handle);
    }
    if (c2) {
        codec2_destroy(c2);
    }
    free(speech);
    free(bits);
    if (fin) {
        fclose(fin);
    }

    return ret;
}

/* Return 1 if name ends with ".c2", else 0 */
static int has_c2_extension(const char *name) {
    size_t len = strlen(name);
    if (len < 4) return 0;
    return (strcmp(name + len - 3, ".c2") == 0);
}

/* Find the oldest *.c2 file that looks "complete" in dirpath.
 * "Complete" here means: mtime at least 1 second in the past.
 * Returns 1 and writes the path into out_path if found, else 0.
 */
static int find_oldest_ready_c2(const char *dirpath, char *out_path, size_t out_len) {
    DIR *dir = opendir(dirpath);
    if (!dir) {
        perror("opendir");
        return 0;
    }

    struct dirent *de;
    time_t now = time(NULL);
    time_t oldest_mtime = 0;
    char candidate[PATH_MAX] = {0};

    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (!has_c2_extension(de->d_name))
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        /* Consider it ready if it's older than 1 second */
        if ((now - st.st_mtime) < 1)
            continue;

        if (candidate[0] == '\0' || st.st_mtime < oldest_mtime) {
            oldest_mtime = st.st_mtime;
            snprintf(candidate, sizeof(candidate), "%s", path);
        }
    }

    closedir(dir);

    if (candidate[0] == '\0')
        return 0;

    snprintf(out_path, out_len, "%s", candidate);
    return 1;
}


int main(int argc, char *argv[]) {
    const char *dirpath = ".";

    if (argc >= 2) {
        dirpath = argv[1];
    }

    printf("play_spool: watching directory %s for *.c2 files\n", dirpath);

    char path[PATH_MAX];

    for (;;) {
        if (find_oldest_ready_c2(dirpath, path, sizeof(path))) {
            printf("Found file to play: %s\n", path);

            int r = play_c2_file(path);
            if (r == 0) {
                printf("Finished playing %s, deleting...\n", path);
                if (unlink(path) != 0) {
                    perror("unlink");
                }
            } else {
                fprintf(stderr, "Error playing %s (keeping file)\n", path);
            }
        } else {
            /* No ready files, sleep a bit */
            usleep(100000); // 100 ms
        }
    }

    return 0;
}
