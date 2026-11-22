#include <kestrel.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[0], "/bin/kpkg-launch-probe") ||
        strcmp(argv[2], "-f") || strcmp(argv[3], "--force"))
        return 2;

    int scoped = strcmp(argv[1], "scoped") == 0;
    int root_file = open("/doc/home.html", O_RDONLY);
    int device = open("/dev/null", O_RDONLY);
    int output = open("/tmp/kpkg-launch-probe", O_WRONLY | O_CREAT);

    if (root_file < 0 || (device < 0) != scoped || (output < 0) != scoped)
        return 3;
    close(root_file);
    if (device >= 0)
        close(device);
    if (output >= 0) {
        close(output);
        unlink_("/tmp/kpkg-launch-probe");
    }
    puts(scoped ? "declared scope applied" : "unscoped access confirmed");
    return scoped ? 23 : 0;
}
