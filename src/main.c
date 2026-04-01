#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("raft-engine 0.1.0");
        return 0;
    }

    if (argc != 1) {
        fprintf(stderr, "Usage: raft-engine [--version]\n");
        return 1;
    }

    puts("Raft Engine initialized successfully.");
    return 0;
}
