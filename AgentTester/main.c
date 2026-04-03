#include <stdio.h>
#include <string.h>
#include <Core/gb.h>

int main(int argc, char **argv)
{
    fprintf(stderr, "SameBoy Agent Tester v" GB_VERSION "\n");

    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        return 0;
    }

    fprintf(stderr, "Usage: %s [--interactive] [--script <path.json>] [--model <model>] [--boot <path>] [rom]\n", argv[0]);
    return 0;
}
