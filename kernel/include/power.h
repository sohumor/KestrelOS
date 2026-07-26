#pragma once

/* Machine power control. Neither call ever returns to its caller. */

__attribute__((noreturn))
void power_reboot(void);

__attribute__((noreturn))
void power_halt(void);
