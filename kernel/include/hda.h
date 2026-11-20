#pragma once

#include <stdbool.h>

void hda_init(void);
bool hda_ready(void);
int hda_open(void);
void hda_close(void);
long hda_write(const void *buf, unsigned long n); /* whole stereo s16le frames */
void hda_poll(void);
int hda_volume(void);
int hda_set_volume(int volume);
