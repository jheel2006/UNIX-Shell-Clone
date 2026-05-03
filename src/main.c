#include "shell_core.h"

/*
 * main
 * ----
 * Phase 4 local shell entry point.
 * The interactive loop lives in shell_core so the server can
 * reuse the same parsing and execution path in Phase 4.
 */
int main(void) {
    return shell_run_interactive();
}
