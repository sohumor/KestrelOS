#include "../kernel/net.c"
#include "initcall.h"

static int check_receive_queue(void)
{
    struct eth_hdr frame = {0};
    uint64_t drops = net_stats.rx_drops;

    for (unsigned i = 0; i < NET_RX_SLOTS + 1; i++) {
        frame.src[0] = (uint8_t)i;
        net_rx_submit((const uint8_t *)&frame, sizeof frame);
    }
    if (rx_count != NET_RX_SLOTS || net_stats.rx_drops != drops + 1)
        goto fail;
    for (unsigned i = 0; i < NET_RX_SLOTS; i++) {
        const struct rx_frame *slot = &rx_frames[(rx_head + i) % NET_RX_SLOTS];
        const struct eth_hdr *copy = (const struct eth_hdr *)slot->data;
        if (slot->len != sizeof frame || copy->src[0] != i)
            goto fail;
    }
    net_rx_drain();
    if (rx_count != NET_RX_SLOTS - NET_RX_BATCH)
        goto fail;
    net_rx_submit((const uint8_t *)&frame, sizeof frame);
    if (((const struct eth_hdr *)rx_frames[0].data)->src[0] != NET_RX_SLOTS)
        goto fail;
    while (rx_count)
        net_rx_drain();
    kprintf("PASS network ingress queue: copies, FIFO wrap, full drop and batch bound\n");
    return 0;
fail:
    kprintf("FAIL network ingress queue\n");
    return -1;
}
core_initcall(check_receive_queue);

static int check_receive_worker(void)
{
    const uint32_t ip = htonl(0xc0000201u);
    const uint8_t mac[6] = {2, 15, 0, 0, 0, 1};
    struct __attribute__((packed)) {
        struct eth_hdr eth;
        struct arp_pkt arp;
    } frame = {0};
    uint8_t found[6];

    task_sleep_ticks(2);
    frame.eth.type = htons(ETHERTYPE_ARP);
    frame.arp.htype = htons(1);
    frame.arp.ptype = htons(ETHERTYPE_IP);
    frame.arp.hlen = 6;
    frame.arp.plen = 4;
    frame.arp.oper = htons(2);
    frame.arp.spa = ip;
    memcpy(frame.arp.sha, mac, sizeof mac);
    net_rx_submit((const uint8_t *)&frame, sizeof frame);
    for (unsigned i = 0; i < TIMER_HZ; i++) {
        if (arp_lookup(ip, found) && memcmp(found, mac, sizeof mac) == 0) {
            kprintf("PASS network ingress worker: dispatched an arrival without caller polling\n");
            return 0;
        }
        task_sleep_ticks(1);
    }
    kprintf("FAIL network ingress worker\n");
    return -1;
}
late_initcall(check_receive_worker);
