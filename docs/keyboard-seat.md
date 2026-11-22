# Keyboard seat

PS/2 and USB keyboards feed the same KestrelOS seat. Each driver delivers
a physical key identity and a press or release; only the PS/2 driver
interprets scan-code prefixes. USB boot reports deliver usage changes
directly, with no intermediate PS/2 byte stream.

The seat owns the US layout, Caps Lock, and desktop shortcuts. A
modifier held on one keyboard applies to a key pressed on another.
Releasing a modifier (or stopping a USB keyboard's input stream) drops
only that keyboard's contribution. An unsupported physical key does not
produce a console byte.

The console and compositor still consume the existing character and
shortcut codes. There is no userspace ABI or layout-setting change.
USB re-enumeration after unplugging is outside this migration.
