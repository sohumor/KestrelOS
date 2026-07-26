#pragma once

#include <stdint.h>

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18
#define SEL_UDATA 0x20
#define SEL_TSS   0x28

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);
