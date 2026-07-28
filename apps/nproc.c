#include <kestrel.h>
#include <stdio.h>

int main(void)
{
    struct k_cpuinfo info;
    if (cpuinfo(&info) < 0) {
        printf("nproc: CPU information unavailable\n");
        return 1;
    }
    printf("nproc: %u CPU%s online (%u discovered), running on CPU %u "
           "(APIC %u)\n",
           info.online, info.online == 1 ? "" : "s", info.discovered,
           info.current_cpu, info.apic_id);
    return 0;
}
