#!/usr/bin/env python3
"""MCP server that lets an MCP client (such as Claude) control Gens64.

It speaks MCP over stdio and forwards tool calls to the --control-pipe
named pipe. Only the Python standard library is needed.

  python gens64_mcp.py [--pipe NAME] [--exe PATH\\gens.exe] [--dll-dir DIR]

With --exe, the gens_start tool can launch the emulator.
"""
import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from gens64ctl import Client, connect

# Newest first. The tools used here are the same in all of them.
PROTOCOL_VERSIONS = ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05")
SERVER_VERSION = "1.0"
DEFAULT_TIMEOUT = 120.0

BUTTONS = ["up", "down", "left", "right", "start", "a", "b", "c", "mode", "x", "y", "z"]
MEMORIES = ["rom", "ram", "vram", "cram", "z80", "mcdprg", "mcdword", "32xram"]


class ToolError(Exception):
    """Reported to the client as a failed tool call."""


def clean(text):
    """A line break would start a second command on the pipe."""
    if "\n" in text or "\r" in text:
        raise ToolError("line breaks are not allowed in arguments")
    return text


def integer(args, name, default=None):
    value = args.get(name, default)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ToolError("%s must be a non-negative integer" % name)
    return value


def text_result(text):
    return [{"type": "text", "text": text or "OK"}]


class Gens64:
    def __init__(self, options):
        self.pipe_name = options.pipe
        self.exe = options.exe
        self.dll_dir = options.dll_dir

    def exchange(self, command, connect_wait, timeout):
        """Send one command and return the raw reply line."""
        outcome = {}

        def work():
            try:
                path = "\\\\.\\pipe\\" + self.pipe_name
                with connect(path, time.monotonic() + connect_wait) as pipe:
                    client = Client(pipe)
                    client.send(command)
                    outcome["line"] = client.read_line()
            except (OSError, EOFError) as err:
                outcome["error"] = str(err)

        # A blocking pipe read can't be timed out, so it runs in its own thread.
        worker = threading.Thread(target=work, daemon=True)
        worker.start()
        worker.join(timeout)
        if "line" in outcome:
            return outcome["line"]
        if "error" in outcome:
            raise ToolError("Gens64 is not reachable on pipe %s: %s (use gens_start, or run "
                            "gens.exe --control-pipe %s)"
                            % (self.pipe_name, outcome["error"], self.pipe_name))
        raise ToolError("no reply from Gens64 within %g seconds" % timeout)

    def send(self, command, timeout=DEFAULT_TIMEOUT):
        line = self.exchange(command, 5.0, timeout)
        if line == "OK":
            return ""
        if line.startswith("OK "):
            return line[3:]
        raise ToolError(line[4:] if line.startswith("ERR ") else line)

    def is_running(self):
        try:
            return self.exchange("ping", 2.0, 5.0).startswith("OK")
        except ToolError:
            return False

    # Tool handlers.

    def start(self, args):
        if self.is_running():
            return text_result("Gens64 is already running.")
        if not self.exe:
            raise ToolError("this server was started without --exe, so it can't launch Gens64")

        command = [self.exe, "--control-pipe", self.pipe_name]
        if args.get("rom"):
            command.append(clean(args["rom"]))
        env = dict(os.environ)
        if self.dll_dir:
            env["PATH"] = self.dll_dir + os.pathsep + env.get("PATH", "")

        # The emulator must not touch this server's stdin and stdout.
        process = subprocess.Popen(command, cwd=os.path.dirname(os.path.abspath(self.exe)), env=env,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise ToolError("Gens64 exited at startup with code %d" % process.returncode)
            if self.is_running():
                return text_result("Gens64 started.")
            time.sleep(0.2)
        raise ToolError("Gens64 started but its control pipe did not open")

    def screenshot(self, args):
        folder = tempfile.mkdtemp(prefix="gens64_")
        try:
            path = os.path.join(folder, "screen.png")
            self.send("screenshot " + path)
            with open(path, "rb") as image:
                data = base64.b64encode(image.read()).decode("ascii")
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        return [{"type": "image", "data": data, "mimeType": "image/png"}]

    def pad(self, args):
        port = integer(args, "port", 1)
        if args.get("release"):
            state = "off"
        else:
            buttons = args.get("buttons", [])
            if not isinstance(buttons, list) or not all(b in BUTTONS for b in buttons):
                raise ToolError("buttons must be a list of: " + " ".join(BUTTONS))
            state = " ".join(buttons) or "none"
        self.send("pad %d %s" % (port, state))
        return text_result("OK")

    def read(self, args):
        return text_result(self.send("read %s %d %d" % (
            clean(args["memory"]), integer(args, "address"), integer(args, "length"))))

    def write(self, args):
        self.send("write %s %d %s" % (
            clean(args["memory"]), integer(args, "address"), clean(args["hex"])))
        return text_result("OK")

    def step(self, args):
        return text_result(self.send("step %d" % integer(args, "frames", 1)))

    def simple(self, command, args=None, name=None):
        """A command with at most one free-form argument."""
        line = command if name is None else "%s %s" % (command, clean(args[name]))
        return text_result(self.send(line))


def schema(properties=None, required=()):
    return {"type": "object", "properties": properties or {}, "required": list(required),
            "additionalProperties": False}


FILE_PROPERTY = {"path": {"type": "string", "description": "Absolute file path."}}
MEMORY_PROPERTIES = {
    "memory": {"type": "string", "enum": MEMORIES,
               "description": "ram is 68000 RAM. mcdprg and mcdword need a Sega CD game, 32xram a 32X game."},
    "address": {"type": "integer", "minimum": 0, "description": "Offset into that memory."},
}

# name: (description, input schema, handler)
TOOLS = {
    "gens_start": ("Launch Gens64, optionally with a ROM. Needs the server's --exe option.",
                   schema({"rom": {"type": "string", "description": "Absolute ROM path."}}),
                   Gens64.start),
    "gens_status": ("Get the system, game, frame count and whether the emulation is held.",
                    schema(), lambda g, a: g.simple("status")),
    "gens_load": ("Load a ROM. To start from its first frame, call gens_hold first.",
                  schema(FILE_PROPERTY, ["path"]), lambda g, a: g.simple("load", a, "path")),
    "gens_close": ("Close the current ROM.", schema(), lambda g, a: g.simple("close")),
    "gens_hold": ("Stop the emulation between frames, so it only advances with gens_step.",
                  schema(), lambda g, a: g.simple("hold")),
    "gens_resume": ("Let the emulation run on its own again in real time.",
                    schema(), lambda g, a: g.simple("resume")),
    "gens_step": ("Hold the emulation, then run this many frames as fast as possible. "
                  "60 frames is about one second of game time on NTSC.",
                  schema({"frames": {"type": "integer", "minimum": 1, "maximum": 1000000}}),
                  Gens64.step),
    "gens_reset": ("Hard reset the emulated system.", schema(), lambda g, a: g.simple("reset")),
    "gens_pad": ("Set which buttons are held on a controller until the next gens_pad call. "
                 "An empty list presses nothing. release gives the pad back to the keyboard.",
                 schema({"port": {"type": "integer", "enum": [1, 2]},
                         "buttons": {"type": "array", "items": {"type": "string", "enum": BUTTONS}},
                         "release": {"type": "boolean"}}, ["port"]),
                 Gens64.pad),
    "gens_read": ("Read up to 65536 bytes of emulated memory, returned as hex.",
                  schema(dict(MEMORY_PROPERTIES, length={"type": "integer", "minimum": 1, "maximum": 65536}),
                         ["memory", "address", "length"]),
                  Gens64.read),
    "gens_write": ("Write bytes to emulated memory.",
                   schema(dict(MEMORY_PROPERTIES, hex={"type": "string", "description": "Bytes as hex digits."}),
                          ["memory", "address", "hex"]),
                   Gens64.write),
    "gens_screenshot": ("Get the current game screen as a PNG image.", schema(), Gens64.screenshot),
    "gens_savestate": ("Save the emulator state to a file.", schema(FILE_PROPERTY, ["path"]),
                       lambda g, a: g.simple("savestate", a, "path")),
    "gens_loadstate": ("Load the emulator state from a file.", schema(FILE_PROPERTY, ["path"]),
                       lambda g, a: g.simple("loadstate", a, "path")),
    "gens_quit": ("Save the settings and close Gens64.", schema(), lambda g, a: g.simple("quit")),
}


def call_tool(gens, params):
    """Returns the tools/call result, or None for an unknown tool."""
    tool = TOOLS.get(params.get("name"))
    if tool is None:
        return None
    try:
        return {"content": tool[2](gens, params.get("arguments") or {}), "isError": False}
    except ToolError as err:
        return {"content": text_result(str(err)), "isError": True}
    except (KeyError, TypeError) as err:
        return {"content": text_result("invalid arguments: %r" % err), "isError": True}


def handle(gens, message):
    """Returns (result, error) for a request."""
    method = message.get("method")
    params = message.get("params") or {}
    if method == "initialize":
        requested = params.get("protocolVersion")
        return {"protocolVersion": requested if requested in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0],
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "gens64", "version": SERVER_VERSION}}, None
    if method == "ping":
        return {}, None
    if method == "tools/list":
        return {"tools": [{"name": name, "description": tool[0], "inputSchema": tool[1]}
                          for name, tool in TOOLS.items()]}, None
    if method == "tools/call":
        result = call_tool(gens, params)
        if result is None:
            return None, {"code": -32602, "message": "unknown tool: %s" % params.get("name")}
        return result, None
    return None, {"code": -32601, "message": "method not found: %s" % method}


def send_message(message):
    sys.stdout.buffer.write(json.dumps(message).encode("utf-8") + b"\n")
    sys.stdout.buffer.flush()


def main():
    parser = argparse.ArgumentParser(description="MCP server for controlling Gens64.")
    parser.add_argument("--pipe", default="gens64", help="control pipe name (default: gens64)")
    parser.add_argument("--exe", help="path to gens.exe, to enable the gens_start tool")
    parser.add_argument("--dll-dir", help="folder added to PATH for the emulator's DLLs")
    gens = Gens64(parser.parse_args())

    for raw in sys.stdin.buffer:
        if not raw.strip():
            continue
        try:
            message = json.loads(raw)
        except ValueError:
            send_message({"jsonrpc": "2.0", "id": None,
                          "error": {"code": -32700, "message": "parse error"}})
            continue

        # Notifications have no id and get no reply.
        if not isinstance(message, dict) or "id" not in message:
            continue

        result, error = handle(gens, message)
        reply = {"jsonrpc": "2.0", "id": message["id"]}
        if error is not None:
            reply["error"] = error
        else:
            reply["result"] = result
        send_message(reply)


if __name__ == "__main__":
    main()
