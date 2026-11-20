#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "net.h"

static uint8_t response[512];
static int length, mode, checks, failures;
struct task *smp_current_task(void) { return NULL; }
bool net_ready(void) { return true; }
uint32_t net_dns_addr(void) { return 0; }
uint32_t random_u32(void) { return 1234; }
int udp_bind(uint16_t port) { (void)port; return 0; }
void udp_unbind(uint16_t port) { (void)port; }
static void byte(int v) { response[length++] = (uint8_t)v; }
static void word(int v) { byte(v >> 8); byte(v); }
static void owner(int unrelated)
{
    if (unrelated) { byte(3); byte('b'); byte('a'); byte('d'); byte(0); }
    else { byte(0xc0); byte(12); }
}
static void answer(int unrelated, int klass)
{
    owner(unrelated); word(1); word(klass); word(0); word(0); word(4);
    byte(1); byte(2); byte(3); byte(4);
}
int udp_send(uint32_t ip, uint16_t src, uint16_t dst, const void *buf, int len)
{
    (void)ip; (void)src; (void)dst;
    memcpy(response, buf, len); length = len;
    response[2] = 0x81; response[7] = 1;
    if (mode == 1) response[5] = 0, length = 12;
    if (mode == 2) response[13] = 'x';
    if (mode == 3) response[len - 3] = 28;
    if (mode == 4) response[len - 1] = 3;
    if (mode == 5) response[2] |= 2;
    if (mode == 8) response[0] ^= 1;
    if (mode == 9) response[3] = 3;
    if (mode == 10) response[7] = 0;
    else if (mode == 11 || mode == 12) {
        response[7] = 2;
        owner(mode == 12); word(5); word(1); word(0); word(0); word(7);
        byte(5); byte('a'); byte('l'); byte('i'); byte('a'); byte('s'); byte(0);
        byte(5); byte('a'); byte('l'); byte('i'); byte('a'); byte('s'); byte(0);
        word(1); word(1); word(0); word(0); word(4);
        byte(1); byte(2); byte(3); byte(4);
    } else answer(mode == 6, mode == 7 ? 3 : 1);
    if (mode == 13) length--;
    if (mode == 14) response[len] = 0xc0, response[len + 1] = (uint8_t)len;
    return len;
}
int udp_recv(uint16_t port, void *buf, int maxlen, int timeout)
{
    (void)port; (void)maxlen; (void)timeout;
    memcpy(buf, response, length); return length;
}
int main(void)
{
    const char *names[] = {"valid A", "missing question", "wrong question name",
        "wrong question type", "wrong question class", "truncated flag",
        "unrelated A owner", "non-IN A", "wrong ID", "error rcode",
        "empty answers", "CNAME chain", "unrelated CNAME", "short RDATA",
        "compression loop"};
    for (mode = 0; mode < 15; mode++) {
        uint32_t ip = 0;
        int result = dns_resolve("example.com", &ip);
        int ok = mode == 0 || mode == 11;
        checks++;
        if ((result == 0) != ok || (ok && memcmp(&ip, "\1\2\3\4", 4))) {
            printf("FAIL: %s (result %d)\n", names[mode], result); failures++;
        }
    }
    printf("DNS: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
