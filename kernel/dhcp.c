#include "kernel.h"
#include "string.h"
#include "netdev.h"
#include "net.h"
#include "dhcp.h"

#define DHCP_BOOTREQUEST 1
#define DHCP_BOOTREPLY   2
#define DHCP_HTYPE_ETH   1
#define DHCP_MAGIC       0x63825363u

#define DHCPDISCOVER     1
#define DHCPOFFER        2
#define DHCPREQUEST      3
#define DHCPACK          5

#define OPT_SUBNET_MASK  1
#define OPT_ROUTER       3
#define OPT_DNS_SERVER   6
#define OPT_REQ_IP       50
#define OPT_MSG_TYPE     53
#define OPT_SERVER_ID    54
#define OPT_PARAM_REQ    55
#define OPT_END          255

struct dhcp_packet {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    char     sname[64];
    char     file[128];
    uint32_t magic;
    uint8_t  options[308];
} __attribute__((packed));

int dhcp_discover(void)
{
    const struct netdev *nd = netdev_current();
    if (!nd)
        return -1;

    struct dhcp_packet pkt;
    memset(&pkt, 0, sizeof(pkt));

    uint32_t xid = 0x3903F123u;
    pkt.op = DHCP_BOOTREQUEST;
    pkt.htype = DHCP_HTYPE_ETH;
    pkt.hlen = 6;
    pkt.xid = htonl(xid);
    pkt.flags = htons(0x8000);   /* broadcast reply flag */
    memcpy(pkt.chaddr, nd->mac, 6);
    pkt.magic = htonl(DHCP_MAGIC);

    int opt_idx = 0;
    pkt.options[opt_idx++] = OPT_MSG_TYPE;
    pkt.options[opt_idx++] = 1;
    pkt.options[opt_idx++] = DHCPDISCOVER;

    pkt.options[opt_idx++] = OPT_PARAM_REQ;
    pkt.options[opt_idx++] = 3;
    pkt.options[opt_idx++] = OPT_SUBNET_MASK;
    pkt.options[opt_idx++] = OPT_ROUTER;
    pkt.options[opt_idx++] = OPT_DNS_SERVER;

    pkt.options[opt_idx++] = OPT_END;

    int pkt_len = sizeof(struct dhcp_packet) - sizeof(pkt.options) + opt_idx;

    /* Send DHCPDISCOVER */
    if (udp_send(0xFFFFFFFFu, 68, 67, &pkt, pkt_len) < 0)
        return -1;

    struct dhcp_packet resp;
    uint32_t offered_ip = 0, server_id = 0;
    uint32_t mask = 0, gw = 0, dns = 0;

    int resp_len = udp_recv(68, &resp, sizeof(resp), 1500);
    if (resp_len < 240 || resp.op != DHCP_BOOTREPLY || resp.xid != htonl(xid))
        return -1;

    offered_ip = resp.yiaddr;

    /* Parse DHCPOFFER options */
    uint8_t *opts = resp.options;
    int max_opts = resp_len - (sizeof(struct dhcp_packet) - sizeof(resp.options));
    int i = 0;
    uint8_t msg_type = 0;

    while (i < max_opts && opts[i] != OPT_END) {
        uint8_t code = opts[i++];
        if (code == 0) continue;
        if (i >= max_opts) break;
        uint8_t len = opts[i++];
        if (i + len > max_opts) break;

        if (code == OPT_MSG_TYPE && len >= 1)
            msg_type = opts[i];
        else if (code == OPT_SERVER_ID && len >= 4)
            memcpy(&server_id, &opts[i], 4);
        else if (code == OPT_SUBNET_MASK && len >= 4)
            memcpy(&mask, &opts[i], 4);
        else if (code == OPT_ROUTER && len >= 4)
            memcpy(&gw, &opts[i], 4);
        else if (code == OPT_DNS_SERVER && len >= 4)
            memcpy(&dns, &opts[i], 4);

        i += len;
    }

    if (msg_type != DHCPOFFER || offered_ip == 0)
        return -1;

    /* Send DHCPREQUEST */
    memset(&pkt, 0, sizeof(pkt));
    pkt.op = DHCP_BOOTREQUEST;
    pkt.htype = DHCP_HTYPE_ETH;
    pkt.hlen = 6;
    pkt.xid = htonl(xid);
    pkt.flags = htons(0x8000);
    memcpy(pkt.chaddr, nd->mac, 6);
    pkt.magic = htonl(DHCP_MAGIC);

    opt_idx = 0;
    pkt.options[opt_idx++] = OPT_MSG_TYPE;
    pkt.options[opt_idx++] = 1;
    pkt.options[opt_idx++] = DHCPREQUEST;

    pkt.options[opt_idx++] = OPT_REQ_IP;
    pkt.options[opt_idx++] = 4;
    memcpy(&pkt.options[opt_idx], &offered_ip, 4);
    opt_idx += 4;

    if (server_id != 0) {
        pkt.options[opt_idx++] = OPT_SERVER_ID;
        pkt.options[opt_idx++] = 4;
        memcpy(&pkt.options[opt_idx], &server_id, 4);
        opt_idx += 4;
    }

    pkt.options[opt_idx++] = OPT_END;
    pkt_len = sizeof(struct dhcp_packet) - sizeof(pkt.options) + opt_idx;

    if (udp_send(0xFFFFFFFFu, 68, 67, &pkt, pkt_len) < 0)
        return -1;

    resp_len = udp_recv(68, &resp, sizeof(resp), 1500);
    if (resp_len < 240 || resp.op != DHCP_BOOTREPLY || resp.xid != htonl(xid))
        return -1;

    opts = resp.options;
    max_opts = resp_len - (sizeof(struct dhcp_packet) - sizeof(resp.options));
    i = 0;
    msg_type = 0;

    while (i < max_opts && opts[i] != OPT_END) {
        uint8_t code = opts[i++];
        if (code == 0) continue;
        if (i >= max_opts) break;
        uint8_t len = opts[i++];
        if (i + len > max_opts) break;

        if (code == OPT_MSG_TYPE && len >= 1)
            msg_type = opts[i];
        else if (code == OPT_SUBNET_MASK && len >= 4)
            memcpy(&mask, &opts[i], 4);
        else if (code == OPT_ROUTER && len >= 4)
            memcpy(&gw, &opts[i], 4);
        else if (code == OPT_DNS_SERVER && len >= 4)
            memcpy(&dns, &opts[i], 4);

        i += len;
    }

    udp_unbind(68);

    if (msg_type != DHCPACK)
        return -1;

    net_configure(offered_ip, mask, gw, dns);
    return 0;
}
