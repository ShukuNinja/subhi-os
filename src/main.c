#include <stdio.h>
#include <string.h>

#define SUBHI_VERSION "0.1.0"

static void print_usage(const char *program)
{
    printf("Subhi-OS v%s\n", SUBHI_VERSION);
    printf("A Linux-based virtual filesystem and storage engine.\n\n");

    printf("Usage:\n");
    printf("  %s --help\n", program);
    printf("  %s --version\n", program);

    /*
     * Filesystem commands will be added in later milestones.
     */
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0) {
        printf("Subhi-OS v%s\n", SUBHI_VERSION);
        return 0;
    }

    fprintf(stderr, "Error: unknown option '%s'\n", argv[1]);
    fprintf(stderr, "Run '%s --help' for usage.\n", argv[0]);

    return 2;
}
