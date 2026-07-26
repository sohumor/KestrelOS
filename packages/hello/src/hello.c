/* hello.c - the demo program shipped in the `hello` package.
 *
 * Deliberately tiny: it exists so `kpkg install hello` has something
 * trivially verifiable to install, and so the dependency resolver has a
 * package with a dependency to resolve (hello depends on
 * kestrel-extras).
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *who = "world";
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            continue;
        who = argv[i];
        break;
    }
    printf("Hello, %s! -- from the KestrelOS `hello` package.\n", who);
    return 0;
}
