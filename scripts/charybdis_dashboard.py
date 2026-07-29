#!/usr/bin/env python3
"""
CHARYBDIS TELEMETRY DASHBOARD — live health TUI over USB.

Polls the `charybdis` shell commands on the RIGHT half's USB CDC console
(charybdis_right_telem firmware, see config/telemetry.c) and renders a live
curses dashboard: battery for both halves, estimated drain, BLE link params +
RSSI, split-peripheral status, endpoint/output, CPU%, die temp, activity state,
last reset cause, and trackball report rate. A threads view ('s') shows
per-thread stack headroom + CPU%. Two-way: keybinds toggle the EXT_POWER
(trackball) rail without unplugging.

Prereq:
    make telem            # or flash the charybdis_right_telem CI artifact
    ...flash RIGHT half, keep it tethered via a DATA usb cable...

Usage:
    ./scripts/charybdis_dashboard.py                 # auto-detect port
    ./scripts/charybdis_dashboard.py /dev/cu.usbmodemXXXX

Keys:  p toggle power   o on   x off   s threads view   q quit

Zero dependencies — Python 3 standard library only (curses/termios/tty).
No fuel-gauge IC exists on nice!nano_v2, so current draw is unmeasurable; the
drain figure is an ESTIMATE derived from the voltage/SoC slope this session.
"""

import curses
import glob
import os
import re
import sys
import termios
import threading
import time
import tty
from collections import deque

BAUD = termios.B115200
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
FWID_RE = re.compile(r"(?:BUILDSTAMP|VER)\s+git=(\S+)")
EXTPOWER_RE = re.compile(r"EXTPOWER\s+state=(-?\d+)")
ALERT_RE = re.compile(r"HANGDUMP|PANIC|FAULT|assert|stack overflow", re.IGNORECASE)
THREAD_RE = re.compile(r"THREAD name=(\S+) free=(\d+) size=(\d+) cyc=(\d+)")
THREADEND_RE = re.compile(r"THREADEND n=(\d+)")

# The firmware only emits in response to a shell command (no log backend under
# CONFIG_SHELL), so the host drives the cadence.
POLL_INTERVAL_S = 2.0
DRAIN_MIN_SPAN_S = 180
DRAIN_MAX_SAMPLES = 2000

TEMP_NA = -9990
RSSI_NA = 127


def find_port(explicit):
    if explicit:
        return explicit
    cands = sorted(glob.glob("/dev/cu.usbmodem*"))
    return cands[0] if cands else None


def iget(d, k, default=None):
    """Parse an int field from the token dict, tolerant of missing keys."""
    try:
        return int(d[k])
    except (KeyError, ValueError, TypeError):
        return default


class SerialLink:
    """Background reader/writer for the CDC ACM port with a reconnect loop, plus
    a poller that issues the shell commands that make the firmware emit."""

    def __init__(self, port):
        self.port = port
        self.fd = -1
        self.lock = threading.Lock()
        self.connected = False
        self.stop = False
        self.want_threads = False  # main thread flips this when in threads view

        # Shared state (guarded by lock).
        self.telem = {}  # latest TELEM token dict (str values)
        self.telem_at = 0.0
        self.fw = "?"
        self.extpower = None
        self.last_msg = ""
        self.alert = ""
        self.samples = deque(maxlen=DRAIN_MAX_SAMPLES)  # (uptime_s, pct, mV)

        self.threads = []  # list of dicts: name, free, size, cpu
        self._thr_accum = []
        self._thr_prev = {}  # name -> cyc, for CPU% deltas

        threading.Thread(target=self._run, daemon=True).start()
        threading.Thread(target=self._poll, daemon=True).start()

    # --- connection -------------------------------------------------------
    def _open(self):
        fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(fd)
        attrs[4] = BAUD
        attrs[5] = BAUD
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        tty.setraw(fd)
        return fd

    def _run(self):
        buf = b""
        while not self.stop:
            if not os.path.exists(self.port):
                self._set_connected(False)
                time.sleep(0.5)
                continue
            try:
                fd = self._open()
            except OSError:
                time.sleep(0.5)
                continue
            with self.lock:
                self.fd = fd
                self.connected = True
            try:
                while not self.stop:
                    try:
                        chunk = os.read(fd, 4096)
                    except BlockingIOError:
                        time.sleep(0.05)
                        continue
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        self._on_line(raw.decode("utf-8", "ignore"))
            except OSError:
                pass
            finally:
                self._set_connected(False)
                try:
                    os.close(fd)
                except OSError:
                    pass
            time.sleep(0.3)

    def _set_connected(self, val):
        with self.lock:
            self.connected = val
            if not val:
                self.fd = -1

    def _poll(self):
        asked_ver = False
        while not self.stop:
            with self.lock:
                conn = self.connected
                threads = self.want_threads
            if conn:
                if not asked_ver:
                    self.send("charybdis ver")
                    asked_ver = True
                self.send("charybdis telem")
                if threads:
                    self.send("charybdis threads")
            else:
                asked_ver = False
            time.sleep(POLL_INTERVAL_S)

    # --- parsing ----------------------------------------------------------
    def _on_line(self, line):
        line = ANSI_RE.sub("", line).rstrip("\r")
        if not line:
            return

        if line.startswith("TELEM ") or " TELEM " in line:
            d = {}
            for tok in line[line.index("TELEM") :].split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    d[k] = v
            if "up" not in d:
                return
            up = iget(d, "up")
            batr = iget(d, "batR")
            vr = iget(d, "vR")
            with self.lock:
                if up is not None and self.samples and up < self.samples[-1][0]:
                    self.samples.clear()  # reboot → drop stale slope
                self.telem = d
                self.telem_at = time.monotonic()
                if (
                    up is not None
                    and batr is not None
                    and vr is not None
                    and batr >= 0
                    and vr >= 0
                ):
                    self.samples.append((up, batr, vr))
            return

        m = THREAD_RE.search(line)
        if m:
            name, free, size, cyc = (
                m.group(1),
                int(m.group(2)),
                int(m.group(3)),
                int(m.group(4)),
            )
            self._thr_accum.append((name, free, size, cyc))
            return

        m = THREADEND_RE.search(line)
        if m:
            self._finalize_threads()
            return

        m = FWID_RE.search(line)
        if m:
            with self.lock:
                self.fw = m.group(1)
            return

        m = EXTPOWER_RE.search(line)
        if m:
            with self.lock:
                self.extpower = int(m.group(1))
                self.last_msg = line.strip()
            return

        if ALERT_RE.search(line):
            with self.lock:
                self.alert = line.strip()[:120]
                self.last_msg = line.strip()

    def _finalize_threads(self):
        rows = self._thr_accum
        self._thr_accum = []
        total_d = 0
        prev = self._thr_prev
        for name, free, size, cyc in rows:
            total_d += max(0, cyc - prev.get(name, cyc))
        out = []
        for name, free, size, cyc in rows:
            dcyc = max(0, cyc - prev.get(name, cyc))
            cpu = (100.0 * dcyc / total_d) if total_d > 0 else 0.0
            out.append({"name": name, "free": free, "size": size, "cpu": cpu})
        self._thr_prev = {r[0]: r[3] for r in rows}
        out.sort(key=lambda r: r["free"])  # most stack-starved first
        with self.lock:
            self.threads = out

    # --- writing ----------------------------------------------------------
    def send(self, cmd):
        with self.lock:
            fd = self.fd
        if fd < 0:
            with self.lock:
                self.last_msg = "not connected — cannot send"
            return
        try:
            os.write(fd, (cmd + "\r\n").encode())
        except OSError as e:
            with self.lock:
                self.last_msg = "send failed: %s" % e

    def set_threads_view(self, on):
        with self.lock:
            self.want_threads = on

    def snapshot(self):
        with self.lock:
            return {
                "connected": self.connected,
                "telem": dict(self.telem),
                "age": time.monotonic() - self.telem_at if self.telem_at else None,
                "fw": self.fw,
                "extpower": self.extpower,
                "last_msg": self.last_msg,
                "alert": self.alert,
                "drain": self._drain_locked(),
                "threads": list(self.threads),
            }

    def _drain_locked(self):
        if len(self.samples) < 2:
            return None
        t0, p0, v0 = self.samples[0]
        t1, p1, v1 = self.samples[-1]
        span = t1 - t0
        if span < DRAIN_MIN_SPAN_S:
            return None
        span_h = span / 3600.0
        pct_hr = (p0 - p1) / span_h
        mv_hr = (v0 - v1) / span_h
        hrs_left = p1 / pct_hr if pct_hr > 0.05 else None
        return {"pct_hr": pct_hr, "mv_hr": mv_hr, "hrs_left": hrs_left}


# --- rendering ------------------------------------------------------------
def bar(pct, width):
    pct = max(0, min(100, pct))
    fill = int(round(pct / 100.0 * width))
    return "[" + "█" * fill + "·" * (width - fill) + "]"


def cp(pct):
    if pct < 0:
        return curses.color_pair(0)
    if pct <= 15:
        return curses.color_pair(3)
    if pct <= 35:
        return curses.color_pair(2)
    return curses.color_pair(1)


def fmt_hms(sec):
    sec = int(sec)
    h, rem = divmod(sec, 3600)
    m, s = divmod(rem, 60)
    return "%02d:%02d:%02d" % (h, m, s)


def draw(stdscr, link):
    curses.curs_set(0)
    stdscr.nodelay(True)
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_GREEN, -1)
    curses.init_pair(2, curses.COLOR_YELLOW, -1)
    curses.init_pair(3, curses.COLOR_RED, -1)
    curses.init_pair(4, curses.COLOR_CYAN, -1)

    mode = "main"
    while True:
        ch = stdscr.getch()
        if ch != -1:
            k = chr(ch).lower() if 0 <= ch < 256 else ""
            if k == "q":
                return
            elif k == "p":
                link.send("charybdis power toggle")
            elif k == "o":
                link.send("charybdis power on")
            elif k == "x":
                link.send("charybdis power off")
            elif k == "s":
                mode = "threads" if mode == "main" else "main"
                link.set_threads_view(mode == "threads")

        s = link.snapshot()
        stdscr.erase()
        H, W = stdscr.getmaxyx()

        def line(y, x, text, attr=0):
            if 0 <= y < H:
                stdscr.addnstr(y, x, text, max(0, W - x - 1), attr)

        cyan = curses.color_pair(4)
        line(0, 2, "CHARYBDIS TELEMETRY", cyan | curses.A_BOLD)
        conn = "● connected" if s["connected"] else "○ waiting"
        line(0, W - 16, conn, curses.color_pair(1 if s["connected"] else 3))
        line(1, 2, "port %s   fw %s" % (link.port, s["fw"]), curses.A_DIM)
        if s["alert"]:
            line(2, 2, "⚠ " + s["alert"], curses.color_pair(3) | curses.A_BOLD)

        if mode == "threads":
            draw_threads(line, s, H, cyan)
        else:
            draw_main(line, s, cyan)

        stdscr.refresh()
        time.sleep(0.25)


def draw_main(line, s, cyan):
    t = s["telem"]
    y = 4
    line(y, 2, "── BATTERY ─────────────────────────────", cyan)
    y += 1
    if t:
        br, vr, bl = iget(t, "batR", -1), iget(t, "vR", -1), iget(t, "batL", -1)
        volts = "%.3f V" % (vr / 1000.0) if vr >= 0 else "  ?  "
        line(
            y,
            4,
            "RIGHT %3s%%  %-8s %s" % (br if br >= 0 else "?", volts, bar(br, 16)),
            cp(br),
        )
        y += 1
        line(
            y,
            4,
            "LEFT  %3s%%           %s" % (bl if bl >= 0 else "?", bar(bl, 16)),
            cp(bl),
        )
        y += 1
        d = s["drain"]
        if d is None:
            line(y, 4, "drain  … gathering", curses.A_DIM)
        elif d["pct_hr"] < -0.05:
            line(
                y,
                4,
                "drain  ↑ charging (+%.1f %%/hr)" % -d["pct_hr"],
                curses.color_pair(1),
            )
        else:
            left = "~%.1f h left" % d["hrs_left"] if d["hrs_left"] else "—"
            line(
                y,
                4,
                "drain  ~%.1f %%/hr  ~%d mV/hr  %s" % (d["pct_hr"], d["mv_hr"], left),
            )
        y += 2

        line(y, 2, "── LINK ────────────────────────────────", cyan)
        y += 1
        prof, pconn = iget(t, "prof", -1), iget(t, "pconn", 0)
        rssi = iget(t, "rssi", RSSI_NA)
        out = t.get("out", "?")
        rssi_s = ("%d dBm" % rssi) if rssi != RSSI_NA else "n/a"
        pc = curses.color_pair(1) if pconn else curses.color_pair(3)
        line(
            y,
            4,
            "host  %s  prof %s  %s   RSSI %s"
            % (out.upper(), prof, "●conn" if pconn else "○down", rssi_s),
            pc,
        )
        y += 1
        ci, lat, sto = iget(t, "ci", 0), iget(t, "lat", 0), iget(t, "sto", 0)
        line(
            y,
            4,
            "conn  int %d (%.1fms)  lat %d  sto %d (%.1fs)"
            % (ci, ci * 1.25, lat, sto, sto * 10 / 1000.0),
        )
        y += 1
        periph = iget(t, "periph", 0)
        pcolor = curses.color_pair(1) if periph > 0 else curses.color_pair(2)
        line(
            y,
            4,
            "split peripheral links: %d %s"
            % (periph, "up" if periph > 0 else "(LEFT down?)"),
            pcolor,
        )
        y += 2

        line(y, 2, "── SYSTEM ──────────────────────────────", cyan)
        y += 1
        cpu = iget(t, "cpu", -1)
        temp = iget(t, "temp", TEMP_NA)
        temp_s = ("%.1f°C" % (temp / 10.0)) if temp != TEMP_NA else "n/a"
        line(
            y,
            4,
            "CPU %3s%% %s   temp %s"
            % (cpu if cpu >= 0 else "?", bar(max(cpu, 0), 12), temp_s),
            cp(100 - cpu if cpu >= 0 else -1),
        )
        y += 1
        act = {"A": "ACTIVE", "I": "IDLE", "S": "SLEEP"}.get(t.get("act"), "?")
        line(y, 4, "activity %s    reset: %s" % (act, t.get("rst", "?")))
        y += 1
        tbr = iget(t, "tbr", 0)
        age = s["age"] or 0
        rate = tbr / age if age > 0 else 0
        line(
            y,
            4,
            "uptime %s   trackball %d (%.0f/s)"
            % (fmt_hms(iget(t, "up", 0)), tbr, rate),
        )
        y += 1
        aattr = curses.color_pair(2) if age > 12 else curses.A_DIM
        line(y, 4, "last update %.1fs ago" % age, aattr)
        y += 2
    else:
        line(y, 4, "-- waiting for TELEM --", curses.A_DIM)
        y += 2

    ep = s["extpower"]
    eptxt = "ON" if ep == 1 else "OFF" if ep == 0 else "?"
    epattr = curses.color_pair(1 if ep == 1 else 3 if ep == 0 else 0)
    line(y, 2, "── CONTROL ─────────────────────────────", cyan)
    y += 1
    line(y, 4, "EXT_POWER (trackball): ")
    line(y, 27, eptxt, epattr | curses.A_BOLD)
    y += 1
    line(y, 4, "[p]toggle [o]on [x]off   [s]threads   [q]uit", curses.A_NORMAL)
    y += 1
    if s["last_msg"]:
        line(y, 4, "› " + s["last_msg"], curses.A_DIM)


def draw_threads(line, s, H, cyan):
    rows = s["threads"]
    y = 4
    line(y, 2, "── THREADS (stack-starved first) ───────", cyan)
    y += 1
    line(
        y,
        4,
        "%-18s %11s  %5s  %s" % ("name", "free/size", "cpu%", "stack"),
        curses.A_DIM,
    )
    y += 1
    if not rows:
        line(y, 4, "… requesting (charybdis threads)", curses.A_DIM)
    for r in rows:
        if y >= H - 2:
            line(y, 4, "… %d more" % (len(rows) - (y - 6)), curses.A_DIM)
            break
        size = r["size"] or 1
        used_pct = 100 * (size - r["free"]) / size
        # color by headroom: red if <15% free, yellow if <30%
        free_pct = 100 * r["free"] / size
        attr = cp(free_pct)
        line(
            y,
            4,
            "%-18s %5d/%-5d  %4.0f%%  %s"
            % (r["name"][:18], r["free"], r["size"], r["cpu"], bar(used_pct, 14)),
            attr,
        )
        y += 1
    line(H - 1, 2, "[s] back   [q] quit", curses.A_DIM)


def main():
    explicit = None
    for a in sys.argv[1:]:
        if a in ("-h", "--help"):
            print(__doc__)
            return 0
        explicit = a
    port = find_port(explicit)
    if not port:
        sys.stderr.write(
            "No USB serial port found (/dev/cu.usbmodem*).\n"
            "  - Flash the RIGHT half with `make telem` firmware and plug in.\n"
            "  - Use a DATA usb cable, not charge-only.\n"
        )
        return 1
    link = SerialLink(port)
    try:
        curses.wrapper(draw, link)
    except KeyboardInterrupt:
        pass
    finally:
        link.stop = True
    return 0


if __name__ == "__main__":
    sys.exit(main())
