#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <N>\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);

    for (int i = 0; i <= n; i++) {
        printf("Demo %d/%d\n", i, n);
        fflush(stdout);
        sleep(1);
    }

    return 0;
}