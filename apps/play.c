#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef PLAY_HOST_TEST
#include <kestrel.h>
#include "gui.h"
#endif

struct wave_pcm {
    const uint8_t *data;
    uint32_t rate, frames;
    uint16_t channels, bits;
    uint64_t output_frames;
};

static uint16_t le16(const uint8_t *p)
{
    return p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | ((uint32_t)le16(p + 2) << 16);
}

static const char *parse_wave(const uint8_t *buf, unsigned long size,
                               struct wave_pcm *pcm)
{
    memset(pcm, 0, sizeof(*pcm));
    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
        return "Not a RIFF/WAVE file";
    uint64_t end = (uint64_t)le32(buf + 4) + 8;
    if (end > size || end < 12)
        return "Truncated WAV file";
    uint32_t data_size = 0;
    int have_fmt = 0;
    for (uint64_t off = 12; off < end; ) {
        if (end - off < 8)
            return "Truncated WAV chunk";
        uint32_t n = le32(buf + off + 4);
        uint64_t next = off + 8 + n + (n & 1);
        if (next > end)
            return "Truncated WAV chunk";
        const uint8_t *p = buf + off + 8;
        if (!memcmp(buf + off, "fmt ", 4)) {
            if (have_fmt || n < 16 || le16(p) != 1)
                return "Only uncompressed PCM WAV is supported";
            pcm->channels = le16(p + 2);
            pcm->rate = le32(p + 4);
            pcm->bits = le16(p + 14);
            if ((pcm->channels != 1 && pcm->channels != 2) ||
                (pcm->bits != 8 && pcm->bits != 16) ||
                pcm->rate < 1000 || pcm->rate > 384000 ||
                le16(p + 12) != pcm->channels * (pcm->bits / 8))
                return "Unsupported PCM format";
            have_fmt = 1;
        } else if (!memcmp(buf + off, "data", 4) && !pcm->data) {
            pcm->data = p;
            data_size = n;
        }
        off = next;
    }
    if (!have_fmt || !pcm->data || !data_size)
        return "WAV has no PCM samples";
    unsigned int stride = pcm->channels * (pcm->bits / 8);
    if (data_size % stride)
        return "Incomplete PCM frame";
    pcm->frames = data_size / stride;
    pcm->output_frames = ((uint64_t)pcm->frames * 48000 + pcm->rate - 1) / pcm->rate;
    return 0;
}

static int sample(const struct wave_pcm *pcm, uint32_t frame, int channel)
{
    if (pcm->channels == 1)
        channel = 0;
    const uint8_t *p = pcm->data + (frame * pcm->channels + channel) * (pcm->bits / 8);
    if (pcm->bits == 8)
        return ((int)*p - 128) * 256;
    return (int16_t)le16(p);
}

static void convert_frame(const struct wave_pcm *pcm, uint64_t frame, int16_t *out)
{
    uint64_t position = frame * pcm->rate;
    uint32_t a = position / 48000;
    uint32_t fraction = position % 48000;
    uint32_t b = a + 1 < pcm->frames ? a + 1 : a;
    for (int channel = 0; channel < 2; channel++) {
        int x = sample(pcm, a, channel), y = sample(pcm, b, channel);
        out[channel] = (int16_t)(((int64_t)x * (48000 - fraction) +
                                  (int64_t)y * fraction) / 48000);
    }
}

#ifndef PLAY_HOST_TEST
static int mixer_read(void)
{
    char line[32];
    int fd = open("/dev/mixer", O_RDONLY);
    if (fd < 0)
        return -1;
    long n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n < 8 || memcmp(line, "volume=", 7))
        return -1;
    line[n] = 0;
    return atoi(line + 7);
}

static int mixer_write(int volume)
{
    char line[8];
    int fd = open("/dev/mixer", O_WRONLY);
    if (fd < 0)
        return -1;
    int n = snprintf(line, sizeof(line), "%d\n", volume);
    long result = write(fd, line, n);
    close(fd);
    return result == n ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *file = "/media/chime.wav", *cwd = "/", *error = 0;
    char path[512], line[160];
    struct k_stat st;
    struct wave_pcm pcm;
    uint8_t *bytes = 0;
    int audio = -1, playing = 0, volume = mixer_read();
    uint64_t sent = 0;
    unsigned long started = 0, refresh_volume = 0;
    gui_ui ui;
    struct k_event ev;
    memset(&ui, 0, sizeof(ui));
    memset(&pcm, 0, sizeof(pcm));
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--cwd=", 6))
            cwd = argv[i] + 6;
        else
            file = argv[i];
    }
    if (file[0] == '/')
        snprintf(path, sizeof(path), "%s", file);
    else
        snprintf(path, sizeof(path), "%s/%s", cwd, file);
    gui_window *win = gui_open("Audio Player", 160, 120, 480, 300, 0);
    if (!win) {
        puts("play: no window manager available");
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0 || stat_(path, &st) < 0 || st.is_dir)
        error = "Could not open WAV file";
    else if (!st.size || st.size > 8 * 1024 * 1024)
        error = "WAV is empty or exceeds 8 MiB";
    else if (!(bytes = malloc(st.size)))
        error = "Not enough memory for WAV";
    else {
        unsigned long done = 0;
        while (done < st.size) {
            long n = read(fd, bytes + done, st.size - done);
            if (n <= 0)
                break;
            done += n;
        }
        error = done == st.size ? parse_wave(bytes, st.size, &pcm) : "Could not read WAV file";
    }
    if (fd >= 0)
        close(fd);
    if (error)
        memset(&pcm, 0, sizeof(pcm));
    if (!error) {
        audio = open("/dev/audio", O_WRONLY);
        if (audio < 0)
            error = "Audio unavailable: no device or already in use";
        else {
            playing = 1;
            started = uptime_ms();
        }
    }
    while (win->open) {
        int got = gui_next_event(win, &ev, playing ? 5 : 100);
        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE || (ev.type == KEV_KEY && ev.key == 27))
                break;
        }
        if (uptime_ms() >= refresh_volume) {
            volume = mixer_read();
            refresh_volume = uptime_ms() + 500;
        }
        gui_clear(win, GUI_SURFACE);
        gui_hgradient(win, gui_mkrc(0, 0, win->w, 58), GUI_PANEL_HI, GUI_PANEL);
        gui_draw_text_aa(win, 20, 16, "Audio Player", FONT_BODY, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
        const char *name = strrchr(path, '/');
        snprintf(line, sizeof(line), "%.48s", name ? name + 1 : path);
        gui_text(win, 20, 78, line, GUI_TEXT, GUI_TRANSPARENT);
        if (pcm.data) {
            snprintf(line, sizeof(line), "%u Hz / %u-bit / %s  ->  48 kHz stereo",
                     pcm.rate, pcm.bits, pcm.channels == 1 ? "mono" : "stereo");
            gui_draw_text_aa(win, 20, 106, line, FONT_SMALL, FONT_REGULAR,
                             FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        }
        int progress = pcm.output_frames ? sent * 100 / pcm.output_frames : 0;
        gui_progress(win, gui_mkrc(20, 143, win->w - 40, 12), progress, GUI_ACCENT);
        snprintf(line, sizeof(line), "%s    %llu.%01llu / %llu.%01llu s",
                 playing ? "Playing" : sent == pcm.output_frames && sent ? "Finished" : "Stopped",
                 (unsigned long long)(sent / 48000), (unsigned long long)(sent / 4800 % 10),
                 (unsigned long long)(pcm.output_frames / 48000),
                 (unsigned long long)(pcm.output_frames / 4800 % 10));
        gui_text(win, 20, 167, line, GUI_TEXT_DIM, GUI_TRANSPARENT);
        if (gui_button_ex(win, gui_mkrc(20, 203, 86, 30), "Play", &ui,
                           pcm.data != 0 && !playing, 0)) {
            audio = open("/dev/audio", O_WRONLY);
            if (audio < 0)
                error = "Audio unavailable: no device or already in use";
            else {
                error = 0;
                sent = 0;
                started = uptime_ms();
                playing = 1;
            }
        }
        if (gui_button_ex(win, gui_mkrc(116, 203, 86, 30), "Stop", &ui, playing, 0)) {
            close(audio);
            audio = -1;
            playing = 0;
        }
        int target = volume;
        gui_draw_text_aa(win, 266, 184, "Volume", FONT_SMALL, FONT_REGULAR,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        if (gui_button_ex(win, gui_mkrc(266, 203, 34, 30), "-", &ui, volume >= 0, 0))
            target = volume < 5 ? 0 : volume - 5;
        snprintf(line, sizeof(line), volume < 0 ? "N/A" : "%d%%", volume);
        gui_text(win, 312, 210, line, GUI_TEXT, GUI_TRANSPARENT);
        if (gui_button_ex(win, gui_mkrc(378, 203, 34, 30), "+", &ui, volume >= 0, 0))
            target = volume > 95 ? 100 : volume + 5;
        if (target != volume && mixer_write(target) == 0)
            volume = target;
        gui_draw_text_aa(win, 20, 264, error ? error : "PCM output / 48 kHz / 16-bit stereo",
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         error ? GUI_WARN : GUI_TEXT_DIM, GUI_TRANSPARENT);
        if (gui_flush(win) != 0)
            break;
        if (playing) {
            int16_t output[1920 * 2];
            uint64_t target_frame = (uint64_t)(uptime_ms() - started + 80) * 48;
            unsigned int count = 0;
            while (sent + count < pcm.output_frames && sent + count < target_frame && count < 1920) {
                convert_frame(&pcm, sent + count, output + count * 2);
                count++;
            }
            if (count && write(audio, output, count * 4) != (long)count * 4) {
                error = "Audio write failed";
                playing = 0;
            }
            sent += count;
            if (sent == pcm.output_frames)
                playing = 0;
            if (!playing) {
                close(audio);
                audio = -1;
            }
        }
    }
    if (audio >= 0)
        close(audio);
    free(bytes);
    gui_close(win);
    return 0;
}
#endif
