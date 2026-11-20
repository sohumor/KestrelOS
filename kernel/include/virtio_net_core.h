#pragma once
#include <stdint.h>
int virtio_net_frame_len(const uint8_t *buf, uint32_t len, int modern);
