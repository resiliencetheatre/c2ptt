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
 * sudo apt install libcodec2-dev libasound2-dev
 *
 * Ini file template:
 *
 * [pttkey]
 * enable_keyboard=1
 * keyboard_device=/dev/input/event0
 * ptt_down_type=1
 * ptt_down_code=100
 * ptt_down_value=1
 * ptt_up_type=1
 * ptt_up_code=100
 * ptt_up_value=0
 *
 * enable_socket=1
 * socket_path=/tmp/pttkey.sock
 *
 * output_dir=/tmp/c2out
 *
 * Socket commands:
 *   ptt_down
 *   ptt_up
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
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <limits.h>

#include <alsa/asoundlib.h>
#include <codec2/codec2.h>

#include "log.h"
#include "ini.h"

#define DEVICE      "default"
#define SAMPLE_RATE 8000
#define CHANNELS    1

struct c2_header {
    char     magic[4];
    uint32_t mode;
    uint32_t bits_per_frame;
    uint32_t samples_per_frame;
};

static volatile sig_atomic_t g_running = 1;
static char g_socket_path[108] = {0};
static int g_socket_fd = -1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static void cleanup_socket(void)
{
    if (g_socket_fd != -1) {
        close(g_socket_fd);
        g_socket_fd = -1;
    }

    if (g_socket_path[0] != '\0') {
        unlink(g_socket_path);
        g_socket_path[0] = '\0';
    }
}

static int record_frame(snd_pcm_t *handle, short *buf, int frames)
{
    int got = 0;
    while (got < frames) {
        int r = snd_pcm_readi(handle, buf + got, frames - got);
        if (r == -EPIPE) {
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

static void make_timestamp_filename(char *buf, size_t len)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "rec-%Y%m%d-%H%M%S.c2", &tm);
}

static int parse_ini_int(const char *s, int defval)
{
    if (!s || !*s)
        return defval;
    return atoi(s);
}

static int ensure_directory_exists(const char *path)
{
    struct stat st;

    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }

    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        return 0;
    }

    if (errno != ENOENT)
        return -1;

    if (mkdir(path, 0775) < 0)
        return -1;

    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    int in_fd = -1, out_fd = -1;
    char buf[8192];
    ssize_t r;

    in_fd = open(src, O_RDONLY);
    if (in_fd < 0)
        return -1;

    out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (out_fd < 0) {
        close(in_fd);
        return -1;
    }

    while ((r = read(in_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out_fd, buf + off, r - off);
            if (w < 0) {
                close(in_fd);
                close(out_fd);
                return -1;
            }
            off += w;
        }
    }

    if (r < 0) {
        close(in_fd);
        close(out_fd);
        return -1;
    }

    if (fsync(out_fd) < 0) {
        close(in_fd);
        close(out_fd);
        return -1;
    }

    close(in_fd);
    close(out_fd);
    return 0;
}

static int move_file_to_dir(const char *src_path, const char *output_dir)
{
    char dst_path[PATH_MAX];
    const char *base;
    const char *slash;

    if (!src_path || !*src_path || !output_dir || !*output_dir) {
        errno = EINVAL;
        return -1;
    }

    slash = strrchr(src_path, '/');
    base = slash ? slash + 1 : src_path;

    if (snprintf(dst_path, sizeof(dst_path), "%s/%s", output_dir, base) >= (int)sizeof(dst_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (rename(src_path, dst_path) == 0)
        return 0;

    if (errno != EXDEV)
        return -1;

    if (copy_file(src_path, dst_path) < 0)
        return -1;

    if (unlink(src_path) < 0)
        return -1;

    return 0;
}

static int create_control_socket(const char *path)
{
    int fd;
    struct sockaddr_un addr;

    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }

    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    return fd;
}

static void strtrim(char *s)
{
    char *p;
    size_t len;

    if (!s || !*s)
        return;

    while (isspace((unsigned char)*s))
        memmove(s, s + 1, strlen(s));

    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }

    p = strchr(s, '\r');
    if (p) *p = '\0';
    p = strchr(s, '\n');
    if (p) *p = '\0';
}

static void stop_recording(bool *recording,
                           FILE **fout,
                           snd_pcm_t *pcm,
                           long *frames_recorded,
                           char *current_filename,
                           const char *output_dir)
{
    if (*recording && *fout) {
        fflush(*fout);
        fclose(*fout);
        *fout = NULL;
        *recording = false;

        log_info("[%d] Stopped recording, codec2_frames=%ld", getpid(), *frames_recorded);

        snd_pcm_drop(pcm);
        snd_pcm_prepare(pcm);

        if (current_filename && *current_filename && output_dir && *output_dir) {
            if (move_file_to_dir(current_filename, output_dir) < 0) {
                log_error("[%d] Failed to move %s to %s: %s",
                          getpid(), current_filename, output_dir, strerror(errno));
            } else {
                log_info("[%d] Moved file to %s", getpid(), output_dir);
                current_filename[0] = '\0';
            }
        }
    }
}

static void start_recording(bool *recording,
                            FILE **fout,
                            snd_pcm_t *pcm,
                            int mode,
                            int bits_per_frame,
                            int samples_per_frame,
                            long *frames_recorded,
                            char *current_filename,
                            size_t current_filename_len)
{
    char filename[64];
    struct c2_header hdr;

    if (*recording)
        return;

    snd_pcm_drop(pcm);
    snd_pcm_prepare(pcm);

    make_timestamp_filename(filename, sizeof(filename));

    if (current_filename && current_filename_len > 0)
        snprintf(current_filename, current_filename_len, "%s", filename);

    *fout = fopen(filename, "wb");
    if (!*fout) {
        log_error("[%d] fopen(%s) failed: %s",
                  getpid(), filename, strerror(errno));
        if (current_filename && current_filename_len > 0)
            current_filename[0] = '\0';
        return;
    }

    memcpy(hdr.magic, "C2BF", 4);
    hdr.mode              = (uint32_t)mode;
    hdr.bits_per_frame    = (uint32_t)bits_per_frame;
    hdr.samples_per_frame = (uint32_t)samples_per_frame;

    if (fwrite(&hdr, sizeof(hdr), 1, *fout) != 1) {
        log_error("[%d] Failed to write header to %s", getpid(), filename);
        fclose(*fout);
        *fout = NULL;
        if (current_filename && current_filename_len > 0)
            current_filename[0] = '\0';
        return;
    }

    *recording = true;
    *frames_recorded = 0;
    log_info("[%d] Started recording to %s", getpid(), filename);
}

static void handle_ptt_down(int *state,
                            bool *recording,
                            FILE **fout,
                            snd_pcm_t *pcm,
                            int mode,
                            int bits_per_frame,
                            int samples_per_frame,
                            long *frames_recorded,
                            char *current_filename,
                            size_t current_filename_len)
{
    if (*state == 0) {
        log_info("[%d] PTT down", getpid());
        *state = 1;
    }

    if (!*recording) {
        start_recording(recording, fout, pcm, mode,
                        bits_per_frame, samples_per_frame, frames_recorded,
                        current_filename, current_filename_len);
    }
}

static void handle_ptt_up(int *state,
                          bool *recording,
                          FILE **fout,
                          snd_pcm_t *pcm,
                          long *frames_recorded,
                          char *current_filename,
                          const char *output_dir)
{
    if (*state == 1) {
        log_info("[%d] PTT up", getpid());
        *state = 0;
    }

    stop_recording(recording, fout, pcm, frames_recorded,
                   current_filename, output_dir);
}

static void process_socket_command(const char *cmd,
                                   int *state,
                                   bool *recording,
                                   FILE **fout,
                                   snd_pcm_t *pcm,
                                   int mode,
                                   int bits_per_frame,
                                   int samples_per_frame,
                                   long *frames_recorded,
                                   char *current_filename,
                                   size_t current_filename_len,
                                   const char *output_dir)
{
    if (!cmd)
        return;

    if (strcmp(cmd, "ptt_down") == 0) {
        handle_ptt_down(state, recording, fout, pcm, mode,
                        bits_per_frame, samples_per_frame, frames_recorded,
                        current_filename, current_filename_len);
    } else if (strcmp(cmd, "ptt_up") == 0) {
        handle_ptt_up(state, recording, fout, pcm, frames_recorded,
                      current_filename, output_dir);
    } else {
        log_error("[%d] Unknown socket command: '%s'", getpid(), cmd);
    }
}

int main(int argc, char *argv[])
{
    int opt;
    int state = 0;
    int rcode = 0;
    int err;
    char keyboard_name[256] = "Unknown";

    int keyboard_fd = -1;
    struct input_event keyboard_event;

    char *keyboard_device = NULL;
    char *ptt_down_command = NULL;
    char *ptt_up_command   = NULL;
    char *ptt_down_type_s = NULL;
    char *ptt_down_code_s = NULL;
    char *ptt_down_value_s = NULL;
    char *ptt_up_type_s = NULL;
    char *ptt_up_code_s = NULL;
    char *ptt_up_value_s = NULL;
    char *socket_path = NULL;
    char *enable_keyboard_s = NULL;
    char *enable_socket_s = NULL;
    char *output_dir = NULL;
    char current_filename[PATH_MAX] = {0};

    int enable_keyboard = 1;
    int enable_socket = 0;
    int ptt_down_type, ptt_down_code, ptt_down_value;
    int ptt_up_type, ptt_up_code, ptt_up_value;

    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params;
    struct CODEC2 *c2 = NULL;
    short *speech = NULL;
    unsigned char *bits = NULL;

    bool recording = false;
    FILE *fout = NULL;
    long frames_recorded = 0;

    log_set_level(LOG_INFO);
    log_set_quiet(1);

    while ((opt = getopt(argc, argv, "dlh")) != -1) {
        switch (opt) {
        case 'h':
            fprintf(stderr, "\npttkey_rec\n");
            fprintf(stderr, "\n Options: -l enable logging\n");
            fprintf(stderr, "          -d set log level to debug\n\n");
            exit(0);
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

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);

    ini_t *config = ini_load("pttkey.ini");
    if (config == NULL) {
        log_error("[%d] Cannot find pttkey.ini, aborting.", getpid());
        exit(1);
    }

    ini_sget(config, "pttkey", "keyboard_device", NULL, &keyboard_device);
    ini_sget(config, "pttkey", "ptt_down_command", NULL, &ptt_down_command);
    ini_sget(config, "pttkey", "ptt_up_command", NULL, &ptt_up_command);
    ini_sget(config, "pttkey", "ptt_down_type", NULL, &ptt_down_type_s);
    ini_sget(config, "pttkey", "ptt_down_code", NULL, &ptt_down_code_s);
    ini_sget(config, "pttkey", "ptt_down_value", NULL, &ptt_down_value_s);
    ini_sget(config, "pttkey", "ptt_up_type", NULL, &ptt_up_type_s);
    ini_sget(config, "pttkey", "ptt_up_code", NULL, &ptt_up_code_s);
    ini_sget(config, "pttkey", "ptt_up_value", NULL, &ptt_up_value_s);
    ini_sget(config, "pttkey", "socket_path", NULL, &socket_path);
    ini_sget(config, "pttkey", "enable_keyboard", NULL, &enable_keyboard_s);
    ini_sget(config, "pttkey", "enable_socket", NULL, &enable_socket_s);
    ini_sget(config, "pttkey", "output_dir", NULL, &output_dir);

    enable_keyboard = parse_ini_int(enable_keyboard_s, 1);
    enable_socket   = parse_ini_int(enable_socket_s, 0);

    ptt_down_type  = parse_ini_int(ptt_down_type_s, 1);
    ptt_down_code  = parse_ini_int(ptt_down_code_s, 100);
    ptt_down_value = parse_ini_int(ptt_down_value_s, 1);
    ptt_up_type    = parse_ini_int(ptt_up_type_s, 1);
    ptt_up_code    = parse_ini_int(ptt_up_code_s, 100);
    ptt_up_value   = parse_ini_int(ptt_up_value_s, 0);

    if (!enable_keyboard && !enable_socket) {
        log_error("[%d] Both enable_keyboard and enable_socket are disabled", getpid());
        ini_free(config);
        exit(1);
    }

    if (!output_dir || !*output_dir) {
        log_error("[%d] output_dir not set in pttkey.ini", getpid());
        ini_free(config);
        exit(1);
    }

    if (ensure_directory_exists(output_dir) < 0) {
        log_error("[%d] Cannot use output_dir %s: %s",
                  getpid(), output_dir, strerror(errno));
        ini_free(config);
        exit(1);
    }

    if (enable_keyboard) {
        if (!keyboard_device || !*keyboard_device) {
            log_error("[%d] enable_keyboard=1 but keyboard_device not set", getpid());
            ini_free(config);
            exit(1);
        }

        keyboard_fd = open(keyboard_device, O_RDONLY | O_NONBLOCK);
        if (keyboard_fd == -1) {
            log_error("[%d] Failed to open keyboard %s. Are you in input group?",
                      getpid(), keyboard_device);
            ini_free(config);
            exit(1);
        }

        rcode = ioctl(keyboard_fd, EVIOCGNAME(sizeof(keyboard_name)), keyboard_name);
        (void)rcode;
        log_debug("[%d] Monitoring %s", getpid(), keyboard_name);
        log_info("[%d] Keyboard open %s", getpid(), keyboard_device);
    }

    if (enable_socket) {
        if (!socket_path || !*socket_path)
            socket_path = "/tmp/pttkey.sock";

        snprintf(g_socket_path, sizeof(g_socket_path), "%s", socket_path);
        g_socket_fd = create_control_socket(g_socket_path);
        if (g_socket_fd < 0) {
            log_error("[%d] Failed to create control socket %s: %s",
                      getpid(), g_socket_path, strerror(errno));
            if (keyboard_fd != -1)
                close(keyboard_fd);
            ini_free(config);
            exit(1);
        }

        log_info("[%d] Socket control listening on %s", getpid(), g_socket_path);
    }

    int mode = CODEC2_MODE_2400;
    c2 = codec2_create(mode);
    if (!c2) {
        log_error("[%d] codec2_create failed", getpid());
        cleanup_socket();
        if (keyboard_fd != -1) close(keyboard_fd);
        ini_free(config);
        exit(1);
    }

    int samples_per_frame = codec2_samples_per_frame(c2);
    int bits_per_frame    = codec2_bits_per_frame(c2);
    int bytes_per_frame   = (bits_per_frame + 7) / 8;

    speech = malloc(samples_per_frame * sizeof(short));
    bits   = malloc(bytes_per_frame);
    if (!speech || !bits) {
        log_error("[%d] malloc failed", getpid());
        codec2_destroy(c2);
        cleanup_socket();
        if (keyboard_fd != -1) close(keyboard_fd);
        ini_free(config);
        exit(1);
    }

    if ((err = snd_pcm_open(&pcm, DEVICE, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        log_error("[%d] snd_pcm_open (capture) failed: %s", getpid(), snd_strerror(err));
        free(speech);
        free(bits);
        codec2_destroy(c2);
        cleanup_socket();
        if (keyboard_fd != -1) close(keyboard_fd);
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
        snd_pcm_close(pcm);
        free(speech);
        free(bits);
        codec2_destroy(c2);
        cleanup_socket();
        if (keyboard_fd != -1) close(keyboard_fd);
        ini_free(config);
        exit(1);
    }

    snd_pcm_uframes_t period_size = samples_per_frame;
    snd_pcm_hw_params_set_period_size_near(pcm, params, &period_size, NULL);

    if ((err = snd_pcm_hw_params(pcm, params)) < 0) {
        log_error("[%d] snd_pcm_hw_params (capture) failed: %s", getpid(), snd_strerror(err));
        snd_pcm_close(pcm);
        free(speech);
        free(bits);
        codec2_destroy(c2);
        cleanup_socket();
        if (keyboard_fd != -1) close(keyboard_fd);
        ini_free(config);
        exit(1);
    }

    log_info("[%d] ALSA capture using sample rate: %u Hz", getpid(), rate);
    log_info("[%d] Codec2: samples/frame=%d, bits/frame=%d, bytes/frame=%d",
             getpid(), samples_per_frame, bits_per_frame, bytes_per_frame);

    while (g_running) {
        struct pollfd pfds[2];
        nfds_t nfds = 0;
        int poll_timeout = recording ? 0 : 50;

        if (keyboard_fd != -1) {
            pfds[nfds].fd = keyboard_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        if (g_socket_fd != -1) {
            pfds[nfds].fd = g_socket_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        if (nfds > 0) {
            int pr = poll(pfds, nfds, poll_timeout);
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                log_error("[%d] poll failed: %s", getpid(), strerror(errno));
                break;
            }
        } else {
            usleep(50000);
        }

        if (keyboard_fd != -1) {
            while (read(keyboard_fd, &keyboard_event, sizeof(keyboard_event)) == sizeof(keyboard_event)) {
                if (state == 0 &&
                    keyboard_event.type  == ptt_down_type &&
                    keyboard_event.code  == ptt_down_code &&
                    keyboard_event.value == ptt_down_value) {

                    handle_ptt_down(&state, &recording, &fout, pcm, mode,
                                    bits_per_frame, samples_per_frame, &frames_recorded,
                                    current_filename, sizeof(current_filename));
                }

                if (state == 1 &&
                    keyboard_event.type  == ptt_up_type &&
                    keyboard_event.code  == ptt_up_code &&
                    keyboard_event.value == ptt_up_value) {

                    handle_ptt_up(&state, &recording, &fout, pcm, &frames_recorded,
                                  current_filename, output_dir);
                }
            }
        }

        if (g_socket_fd != -1) {
            for (;;) {
                int cfd;
                char buf[128];
                ssize_t n;

                cfd = accept(g_socket_fd, NULL, NULL);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    log_error("[%d] accept failed: %s", getpid(), strerror(errno));
                    break;
                }

                n = read(cfd, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    strtrim(buf);
                    process_socket_command(buf, &state, &recording, &fout, pcm, mode,
                                           bits_per_frame, samples_per_frame, &frames_recorded,
                                           current_filename, sizeof(current_filename), output_dir);
                }

                close(cfd);
            }
        }

        if (recording && fout) {
            if ((err = record_frame(pcm, speech, samples_per_frame)) < 0) {
                log_error("[%d] record_frame failed, stopping recording", getpid());
                stop_recording(&recording, &fout, pcm, &frames_recorded,
                               current_filename, output_dir);
                continue;
            }

            codec2_encode(c2, bits, speech);

            if (fwrite(bits, bytes_per_frame, 1, fout) != 1) {
                log_error("[%d] fwrite failed, stopping recording", getpid());
                stop_recording(&recording, &fout, pcm, &frames_recorded,
                               current_filename, output_dir);
                continue;
            }

            frames_recorded++;
        }
    }

    log_info("[%d] Exiting", getpid());

    if (recording && fout)
        stop_recording(&recording, &fout, pcm, &frames_recorded,
                       current_filename, output_dir);

    if (keyboard_fd != -1)
        close(keyboard_fd);

    if (pcm)
        snd_pcm_close(pcm);

    if (c2)
        codec2_destroy(c2);

    free(speech);
    free(bits);
    cleanup_socket();
    ini_free(config);

    return 0;
}
