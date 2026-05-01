## UNIX Shell Clone

A UNIX-style shell project built in C with both a local shell and a TCP-based remote shell. The codebase also includes a scheduler-driven execution path for long-running demo programs, live state logging, and a standalone scheduler test harness.

## Features

- Local shell executable: `myshell`
- Remote shell server and client: `server` and `client`
- Task scheduling support for program execution
- Preemption and re-queueing for long-running tasks
- Live demo progress updates for `./demo N`
- Colored task-state output in the server logs
- Gantt-style execution summary for preempted workloads
- Standalone scheduler test binary: `scheduler_test`

## Project Layout

- `src/main.c` — entry point for the local shell
- `src/client.c` — remote shell client
- `src/server.c` — remote shell server and task execution logic
- `src/scheduler.c` — task scheduling core
- `src/scheduler_test.c` — scheduler test harness
- `src/demo.c` — long-running demo program
- `include/` — project headers

## Build

Build everything with:

```sh
make clean && make all
```

This produces:

- `myshell`
- `client`
- `server`
- `scheduler_test`
- `demo`

## Run

### Local Shell

Start the local shell:

```sh
./myshell
```

### Remote Shell

Start the server in one terminal:

```sh
./server
```

Start the client in another terminal (multiple clients can be started in multiple terminals):

```sh
./client
```

Example commands inside the client:

```text
pwd
ls -l
./demo 3
exit
```

## Demo Program

`./demo N` runs for `N` one-second iterations and is used to show scheduling behavior and real-time progress updates.

Example:

```text
./demo 5
```

## Scheduler Test

The standalone scheduler test can be run with:

```sh
./scheduler_test
```

It exercises the task queue, dispatch logic, preemption, and re-queueing behavior without needing the full client/server flow.

## Notes

- The server prints colored task-state logs for easier reading.
- The execution summary includes a Gantt-style timeline when preemption occurs.
- Output files such as `output.txt` and `error.log` may be created depending on the commands you run.
- Use `exit` in the client to close the remote session cleanly.

## Requirements

- A C compiler
- POSIX-compatible environment
- Standard build tools such as `make`

## License

No explicit license has been provided.
