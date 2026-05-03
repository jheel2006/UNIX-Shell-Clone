#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * demo
 * ----
 * Phase 4 test program used by the server scheduler. It prints one
 * progress line each second so the preemption/resume behavior is easy
 * to see in seperate client terminals.
 */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <N>\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);

    /*
     * The sample output counts from 0 through N, so "./demo 12" shows
     * Demo 0/12 ... Demo 12/12. This makes the final complete state
     * visible during the demo video.
     */
    for (int i = 0; i <= n; i++) {
        printf("Demo %d/%d\n", i, n);
        fflush(stdout);
        sleep(1);
    }

    return 0;
}
