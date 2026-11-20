#pragma once

#define TEST_EXIT_SUCCESS 0x10
#define TEST_EXIT_FAILURE 0x11

void testmode_init(void);
void testmode_ready(void);
void testmode_exit(unsigned code);
