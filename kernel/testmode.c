#include "testmode.h"
#include "io.h"
#include "string.h"

static int mode;

static unsigned fw_be(unsigned bytes)
{
    unsigned value = 0;
    while (bytes--)
        value = (value << 8) | inb(0x511);
    return value;
}

void testmode_init(void)
{
    outw(0x510, 0);
    if (inb(0x511) != 'Q' || inb(0x511) != 'E' ||
        inb(0x511) != 'M' || inb(0x511) != 'U')
        return;
    outw(0x510, 0x19); /* FW_CFG_FILE_DIR, big-endian entries */
    unsigned count = fw_be(4);
    if (count > 256)
        return;
    while (count--) {
        unsigned size = fw_be(4);
        unsigned selector = fw_be(2);
        fw_be(2);
        char name[56];
        for (unsigned i = 0; i < sizeof(name); i++)
            name[i] = inb(0x511);
        name[55] = 0;
        if (!strcmp(name, "opt/kestrel-test") && size == 1) {
            outw(0x510, selector);
            int value = inb(0x511);
            if (value == '1' || value == '2')
                mode = value - '0';
            return;
        }
    }
}

void testmode_exit(unsigned code)
{
    if (mode)
        outl(0xf4, code);
}

void testmode_ready(void)
{
    if (mode == 1)
        testmode_exit(TEST_EXIT_SUCCESS);
}
