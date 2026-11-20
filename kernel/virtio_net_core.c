#include "virtio_net_core.h"

int virtio_net_frame_len(const uint8_t *buf, uint32_t len, int modern)
{
    uint32_t header = modern ? 12 : 10;
    if (len < header + 14 || len > header + 1514 || buf[0] || buf[1])
        return -1;
    return len - header;
}
