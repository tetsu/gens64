#!/usr/bin/env python3
"""Send commands to a running Gens64 started with --control-pipe.

Examples:
  gens64ctl.py status
  gens64ctl.py --pipe gens64 screenshot C:\\temp\\shot.png
  gens64ctl.py - < commands.txt      (one command per line)

Prints each reply. Exit status: 0 = OK, 1 = the emulator replied ERR,
2 = could not connect, or no reply before the timeout.
"""
import argparse
import os
import sys
import threading
import time


def connect(path, deadline):
    """Open the pipe. It is busy for a moment after the previous client leaves."""
    while True:
        try:
            return open(path, "r+b", buffering=0)
        except FileNotFoundError:
            # Nothing is serving this pipe, so waiting won't help.
            raise
        except OSError as err:
            if time.monotonic() >= deadline:
                raise err
            time.sleep(0.02)


class Client:
    def __init__(self, pipe):
        self.pipe = pipe
        self.buffer = b""

    def send(self, command):
        self.pipe.write(command.encode("utf-8") + b"\n")

    def read_line(self):
        while b"\n" not in self.buffer:
            data = self.pipe.read(65536)
            if not data:
                raise EOFError("the emulator closed the connection")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line.decode("utf-8", "replace")


def run(args, commands):
    """Returns the exit status."""
    try:
        path = "\\\\.\\pipe\\" + args.pipe
        pipe = connect(path, time.monotonic() + min(args.timeout, 5.0))
    except OSError as err:
        print("cannot connect to %s: %s" % (args.pipe, err), file=sys.stderr)
        return 2

    client = Client(pipe)
    try:
        for command in commands:
            client.send(command)
            line = client.read_line()
            print(line, flush=True)
            if not line.startswith("OK"):
                return 1
    except (OSError, EOFError) as err:
        print("connection lost: %s" % err, file=sys.stderr)
        return 2
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Send a command to a running Gens64.")
    parser.add_argument("--pipe", default="gens64",
                        help="pipe name given to --control-pipe (default: gens64)")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="seconds to wait for all replies (default: 60)")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="command and arguments, or - to read commands from stdin")
    args = parser.parse_args()

    # A reply must print even if the console can't show all of its characters.
    sys.stdout.reconfigure(errors="replace")

    if not args.command:
        parser.error("no command given")

    if args.command == ["-"]:
        commands = [line.strip() for line in sys.stdin if line.strip()]
    else:
        commands = [" ".join(args.command)]

    result = []
    # A blocking pipe read can't be timed out, so it runs in its own thread.
    worker = threading.Thread(target=lambda: result.append(run(args, commands)),
                              daemon=True)
    worker.start()
    worker.join(args.timeout)

    sys.stdout.flush()
    if not result:
        print("no reply within %g seconds" % args.timeout, file=sys.stderr)
        sys.stderr.flush()
        os._exit(2)
    os._exit(result[0])


if __name__ == "__main__":
    main()
