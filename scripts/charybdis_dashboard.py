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


# --- color + formatting helpers -------------------------------------------
# Color pairs: 1 green, 2 yellow, 3 red, 4 cyan(accent), 5 blue, 6 magenta.
C_OK, C_WARN, C_BAD, C_ACCENT, C_INFO, C_ALT = 1, 2, 3, 4, 5, 6


def C(pair, bold=False):
    a = curses.color_pair(pair)
    return a | curses.A_BOLD if bold else a


def bar(pct, width):
    pct = max(0, min(100, pct))
    fill = int(round(pct / 100.0 * width))
    return "█" * fill + "░" * (width - fill)


def pct_pair(free_pct):
    """Color for a 'higher is healthier' percentage (battery, stack headroom)."""
    if free_pct < 0:
        return 0
    if free_pct <= 15:
        return C_BAD
    if free_pct <= 35:
        return C_WARN
    return C_OK


def load_pair(load):
    """Color for a 'higher is worse' percentage (CPU load)."""
    if load < 0:
        return 0
    if load >= 80:
        return C_BAD
    if load >= 40:
        return C_WARN
    return C_OK


def fmt_hms(sec):
    sec = int(sec)
    h, rem = divmod(sec, 3600)
    m, s = divmod(rem, 60)
    return "%d:%02d:%02d" % (h, m, s)


def rssi_quality(r):
    if r == RSSI_NA:
        return "n/a", 0
    if r >= -55:
        return "excellent", C_OK
    if r >= -67:
        return "good", C_OK
    if r >= -75:
        return "fair", C_WARN
    return "weak", C_BAD


RESET_DESC = {
    "POR": ("power-on", C_INFO),
    "PIN": ("reset pin", C_INFO),
    "SOFT": ("soft reboot", C_INFO),
    "BOR": ("brownout", C_WARN),
    "WDT": ("watchdog HANG", C_BAD),
    "LOCKUP": ("CPU lockup", C_BAD),
    "none": ("—", 0),
}
ACT_DESC = {
    "ACTIVE": ("ACTIVE", "keys live", C_OK),
    "IDLE": ("IDLE", "screen idle", C_WARN),
    "SLEEP": ("SLEEP", "deep sleep", C_INFO),
}


def cpu_word(load):
    if load < 0:
        return "?"
    if load < 10:
        return "idle"
    if load < 40:
        return "light"
    if load < 80:
        return "busy"
    return "HIGH"


# --- low-level draw primitives (manual boxes on stdscr, no subwindows) -----
def put(scr, y, x, text, attr=0):
    H, W = scr.getmaxyx()
    if y < 0 or y >= H or x < 0 or x >= W:
        return
    try:
        scr.addnstr(y, x, text, W - 1 - x, attr)
    except curses.error:
        pass


def box(scr, y, x, h, w, title, tattr):
    """Draw a bordered panel; return the inner content x and first content y,
    or None if it does not fit the screen."""
    H, W = scr.getmaxyx()
    if h < 3 or w < 6 or y < 0 or x < 0 or y + h > H or x + w > W:
        return None
    try:
        scr.hline(y, x + 1, curses.ACS_HLINE, w - 2)
        scr.hline(y + h - 1, x + 1, curses.ACS_HLINE, w - 2)
        scr.vline(y + 1, x, curses.ACS_VLINE, h - 2)
        scr.vline(y + 1, x + w - 1, curses.ACS_VLINE, h - 2)
        scr.addch(y, x, curses.ACS_ULCORNER)
        scr.addch(y, x + w - 1, curses.ACS_URCORNER)
        scr.addch(y + h - 1, x, curses.ACS_LLCORNER)
        scr.addch(y + h - 1, x + w - 1, curses.ACS_LRCORNER)
    except curses.error:
        pass
    if title:
        put(scr, y, x + 2, " " + title + " ", tattr | curses.A_BOLD)
    return (x + 2, y + 1)


def field(scr, cx, y, label, value, vattr=0, lw=13):
    """One 'Label   value' row inside a panel."""
    put(scr, y, cx, label.ljust(lw), curses.A_DIM)
    put(scr, y, cx + lw, value, vattr)


# --- panels ----------------------------------------------------------------
def panel_battery(scr, y, x, w, s):
    t = s["telem"]
    inner = box(scr, y, x, 6, w, "POWER & BATTERY", C(C_ACCENT))
    if inner is None:
        return
    cx, cy = inner
    bw = max(6, w - 34)
    br = iget(t, "batR", -1)
    bl = iget(t, "batL", -1)
    vr = iget(t, "vR", -1)
    volts = "%.2fV" % (vr / 1000.0) if vr >= 0 else "  -  "
    put(scr, cy, cx, "Right cell", curses.A_DIM)
    put(
        scr,
        cy,
        cx + 11,
        "%3s%%  %s  %s" % (br if br >= 0 else "?", volts, bar(br, bw)),
        C(pct_pair(br)),
    )
    put(scr, cy + 1, cx, "Left cell", curses.A_DIM)
    put(
        scr,
        cy + 1,
        cx + 11,
        "%3s%%         %s" % (bl if bl >= 0 else "?", bar(bl, bw)),
        C(pct_pair(bl)),
    )
    d = s["drain"]
    if d is None:
        drain_s, dattr = "gathering… (needs ~3 min)", curses.A_DIM
    elif d["pct_hr"] < -0.05:
        drain_s, dattr = "charging  (+%.1f %%/hr)" % -d["pct_hr"], C(C_OK)
    else:
        left = "→ ~%.0f h to empty" % d["hrs_left"] if d["hrs_left"] else ""
        drain_s, dattr = "~%.1f %%/hr  %s" % (d["pct_hr"], left), 0
    field(scr, cx, cy + 2, "Drain (est)", drain_s, dattr)
    ep = s["extpower"]
    eptxt, epattr = (
        ("● ON", C(C_OK, True))
        if ep == 1
        else ("○ OFF", C(C_BAD, True))
        if ep == 0
        else ("? unknown", curses.A_DIM)
    )
    field(scr, cx, cy + 3, "Trackball rail", eptxt, epattr)


def panel_link(scr, y, x, w, s):
    t = s["telem"]
    inner = box(scr, y, x, 8, w, "WIRELESS LINK (host)", C(C_ACCENT))
    if inner is None:
        return
    cx, cy = inner
    out = t.get("out", "?").upper()
    prof = iget(t, "prof", -1)
    pconn = iget(t, "pconn", 0)
    field(scr, cx, cy, "Output", "%s   profile %s" % (out, prof))
    field(
        scr,
        cx,
        cy + 1,
        "Connection",
        "● connected" if pconn else "○ not connected",
        C(C_OK) if pconn else C(C_BAD),
    )
    rssi = iget(t, "rssi", RSSI_NA)
    q, qc = rssi_quality(rssi)
    rtxt = "%d dBm  (%s)" % (rssi, q) if rssi != RSSI_NA else "n/a (USB only?)"
    field(scr, cx, cy + 2, "Signal", rtxt, C(qc) if qc else curses.A_DIM)
    ci, lat, sto = iget(t, "ci", 0), iget(t, "lat", 0), iget(t, "sto", 0)
    field(
        scr,
        cx,
        cy + 3,
        "Interval",
        "%.1f ms  (latency %d)" % (ci * 1.25, lat) if ci else "—",
    )
    field(
        scr,
        cx,
        cy + 4,
        "Supervision",
        "%.1f s timeout" % (sto * 10 / 1000.0) if sto else "—",
    )
    periph = iget(t, "periph", 0)
    field(
        scr,
        cx,
        cy + 5,
        "Split halves",
        "%d peripheral linked" % periph if periph > 0 else "0 — LEFT down?",
        C(C_OK) if periph > 0 else C(C_WARN),
    )


def panel_compute(scr, y, x, w, s):
    t = s["telem"]
    inner = box(scr, y, x, 5, w, "COMPUTE & THERMAL", C(C_ACCENT))
    if inner is None:
        return
    cx, cy = inner
    bw = max(6, w - 30)
    cpu = iget(t, "cpu", -1)
    put(scr, cy, cx, "CPU load".ljust(13), curses.A_DIM)
    put(
        scr,
        cy,
        cx + 13,
        "%3s%% %s %s" % (cpu if cpu >= 0 else "?", bar(max(cpu, 0), bw), cpu_word(cpu)),
        C(load_pair(cpu)),
    )
    temp = iget(t, "temp", TEMP_NA)
    field(
        scr,
        cx,
        cy + 1,
        "Die temp",
        "%.1f °C" % (temp / 10.0) if temp != TEMP_NA else "n/a (no driver)",
    )
    tbr = iget(t, "tbr", 0)
    age = s["age"] or 0
    rate = tbr / age if age > 0 else 0
    field(scr, cx, cy + 2, "Trackball", "%d ev/poll   %.0f ev/s" % (tbr, rate))


def panel_state(scr, y, x, w, s):
    t = s["telem"]
    inner = box(scr, y, x, 5, w, "STATE", C(C_ACCENT))
    if inner is None:
        return
    cx, cy = inner
    aname, anote, aattr = ACT_DESC.get(t.get("act"), ("?", "", 0))
    field(
        scr,
        cx,
        cy,
        "Activity",
        "%s  (%s)" % (aname, anote),
        C(aattr) if aattr else curses.A_DIM,
    )
    rname, rattr = RESET_DESC.get(t.get("rst"), (t.get("rst", "?"), 0))
    field(
        scr,
        cx,
        cy + 1,
        "Last reset",
        "%s — %s" % (t.get("rst", "?"), rname),
        C(rattr) if rattr else curses.A_DIM,
    )
    field(scr, cx, cy + 2, "HID endpoint", t.get("out", "?").upper())


def render_header(scr, s, link, W):
    inner = box(scr, 0, 0, 3, W, None, 0)
    put(scr, 0, 2, " CHARYBDIS · right-half telemetry ", C(C_ACCENT, True))
    if s["connected"]:
        conn, cattr = "● Connected", C(C_OK, True)
    else:
        conn, cattr = "○ Waiting for device", C(C_BAD, True)
    put(scr, 0, max(2, W - len(conn) - 3), conn, cattr)
    if inner:
        cx, cy = inner
        t = s["telem"]
        up = fmt_hms(iget(t, "up", 0)) if t else "—"
        age = "%.1fs ago" % s["age"] if s["age"] else "—"
        aattr = C(C_WARN) if (s["age"] and s["age"] > 12) else curses.A_DIM
        put(scr, cy, cx, "firmware ", curses.A_DIM)
        put(scr, cy, cx + 9, link.fw, C(C_INFO))
        put(scr, cy, cx + 9 + len(link.fw) + 3, "uptime ", curses.A_DIM)
        put(scr, cy, cx + 9 + len(link.fw) + 10, up)
        put(scr, cy, cx + 9 + len(link.fw) + 10 + len(up) + 3, "updated ", curses.A_DIM)
        put(scr, cy, cx + 9 + len(link.fw) + 10 + len(up) + 11, age, aattr)


def render_footer(scr, s, H, W):
    keys = "[p] power toggle   [o] on   [x] off   [s] threads   [q] quit"
    put(scr, H - 1, 2, keys, C(C_INFO))
    msg = s["last_msg"]
    if msg:
        put(scr, H - 1, max(2, W - len(msg) - 3), "› " + msg, curses.A_DIM)


def render_main(scr, s, W):
    y0 = 3
    if W >= 84:
        wl = (W - 1) // 2
        wr = W - wl - 1
        xr = wl + 1
        panel_battery(scr, y0, 0, wl, s)
        panel_compute(scr, y0 + 6, 0, wl, s)
        panel_link(scr, y0, xr, wr, s)
        panel_state(scr, y0 + 8, xr, wr, s)
    else:
        panel_battery(scr, y0, 0, W, s)
        panel_link(scr, y0 + 6, 0, W, s)
        panel_compute(scr, y0 + 14, 0, W, s)
        panel_state(scr, y0 + 19, 0, W, s)


def render_threads(scr, s, H, W):
    rows = s["threads"]
    h = max(4, H - 5)
    inner = box(
        scr, 3, 0, h, W, "THREADS · stack headroom (most-starved first)", C(C_ACCENT)
    )
    if inner is None:
        return
    cx, cy = inner
    bw = max(8, W - 46)
    put(
        scr,
        cy,
        cx,
        "%-18s %6s %6s  %-*s %5s"
        % ("thread", "free", "size", bw, "stack used", "cpu%"),
        curses.A_DIM,
    )
    if not rows:
        put(scr, cy + 1, cx, "… requesting (charybdis threads)", curses.A_DIM)
        return
    for i, r in enumerate(rows):
        ry = cy + 1 + i
        if ry >= 3 + h - 1:
            put(
                scr,
                ry,
                cx,
                "… %d more (enlarge window)" % (len(rows) - i),
                curses.A_DIM,
            )
            break
        size = r["size"] or 1
        free_pct = 100.0 * r["free"] / size
        used = bar(100 - free_pct, bw)
        put(
            scr,
            ry,
            cx,
            "%-18s %6d %6d  %s %4.0f%%"
            % (r["name"][:18], r["free"], r["size"], used, r["cpu"]),
            C(pct_pair(free_pct)),
        )


# --- main loop -------------------------------------------------------------
def draw(stdscr, link):
    curses.curs_set(0)
    stdscr.nodelay(True)
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(C_OK, curses.COLOR_GREEN, -1)
    curses.init_pair(C_WARN, curses.COLOR_YELLOW, -1)
    curses.init_pair(C_BAD, curses.COLOR_RED, -1)
    curses.init_pair(C_ACCENT, curses.COLOR_CYAN, -1)
    curses.init_pair(C_INFO, curses.COLOR_BLUE, -1)
    curses.init_pair(C_ALT, curses.COLOR_MAGENTA, -1)

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
        if H < 12 or W < 40:
            put(stdscr, 0, 0, "Window too small — enlarge.", C(C_WARN))
            stdscr.refresh()
            time.sleep(0.2)
            continue

        render_header(stdscr, s, link, W)
        if s["alert"]:
            put(stdscr, 3, 2, "⚠ FAULT: " + s["alert"], C(C_BAD, True))
            render_footer(stdscr, s, H, W)
        elif mode == "threads":
            render_threads(stdscr, s, H, W)
            render_footer(stdscr, s, H, W)
        else:
            render_main(stdscr, s, W)
            render_footer(stdscr, s, H, W)

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
