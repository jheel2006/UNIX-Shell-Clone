#include "shell_core.h"

/*
 * main
 * ----
 * Phase 1 local shell entry point.
 * The interactive loop lives in shell_core so the server can
 * reuse the same parsing and execution path in Phase 2.
 */
int main(void) {
    return shell_run_interactive();
}
