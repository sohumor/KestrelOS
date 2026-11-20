#include <kestrel.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char buf[32];
    int audio = open("/dev/audio", O_WRONLY);
    int mixer = open("/dev/mixer", O_WRONLY);
    if (audio < 0 || mixer < 0)
        return 1;
    int second = open("/dev/audio", O_WRONLY);
    if (second >= 0) {
        close(second);
        return 1;
    }
    if (write(audio, buf, 0) != 0 || write(audio, buf, 1) != -1 ||
        write(mixer, "", 0) != 0 || write(mixer, "101", 3) != -1 ||
        write(mixer, "-1", 2) != -1 || write(mixer, "bad", 3) != -1 ||
        write(mixer, "3x", 2) != -1 || write(mixer, "", 1) != -1 ||
        write(mixer, "0\n", 2) != 2 || write(mixer, "100", 3) != 3)
        return 1;
    close(mixer);
    close(audio);
    mixer = open("/dev/mixer", O_RDONLY);
    if (mixer < 0)
        return 1;
    memset(buf, 0, sizeof(buf));
    if (read(mixer, buf, sizeof(buf) - 1) != 11 ||
        strcmp(buf, "volume=100\n") != 0 || read(mixer, buf, 1) != 0 ||
        write(mixer, "50", 2) != -1)
        return 1;
    close(mixer);
    audio = open("/dev/audio", O_RDONLY);
    if (audio < 0 || read(audio, buf, sizeof(buf)) != 0)
        return 1;
    close(audio);
    puts("audio: empty, invalid, exclusive-open and mixer checks passed");
    return 0;
}
