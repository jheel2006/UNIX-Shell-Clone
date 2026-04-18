# UNIX-Shell-Clone

This project is a UNIX shell implemented in phases.

The codebase currently contains:
- `myshell` for the local shell path
- `server` for the socket-based remote shell path
- `client` for the remote shell client
- `scheduler_test` for Phase 4 scheduler-core verification

## Current Status

Phase 1 and earlier Phase 3 shell behavior still build as before.

For Phase 4, only the `L` portion of the split has been implemented so far.
That means the scheduler core exists, is compiled, and can be tested in isolation,
but it is not fully wired into the live client/server execution path yet.

## What L Has Done

The following Phase 4 scheduler-side work is implemented:

1. Added a dedicated scheduler module.
   Files:
   - `include/scheduler.h`
   - `src/scheduler.c`

2. Added a `Task` model that stores the information the scheduler needs.
   This includes:
   - task id
   - client number
   - task type
   - predicted burst
   - remaining burst
   - number of completed rounds
   - arrival order
   - preemption flag
   - cancellation flag

3. Added a waiting queue managed by the scheduler.
   The queue stores pending tasks until the scheduler selects one to run.

4. Added a dedicated scheduler thread.
   This thread simulates the single CPU/main execution lane required by the
   Phase 4 description. Only one scheduled task is dispatched at a time.

5. Implemented the combined scheduling policy.
   The current policy is:
   - shell commands always have immediate priority
   - program tasks are chosen using shortest remaining job first
   - round robin quantum is used per round
   - first round quantum = 3
   - later round quantum = 7

6. Implemented selective preemption support.
   The scheduler can request that a running program task stop and return to the
   waiting queue when:
   - a shell command arrives
   - a program with shorter remaining burst arrives

7. Implemented re-queueing behavior.
   If a program task does not finish in its current round, it is returned to the
   queue with updated remaining burst rather than restarting from the beginning.

8. Added synchronization for shared scheduler state.
   The scheduler uses:
   - mutex protection for shared data
   - a condition variable for waking the scheduler thread when work arrives

9. Added cancellation hooks for client-owned tasks.
   This allows the server integration layer to remove waiting tasks belonging to
   a disconnected client and mark a currently running task for cancellation.

10. Added a standalone scheduler test harness.
    File:
    - `src/scheduler_test.c`

    This test harness verifies the scheduling logic without needing the full
    socket server integration to be finished first.

## What J Still Needs To Do

The following work is still required on the execution/client-integration side:

1. Modify the server client-thread flow so requests create tasks instead of
   executing immediately inside the client thread.

2. Detect shell commands versus program commands.
   This classification is needed before submitting the task to the scheduler.

3. Implement the real execution callback for scheduled tasks.
   In practice, J needs to provide something equivalent to:
   - `execute_task(task, quantum)`

   That execution layer must:
   - run shell commands to completion in one round
   - run programs for only the scheduled quantum
   - preserve remaining work when preempted
   - return the correct scheduler result so the task is either finished,
     cancelled, or re-queued

4. Build `demo.c`.
   The demo program should:
   - accept `N`
   - print one line per second
   - run for exactly `N` iterations

5. Connect task execution output back to the client socket.
   J needs to make sure each client receives the correct output for its own
   task, especially after preemption/resume scenarios.

6. Add detailed server logs for execution behavior.
   This includes logs such as:
   - task created
   - task queued
   - task started
   - task preempted
   - task resumed
   - task finished
   - task cancelled

7. Update disconnect handling in the live server path.
   When a client disconnects, the server should call the scheduler cancellation
   path so that all tasks owned by that client are removed correctly.

## Important Boundary Between L And J

To keep the split clean:

- `L` owns the scheduling data structures, selection logic, queueing logic,
  preemption decisions, synchronization, and scheduler thread behavior.
- `J` owns request intake, command classification, real command/program
  execution, output delivery to the client, and the testing demo program.

One practical note:
- scheduler event logging belongs naturally with `L`
- execution/output logging belongs naturally with `J`

## Build Instructions

Compile everything with:

```sh
make clean && make all
```

This currently builds:
- `myshell`
- `client`
- `server`
- `scheduler_test`

## How To Test L's Phase 4 Work

Since the full live server integration is not finished yet, the correct way to
test the implemented Phase 4 scheduler core right now is the standalone test:

```sh
./scheduler_test
```

What this test does:

1. Submits one longer program task.
2. Submits a shorter program task after it.
3. Submits a shell task after that.

What you should observe in the output:

1. The first long task is dispatched.
2. When the shorter task arrives, the scheduler requests preemption.
3. The first task is re-queued with reduced remaining burst.
4. The shell task is selected immediately when available.
5. The shorter program finishes before the longer one resumes and completes.

This confirms that the following scheduler behaviors are working:
- enqueueing
- dispatching
- selective preemption
- re-queueing
- shell-command priority
- shortest-remaining-job selection
- round-based quantum handling

## Existing Shell Usage

### Local Shell

Run:

```sh
./myshell
```

### Remote Shell

Run the server in one terminal:

```sh
./server
```

Run the client in another terminal:

```sh
./client
```

Example client input:

```text
$ pwd
$ ls -l
$ unknowncmd
$ exit
```

Important:
The current `server` executable still builds, but the complete Phase 4 runtime
behavior depends on J finishing the real task-execution integration described
above. So the scheduler core is ready, but the final end-to-end Phase 4 behavior
is not fully complete yet.

## Notes

- Output redirection files such as `output.txt` and `error.log` may be created
  in the project directory depending on executed commands.
- Type `exit` in the client to close a remote shell session cleanly.
