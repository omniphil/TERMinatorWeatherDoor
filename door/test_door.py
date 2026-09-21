#!/usr/bin/env python3
"""A fake TERMinator on a pty, so the door can be tested without a BBS or Windows.

It answers Query, takes the module upload and checks its hash, plays the part of
the module, and checks what comes back: the forecast packets, a place search, a
pick, a unit change. It talks to the real Open-Meteo, so it needs the network.

    python3 test_door.py             base64 transport
    python3 test_door.py --bin       binary frames
    python3 test_door.py --nomouse   a TERMinator before 1.1.3 (no mouse=1): the
                                     door must not offer TRACE, and say to update
"""
import base64
import hashlib
import os
import pty
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time

MODULE = "weather"
APC = "\033_TERMinator:TRACE;"
ST = "\033\\"

IN_CURRENT, IN_HOURLY, IN_DAILY, IN_PLACES, IN_STATUS, IN_PREFS = range(1, 7)
OUT_READY, OUT_SEARCH, OUT_PICK, OUT_UNITS, OUT_REFRESH = range(1, 6)

ESCAPED = {0x00, 0x0A, 0x0D, 0x11, 0x13, 0x18, 0x1B, ord('='), 0xFF}

fails = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        fails.append(what)


def unescape(data):
    out, i = bytearray(), 0
    while i < len(data):
        if data[i] == ord('=') and i + 1 < len(data):
            out.append((data[i + 1] - 64) & 0xFF)
            i += 2
        else:
            out.append(data[i])
            i += 1
    return bytes(out)


def escape(data):
    """Module -> door, as TERMinator does it: EVERY control byte is escaped,
    because the door reads through a pty that would act on them (0x03 is
    Ctrl-C). Escaping only the door's nine lets a message type of 3 kill it."""
    out = bytearray()
    for b in data:
        if b in ESCAPED or b < 0x20:
            out += bytes((ord('='), (b + 64) & 0xFF))
        else:
            out.append(b)
    return bytes(out)


class Fake:
    """Reads APC commands from the door and answers as TERMinator would."""

    def __init__(self, binary, mouse=True):
        self.binary = binary
        self.mouse = mouse
        self.module_sent = None
        self.buf = b""
        self.assets = {}        # sha256 -> bytes
        self.module = None
        self.pending = {}       # (kind, hash) -> bytearray
        self.from_door = []     # decoded module messages
        self.raw = b""          # plain terminal output, for spotting prompts

    # -- transport ---------------------------------------------------------
    def feed(self, chunk):
        self.buf += chunk
        self.raw = (self.raw + chunk)[-8192:]
        cmds = []
        while True:
            start = self.buf.find(b"\033_")
            if start < 0:
                self.buf = self.buf[-2:]        # keep a possible split ESC
                break
            end = self.buf.find(b"\033\\", start)
            if end < 0:
                break
            cmds.append(self.buf[start + 2:end])
            self.buf = self.buf[end + 2:]
        return cmds

    def send(self, fd, text):
        os.write(fd, (APC + text + ST).encode("latin-1"))

    def send_module_data(self, fd, payload):
        """Module -> door. TERMinator escapes every control byte on this path."""
        if self.binary:
            head = f"Data;module={MODULE}".encode()
            body = escape(head + b"\n" + payload)
            os.write(fd, b"\033_TERMinator:TRACE;Bin;" + body + b"\033\\")
        else:
            b64 = base64.b64encode(payload).decode()
            self.send(fd, f"Data;module={MODULE};b64={b64}")

    # -- command handling --------------------------------------------------
    def handle(self, fd, cmd):
        text = cmd.decode("latin-1", "replace")
        if not text.startswith("TERMinator:TRACE;"):
            return
        body = text[len("TERMinator:TRACE;"):]

        if body.startswith("Query"):
            caps = "Info;v=2;wasm=1;audio=1;assets=1;send=1;store=1;tick=1"
            if self.mouse:
                caps += ";text=1;mouse=1;pad=1;import=1"
            if self.binary:
                caps += ";bin=1"
            self.send(fd, caps)
            return

        if body.startswith("Asset;"):
            sha = re.search(r"sha256=([0-9a-f]{64})", body).group(1)
            if sha in self.assets:
                self.send(fd, f"Have;module={MODULE};sha256={sha}")
            else:
                self.pending[("asset", sha)] = bytearray()
                self.send(fd, f"NeedAsset;module={MODULE}")
            return

        if body.startswith("Open;"):
            sha = re.search(r"wasm=([0-9a-f]{64})", body).group(1)
            check("exclusive=1" in body, "Open asks for the whole screen (exclusive=1)")
            if self.module == sha:
                self.send(fd, f"Ready;module={MODULE}")
            else:
                self.pending[("module", sha)] = bytearray()
                self.send(fd, f"Need;module={MODULE}")
            return

        if body.startswith("Bin;"):
            # A binary frame carries its own header line, and it may be either
            # an upload chunk or a data message -- route on that, not on "Bin".
            payload = unescape(cmd[len("TERMinator:TRACE;Bin;"):])
            nl = payload.find(b"\n")
            head = payload[:nl].decode() if nl >= 0 else payload.decode()
            if head.startswith("Put;"):
                self.handle_put(fd, head, cmd)
            elif head.startswith("Data;"):
                # Strip the frame's own "Data;module=..." line; what follows is
                # the door's message, which dispatch() unwraps in turn.
                self.dispatch(payload[nl + 1:])
            return

        if body.startswith("Put;"):
            self.handle_put(fd, body, cmd)
            return

        if body.startswith("PutDone;"):
            sha = re.search(r"(?:asset|module)=([0-9a-f]{64})", body)
            if "asset=" in body:
                sha = re.search(r"asset=([0-9a-f]{64})", body).group(1)
                data = bytes(self.pending.pop(("asset", sha), b""))
                got = hashlib.sha256(data).hexdigest()
                check(got == sha, f"asset hash matches ({len(data)} bytes)")
                self.assets[sha] = data
                self.send(fd, f"Have;module={MODULE};sha256={sha}")
            else:
                key = next(k for k in self.pending if k[0] == "module")
                data = bytes(self.pending.pop(key))
                got = hashlib.sha256(data).hexdigest()
                check(got == key[1], f"module hash matches ({len(data)} bytes)")
                self.module = key[1]
                self.module_sent = data
                self.send(fd, f"Ready;module={MODULE}")
            return

        if body.startswith("Data;"):
            self.handle_data(body, cmd)
            return

        if body.startswith("Close;"):
            self.send(fd, f"Closed;module={MODULE}")

    def handle_put(self, fd, body, raw):
        if raw.startswith(b"TERMinator:TRACE;Bin;"):
            payload = unescape(raw[len("TERMinator:TRACE;Bin;"):])
            nl = payload.find(b"\n")
            head, data = payload[:nl].decode(), payload[nl + 1:]
        else:
            head, _, b64 = body.partition(";data=")
            data = base64.b64decode(b64)
        off = int(re.search(r"offset=(\d+)", head).group(1))
        kind = "asset" if "asset=" in head else "module"
        sha = re.search(r"(?:asset|module)=([0-9a-f]{64})", head)
        key = (kind, sha.group(1)) if sha else next(k for k in self.pending if k[0] == kind)
        buf = self.pending.setdefault(key, bytearray())
        if len(buf) < off + len(data):
            buf.extend(b"\0" * (off + len(data) - len(buf)))
        buf[off:off + len(data)] = data

    def handle_data(self, body, raw):
        if body.startswith("Bin;") or raw.startswith(b"TERMinator:TRACE;Bin;"):
            payload = unescape(raw[len("TERMinator:TRACE;Bin;"):])
            nl = payload.find(b"\n")
            head, data = payload[:nl].decode(), payload[nl + 1:]
            if head.startswith(f"Data;module={MODULE}"):
                self.dispatch(data)
            return
        if ";b64=" in body:
            data = base64.b64decode(body.split(";b64=", 1)[1])
            self.dispatch(data)
        else:
            # plain text, e.g. "Data;module=micropolis;tiles=<sha>"
            prefix = f"Data;module={MODULE};"
            if body.startswith(prefix):
                self.from_door.append(("text", body[len(prefix):]))

    def dispatch(self, data):
        nl = data.find(b"\n")
        if nl < 0:
            return
        payload = data[nl + 1:]
        if len(payload) < 8:
            return
        mtype, flags, count, nbytes = struct.unpack_from("<BBHI", payload, 0)
        self.from_door.append((mtype, count, payload[8:8 + nbytes]))


def packet(mtype, body=b""):
    return struct.pack("<BBHI", mtype, 0, 1, len(body)) + body


def main():
    binary = "--bin" in sys.argv
    mouse = "--nomouse" not in sys.argv
    print("Weather door test (%s, %s)"
          % ("binary frames" if binary else "base64",
             "mouse" if mouse else "NO mouse -- TRACE must not be offered"))

    here = os.path.dirname(os.path.abspath(__file__))
    for need in ("weatherdoor", "weather.wasm"):
        if not os.path.exists(os.path.join(here, need)):
            print(f"missing {need} -- run make && make install")
            return 1
    # A clean slate for the local caller, so this is a first visit.
    shutil.rmtree(os.path.join(here, "saves", "Player-0"), ignore_errors=True)

    pid, fd = pty.fork()
    if pid == 0:
        # Run from somewhere unrelated: a BBS never starts a door in its folder.
        os.chdir(tempfile.gettempdir())
        os.execv(os.path.join(here, "weatherdoor"), ["weatherdoor"])
        os._exit(1)

    fake = Fake(binary, mouse)
    deadline = time.time() + 60
    step = "menu"
    got = {}
    places = []
    step_at = time.time()

    try:
        while time.time() < deadline and step != "done":
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                for cmd in fake.feed(chunk):
                    fake.handle(fd, cmd)

            for item in fake.from_door:
                if item[0] == "text":
                    got.setdefault("text", []).append(item[1])
                else:
                    got.setdefault(item[0], []).append(item)
            fake.from_door = []

            if step == "menu" and b"Choose how to view" in fake.raw and not mouse:
                time.sleep(0.3)
                pump_more = os.read(fd, 65536) if select.select([fd], [], [], 0.3)[0] else b""
                raw = fake.raw + pump_more
                check(b"UPDATE" in raw and b"1.1.3" in raw, "the menu says to update TERMinator")
                check(b"Enter = \x1b[1;37m[2]" in raw, "ANSI is the default")
                os.write(fd, b"1")          # TRACE must be refused
                time.sleep(0.5)
                os.write(fd, b"q")
                step = "done"
            elif step == "menu" and b"Choose how to view" in fake.raw:
                check(b"DETECTED" in fake.raw, "the menu says TRACE was detected")
                os.write(fd, b"1")
                step = "open"
            elif step == "open" and "start" in got.get("text", []):
                print("  --   module reports ready")
                fake.send_module_data(fd, packet(OUT_READY))
                step = "forecast"
                step_at = time.time()
            elif step == "forecast" and IN_DAILY in got:
                prefs = got.get(IN_PREFS, [None])[0]
                check(prefs is not None and prefs[2][1] == 1, "prefs say first visit (open the picker)")
                cur = got[IN_CURRENT][-1][2]
                check(len(cur) == 100, f"WxCurrent is 100 bytes ({len(cur)})")
                name = cur[50:98].split(b"\0")[0].decode()
                check(name.startswith("New York"), f"first forecast is the board default ({name})")
                hr = got[IN_HOURLY][-1][2]
                count = struct.unpack_from("<H", hr, 4)[0]
                check(count == 168, f"168 hours ({count})")
                dy = got[IN_DAILY][-1][2]
                check(struct.unpack_from("<H", dy, 0)[0] == 7, "7 days")
                check(all(len(x[2]) <= 4096 for k in (IN_CURRENT, IN_HOURLY, IN_DAILY) for x in got[k]),
                      "every packet fits the 4 KB payload limit")
                q = b"Portland, OR"
                fake.send_module_data(fd, packet(OUT_SEARCH, bytes([len(q)]) + q))
                got.pop(IN_CURRENT, None)
                step = "search"
            elif step == "search" and IN_PLACES in got:
                body = got[IN_PLACES][-1][2]
                n = len(body) // 56
                places = [body[i * 56 + 8:i * 56 + 56].split(b"\0")[0].decode() for i in range(n)]
                check(n >= 1 and places[0] == "Portland, Oregon", f"search found {places[:3]}")
                fake.send_module_data(fd, packet(OUT_PICK, bytes([0])))
                step = "pick"
            elif step == "pick" and IN_CURRENT in got:
                cur = got[IN_CURRENT][-1][2]
                name = cur[50:98].split(b"\0")[0].decode()
                check(name == "Portland, Oregon", f"the pick fetched the new place ({name})")
                fake.send_module_data(fd, packet(OUT_UNITS, bytes([1])))
                step = "units"
                step_at = time.time()
            elif step == "units" and time.time() - step_at > 1.5:
                prefs = open(os.path.join(here, "saves", "Player-0", "prefs")).read()
                check("Portland, Oregon" in prefs and "units = 1" in prefs and "display = 1" in prefs,
                      "prefs saved beside the binary: place, units, display")
                step = "done"

        check(step == "done", f"got through every step (stopped at '{step}')")
        if mouse:
            check(fake.module_sent == open(os.path.join(here, "weather.wasm"), "rb").read(),
                  "door sent weather.wasm")
        else:
            check(fake.module_sent is None, "no module was sent")
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
        except (OSError, ChildProcessError):
            pass

    print()
    if fails:
        print(f"FAILED ({len(fails)}): " + "; ".join(fails))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
