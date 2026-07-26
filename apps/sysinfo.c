/* KestrelOS sysinfo: neofetch-style system summary.
 *
 * Left: ASCII kestrel (falcon) logo. Right: OS/version, kernel,
 * uptime, memory, process count, network info.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kestrel.h>

#define INFO_LINES 10

static const char *logo[] = {
    "        _  _         ",
    "       ( `   )_      ",
    "      .-.\\ ^ /.-.    ",
    "     (   \\ v /   )   ",
    "      `-.,\\ /,.-'    ",
    "     __.-' Y '-.__   ",
    "        /  |  \\      ",
    "       /   |   \\     ",
    "      '    |    `    ",
    "          =^=        ",
};
#define LOGO_LINES (int)(sizeof(logo) / sizeof(logo[0]))
#define LOGO_WIDTH 22

static void read_version(char *buf, unsigned long sz)
{
    strncpy(buf, "unknown", sz - 1);
    buf[sz - 1] = 0;
    int fd = open("/etc/version", O_RDONLY);
    if (fd < 0)
        return;
    long n = read(fd, buf, sz - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    char version[64];
    read_version(version, sizeof(version));

    /* uptime */
    unsigned long ms = uptime_ms();
    unsigned long secs = ms / 1000;
    char uptime_str[48];
    snprintf(uptime_str, sizeof(uptime_str), "%luh %lum %lus",
             secs / 3600, (secs / 60) % 60, secs % 60);

    /* memory */
    uint64_t total_kb = 0, free_kb = 0;
    char mem_str[48];
    if (meminfo(&total_kb, &free_kb) == 0 && total_kb > 0)
        snprintf(mem_str, sizeof(mem_str), "%llu / %llu MiB",
                 (unsigned long long)((total_kb - free_kb) / 1024),
                 (unsigned long long)(total_kb / 1024));
    else
        strcpy(mem_str, "unknown");

    /* processes */
    struct k_psinfo ps;
    int nproc = 0;
    while (psinfo(nproc, &ps) == 0)
        nproc++;

    /* network */
    struct k_netinfo ni;
    char ip_str[16], gw_str[16], mac_str[24];
    int net_up = (netinfo(&ni) == 0 && ni.up);
    if (net_up) {
        ip_ntoa(ni.ip, ip_str);
        ip_ntoa(ni.gateway, gw_str);
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ni.mac[0], ni.mac[1], ni.mac[2],
                 ni.mac[3], ni.mac[4], ni.mac[5]);
    }

    char info[INFO_LINES][80];
    char proc_str[16];
    snprintf(proc_str, sizeof(proc_str), "%d", nproc);
    snprintf(info[0], sizeof(info[0]), "\033[1;36mKestrelOS\033[0m");
    /* /etc/version already reads "KestrelOS <ver>": do not prefix it again. */
    snprintf(info[1], sizeof(info[1]), "\033[36mos:\033[0m      %s", version);
    snprintf(info[2], sizeof(info[2]), "\033[36mkernel:\033[0m  kestrel x86_64");
    snprintf(info[3], sizeof(info[3]), "\033[36muptime:\033[0m  %s", uptime_str);
    snprintf(info[4], sizeof(info[4]), "\033[36mmemory:\033[0m  %s", mem_str);
    snprintf(info[5], sizeof(info[5]), "\033[36mprocs:\033[0m   %s", proc_str);
    if (net_up) {
        snprintf(info[6], sizeof(info[6]), "\033[36mip:\033[0m      %s", ip_str);
        snprintf(info[7], sizeof(info[7]), "\033[36mgw:\033[0m      %s", gw_str);
        snprintf(info[8], sizeof(info[8]), "\033[36mmac:\033[0m     %s", mac_str);
    } else {
        snprintf(info[6], sizeof(info[6]), "\033[36mnet:\033[0m     down");
        info[7][0] = 0;
        info[8][0] = 0;
    }
    snprintf(info[9], sizeof(info[9]), "\033[36mshell:\033[0m   ksh");

    printf("\n");
    int rows = LOGO_LINES > INFO_LINES ? LOGO_LINES : INFO_LINES;
    for (int i = 0; i < rows; i++) {
        term_color(TERM_YELLOW);
        if (i < LOGO_LINES) {
            printf("  %s", logo[i]);
        } else {
            printf("  ");
            for (int j = 0; j < LOGO_WIDTH; j++)
                putchar(' ');
        }
        term_reset();
        if (i < INFO_LINES && info[i][0])
            printf("  %s", info[i]);
        printf("\n");
    }
    printf("\n");
    return 0;
}
