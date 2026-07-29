#!/usr/bin/env python3
"""
CHARYBDIS TELEMETRY DASHBOARD — live health TUI over USB.

Reads the TELEM line emitted by config/telemetry.c on the RIGHT half's USB CDC
console (charybdis_right_telem firmware) and renders a live curses dashboard:
battery for both halves, right-half voltage, an estimated drain rate, CPU busy%,
and uptime. Two-way: keybinds send `charybdis power ...` shell commands back over
the same port to toggle the EXT_POWER (trackball) rail without unplugging.

Firmware line grammar (see config/telemetry.c):
    TELEM up=<sec> batL=<pct|-1> batR=<pct|-1> vR=<mV|-1> cpu=<pct|-1>

Prereq:
    make telem            # or flash the charybdis_right_telem CI artifact
    ...flash RIGHT half, keep it tethered via a DATA usb cable...

Usage:
    ./scripts/charybdis_dashboard.py                 # auto-detect port
    ./scripts/charybdis_dashboard.py /dev/cu.usbmodemXXXX

Keys:  p toggle power   o power on   x power off   t force telem   q quit

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
TELEM_RE = re.compile(
    r"TELEM\s+up=(\d+)\s+batL=(-?\d+)\s+batR=(-?\d+)\s+vR=(-?\d+)\s+cpu=(-?\d+)"
)
BUILDSTAMP_RE = re.compile(r"BUILDSTAMP\s+git=(\S+)")
EXTPOWER_RE = re.compile(r"EXTPOWER\s+state=(-?\d+)")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
ALERT_RE = re.compile(r"HANGDUMP|PANIC|FAULT|assert|stack overflow", re.IGNORECASE)

# Drain estimate only shown once the session window is wide enough that the
# slope is meaningful (LiPo curves are flat in the mid-band → short windows lie).
DRAIN_MIN_SPAN_S = 180
DRAIN_MAX_SAMPLES = 2000  # ~2.8 h at 5 s cadence; the whole session is the window


def find_port(explicit):
    if explicit:
        return explicit
    cands = sorted(glob.glob("/dev/cu.usbmodem*"))
    return cands[0] if cands else None


class SerialLink:
    """Background reader + writer for the CDC ACM port, with a reconnect loop.

    Reads run in a daemon thread and mutate shared state under a lock; the
    curses main thread only reads that state and calls send(). The port is
    opened O_RDWR so the same fd carries both the TELEM stream and the shell
    commands the keybinds write back.
    """

    def __init__(self, port):
        self.port = port
        self.fd = -1
        self.lock = threading.Lock()
        self.connected = False
        self.stop = False

        # Shared state (guarded by lock).
        self.telem = None  # dict of latest parsed values
        self.telem_at = 0.0  # host monotonic time of last TELEM
        self.fw = "?"  # BUILDSTAMP git desc
        self.extpower = None  # last EXTPOWER state ack (0/1)
        self.last_msg = ""  # last notable line (ack / alert)
        self.alert = ""  # sticky fault/hang banner
        self.samples = deque(maxlen=DRAIN_MAX_SAMPLES)  # (uptime_s, pct, mV)

        threading.Thread(target=self._run, daemon=True).start()

    # --- connection -------------------------------------------------------
    def _open(self):
        fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        # Raw mode: no canonical line processing, no echo. CDC ACM ignores the
        # baud rate but termios still wants a valid speed set.
        attrs = termios.tcgetattr(fd)
        attrs[4] = BAUD  # ispeed
        attrs[5] = BAUD  # ospeed
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
                    if not chunk:  # EOF → USB dropped / reboot
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

    # --- parsing ----------------------------------------------------------
    def _on_line(self, line):
        line = ANSI_RE.sub("", line).rstrip("\r")
        if not line:
            return

        m = TELEM_RE.search(line)
        if m:
            up, batl, batr, vr, cpu = (int(x) for x in m.groups())
            with self.lock:
                # Uptime going backwards ⇒ the half rebooted; drop stale slope.
                if self.samples and up < self.samples[-1][0]:
                    self.samples.clear()
                self.telem = {
                    "up": up,
                    "batL": batl,
                    "batR": batr,
                    "vR": vr,
                    "cpu": cpu,
                }
                self.telem_at = time.monotonic()
                if batr >= 0 and vr >= 0:
                    self.samples.append((up, batr, vr))
            return

        m = BUILDSTAMP_RE.search(line)
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
            with self.lock:
                self.last_msg = "sent: " + cmd
        except OSError as e:
            with self.lock:
                self.last_msg = "send failed: %s" % e

    def snapshot(self):
        with self.lock:
            return {
                "connected": self.connected,
                "telem": dict(self.telem) if self.telem else None,
                "age": time.monotonic() - self.telem_at if self.telem_at else None,
                "fw": self.fw,
                "extpower": self.extpower,
                "last_msg": self.last_msg,
                "alert": self.alert,
                "drain": self._drain_locked(),
            }

    # --- drain estimate (host-side; no current sensor exists) -------------
    def _drain_locked(self):
        if len(self.samples) < 2:
            return None
        t0, p0, v0 = self.samples[0]
        t1, p1, v1 = self.samples[-1]
        span = t1 - t0
        if span < DRAIN_MIN_SPAN_S:
            return None
        span_h = span / 3600.0
        d_pct = p0 - p1  # >0 draining, <0 charging
        d_mv = v0 - v1
        pct_hr = d_pct / span_h
        mv_hr = d_mv / span_h
        hrs_left = None
        if pct_hr > 0.05:  # draining meaningfully
            hrs_left = p1 / pct_hr
        return {"pct_hr": pct_hr, "mv_hr": mv_hr, "hrs_left": hrs_left, "span_s": span}


# --- rendering ------------------------------------------------------------
def bar(pct, width):
    pct = max(0, min(100, pct))
    fill = int(round(pct / 100.0 * width))
    return "[" + "█" * fill + "·" * (width - fill) + "]"


def bat_attr(pct):
    if pct < 0:
        return curses.color_pair(0)
    if pct <= 15:
        return curses.color_pair(3)  # red
    if pct <= 35:
        return curses.color_pair(2)  # yellow
    return curses.color_pair(1)  # green


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
            elif k == "t":
                link.send("charybdis telem")

        s = link.snapshot()
        stdscr.erase()
        H, W = stdscr.getmaxyx()
        cyan = curses.color_pair(4)

        def line(y, x, text, attr=0):
            if 0 <= y < H:
                stdscr.addnstr(y, x, text, max(0, W - x - 1), attr)

        line(0, 2, "CHARYBDIS TELEMETRY DASHBOARD", cyan | curses.A_BOLD)
        conn = "● connected" if s["connected"] else "○ waiting for device"
        line(0, W - 24, conn, curses.color_pair(1 if s["connected"] else 3))
        line(1, 2, "port %s   fw %s" % (link.port, s["fw"]), curses.A_DIM)

        if s["alert"]:
            line(2, 2, "⚠ " + s["alert"], curses.color_pair(3) | curses.A_BOLD)

        t = s["telem"]
        y = 4
        line(y, 2, "── BATTERY ───────────────────────────────────", cyan)
        y += 1
        if t:
            br, vr, bl = t["batR"], t["vR"], t["batL"]
            volts = "%.3f V" % (vr / 1000.0) if vr >= 0 else "  ?  "
            line(
                y,
                4,
                "RIGHT (central)  %3s%%  %-8s %s"
                % (br if br >= 0 else "?", volts, bar(br, 20)),
                bat_attr(br),
            )
            y += 1
            line(
                y,
                4,
                "LEFT  (periph)   %3s%%           %s"
                % (bl if bl >= 0 else "?", bar(bl, 20)),
                bat_attr(bl),
            )
            y += 1
            d = s["drain"]
            if d is None:
                line(
                    y,
                    4,
                    "drain (est)      … gathering (needs ≥%ds span)" % DRAIN_MIN_SPAN_S,
                    curses.A_DIM,
                )
            elif d["pct_hr"] < -0.05:
                line(
                    y,
                    4,
                    "drain (est)      ↑ charging  (+%.1f %%/hr)" % (-d["pct_hr"]),
                    curses.color_pair(1),
                )
            else:
                left = "~%.1f h to empty" % d["hrs_left"] if d["hrs_left"] else "—"
                line(
                    y,
                    4,
                    "drain (est)      ~%.1f %%/hr  ~%d mV/hr  %s"
                    % (d["pct_hr"], d["mv_hr"], left),
                    curses.A_NORMAL,
                )
        else:
            line(y, 4, "-- waiting for TELEM line --", curses.A_DIM)
        y += 2

        line(y, 2, "── SYSTEM ────────────────────────────────────", cyan)
        y += 1
        if t:
            cpu = t["cpu"]
            cattr = curses.color_pair(3 if cpu >= 80 else 2 if cpu >= 40 else 1)
            line(
                y,
                4,
                "CPU busy   %3s%%  %s"
                % (cpu if cpu >= 0 else "?", bar(max(cpu, 0), 20)),
                cattr if cpu >= 0 else curses.A_DIM,
            )
            y += 1
            line(y, 4, "uptime     %s" % fmt_hms(t["up"]))
            y += 1
            age = s["age"]
            aattr = curses.color_pair(2) if (age and age > 12) else curses.A_DIM
            line(y, 4, "last TELEM %.1fs ago" % (age if age else 0), aattr)
        else:
            line(y, 4, "--", curses.A_DIM)
            y += 2
        y += 2

        line(y, 2, "── CONTROL ───────────────────────────────────", cyan)
        y += 1
        ep = s["extpower"]
        eptxt = "ON" if ep == 1 else "OFF" if ep == 0 else "?"
        epattr = curses.color_pair(1 if ep == 1 else 3 if ep == 0 else 0)
        line(y, 4, "EXT_POWER (trackball rail): ", curses.A_NORMAL)
        line(y, 32, eptxt, epattr | curses.A_BOLD)
        y += 1
        line(y, 4, "[p] toggle   [o] on   [x] off   [t] force telem", curses.A_NORMAL)
        y += 1
        if s["last_msg"]:
            line(y, 4, "› " + s["last_msg"], curses.A_DIM)
        y += 2
        line(y, 2, "[q] quit", curses.A_DIM)

        stdscr.refresh()
        time.sleep(0.25)


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
            "  - A board in bootloader mode shows as a disk, not a serial port.\n"
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
