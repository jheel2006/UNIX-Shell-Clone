# UNIX-Shell-Clone

This project is my UNIX shell project done in phases.

For Phase 2, the shell works through sockets:
- `server` runs the shell logic on the server side
- `client` shows the prompt, sends commands, and prints back the result

## How To Run

First compile everything:

```sh
make clean && make
```

This gives you 3 executables:
- `myshell`
- `server`
- `client`

## Run Phase 1 Shell Locally

If you want to run the local shell only:

```sh
./myshell
```

Then type commands directly there.

## Run Phase 2 Remote Shell

Open 2 terminals.

In terminal 1, start the server:

```sh
./server
```

In terminal 2, start the client:

```sh
./client
```

Then type commands in the client prompt.

Example:

```text
$ pwd
$ ls -l
$ unknowncmd
$ exit
```

The client shows the command output like a shell.
The server shows the socket communication flow and execution logs.

## Quick Demo

If you want a quick non-interactive demo, start `./server` in one terminal, then in another terminal run:

```sh
printf 'pwd\nls -l\nunknowncmd\nexit\n' | ./client
```

## Notes

- The server handles one client session at a time.
- Output redirection files like `output.txt` and `error.log` are created in the project directory.
- To stop the remote shell session cleanly, type:

```text
exit
```
