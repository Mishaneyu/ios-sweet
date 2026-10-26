#!/usr/bin/env python3
"""Cross-platform live debugger & observability suite for a booted iOS phone.

Talks to bsd/dev/sweet_files.c + bsd/dev/sweet_diag.c (Kernel 0132+, TCP 2223)
over Wi-Fi (en0) or USB Ethernet (ue0 / FlameBridge) on Windows, macOS, or Linux.

Quick start:
  python tools/sweet-dbg.py connect 192.168.1.42   # or: connect --scan
  python tools/sweet-dbg.py status                 # health, FPS, GPU/Turnip clients, crashes
  python tools/sweet-dbg.py doctor                 # automated root-cause checks & remedies
  python tools/sweet-dbg.py watch                  # live terminal TUI + scrolling KLOG
  python tools/sweet-dbg.py web                    # live browser dashboard + remote touch
  python tools/sweet-dbg.py klog -f                # lossless live XNU console tail over TCP
  python tools/sweet-dbg.py crashes                # userland crash ring (+ ipsw symbolication)
  python tools/sweet-dbg.py shot                   # auto-geometry PNG screenshot
  python tools/sweet-dbg.py cmd "fbmode cpu"       # live display/backlight/touch/poke commands
  python tools/sweet-dbg.py snapshot --redact      # save redacted debug bundle to logs/
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.parse
import zlib

DEFAULT_PORT = 2223
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAVED_HOST_PATH = os.path.join(REPO_ROOT, "work", ".sweet-host")
LOGS_DIR = os.path.join(REPO_ROOT, "logs")


def _enable_win_vt():
    if os.name != "nt":
        return
    try:
        import ctypes
        k32 = ctypes.windll.kernel32
        h = k32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if k32.GetConsoleMode(h, ctypes.byref(mode)):
            k32.SetConsoleMode(h, mode.value | 0x0004)  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
    except Exception:
        pass


_enable_win_vt()
USE_COLOR = sys.stdout.isatty() and os.environ.get("NO_COLOR") is None


def C(code, text):
    return f"\033[{code}m{text}\033[0m" if USE_COLOR else str(text)


def B(t):   return C("1", t)
def DIM(t): return C("2", t)
def GRN(t): return C("32", t)
def YLW(t): return C("33", t)
def RED(t): return C("31", t)
def CYN(t): return C("36", t)


def badge(state, label=None):
    txt = label or state
    if state == "OK":
        return C("1;32", f"[{txt}]")
    if state == "WARN":
        return C("1;33", f"[{txt}]")
    if state == "FAIL":
        return C("1;31", f"[{txt}]")
    return C("2", f"[{txt}]")


def bar(pct, width=16):
    pct = max(0, min(100, int(round(pct))))
    fill = (pct * width + 50) // 100
    body = "#" * fill + "-" * (width - fill)
    col = "31" if pct >= 85 else ("33" if pct >= 50 else "32")
    return "[" + C(col, body) + "]"


# ---- TCP 2223 client (DIAG / KLOG / CMD / GET / READ) ----

def parse_host_port(spec, default_port=DEFAULT_PORT):
    spec = spec.strip()
    if ":" in spec:
        h, p = spec.rsplit(":", 1)
        return h.strip(), int(p)
    return spec, default_port


def resolve_endpoint(explicit=None):
    if explicit:
        return parse_host_port(explicit)
    env = os.environ.get("SWEET_HOST", "").strip()
    if env:
        return parse_host_port(env)
    if os.path.isfile(SAVED_HOST_PATH):
        try:
            saved = open(SAVED_HOST_PATH, "r", encoding="utf-8").read().strip()
            if saved:
                return parse_host_port(saved)
        except OSError:
            pass
    for cand in (("127.0.0.1", DEFAULT_PORT), ("192.168.7.2", DEFAULT_PORT)):
        try:
            with socket.create_connection(cand, timeout=1.2):
                return cand
        except OSError:
            continue
    return ("192.168.7.2", DEFAULT_PORT)


class SweetClient:
    def __init__(self, host, port=DEFAULT_PORT, timeout=8.0):
        self.host = host
        self.port = port
        self.timeout = timeout

    @property
    def label(self):
        return f"{self.host}:{self.port}"

    def _connect(self, timeout=None):
        return socket.create_connection((self.host, self.port), timeout=timeout or self.timeout)

    @staticmethod
    def _recv_line(sock):
        buf = bytearray()
        while not buf.endswith(b"\n"):
            ch = sock.recv(1)
            if not ch:
                raise ConnectionError("phone closed connection before header newline")
            buf += ch
            if len(buf) > 512:
                raise ValueError("oversized reply header")
        return buf.decode("utf-8", errors="replace").strip()

    @staticmethod
    def _recv_exact(sock, n):
        out = bytearray()
        while len(out) < n:
            chunk = sock.recv(min(262144, n - len(out)))
            if not chunk:
                raise ConnectionError(f"short read ({len(out)} of {n} bytes)")
            out += chunk
        return bytes(out)

    def diag(self, timeout=None):
        with self._connect(timeout=timeout) as s:
            s.sendall(b"DIAG\n")
            hdr = self._recv_line(s)
            if not hdr.startswith("OK "):
                raise RuntimeError(
                    f"phone replied '{hdr}' to DIAG (is Kernel 0132+ booted on {self.label}?)"
                )
            n = int(hdr.split()[1])
            raw = self._recv_exact(s, n).decode("utf-8", errors="replace")
            return json.loads(raw)

    def klog(self, cursor=0, max_bytes=65536, timeout=None):
        with self._connect(timeout=timeout) as s:
            s.sendall(f"KLOG {int(cursor)} {int(max_bytes)}\n".encode("ascii"))
            hdr = self._recv_line(s)
            if not hdr.startswith("OK "):
                raise RuntimeError(f"phone replied '{hdr}' to KLOG")
            parts = hdr.split()
            new_cursor = int(parts[1])
            got = int(parts[2])
            data = self._recv_exact(s, got) if got > 0 else b""
            return new_cursor, data

    def cmd(self, command_line, timeout=15.0):
        line = command_line.strip()
        with self._connect(timeout=timeout) as s:
            s.sendall(f"CMD {line}\n".encode("utf-8"))
            hdr = self._recv_line(s)
            if not hdr.startswith("OK "):
                raise RuntimeError(f"phone replied '{hdr}' to CMD")
            n = int(hdr.split()[1])
            data = self._recv_exact(s, n) if n > 0 else b""
            return data.decode("utf-8", errors="replace")

    def get_file(self, remote_path, max_bytes=512 * 1024, timeout=None):
        with self._connect(timeout=timeout) as s:
            s.sendall(f"GET {remote_path}\n".encode("utf-8"))
            hdr = self._recv_line(s)
            if not hdr.startswith("OK "):
                return None
            n = int(hdr.split()[1])
            raw = self._recv_exact(s, n)
            if len(raw) > max_bytes:
                raw = raw[-max_bytes:]
            return raw

    def read_range(self, remote_path, offset, length, timeout=25.0):
        with self._connect(timeout=timeout) as s:
            s.sendall(f"READ {remote_path} {int(offset)} {int(length)}\n".encode("ascii"))
            hdr = self._recv_line(s)
            if not hdr.startswith("OK "):
                raise RuntimeError(f"READ {remote_path} failed: {hdr}")
            n = int(hdr.split()[1])
            return self._recv_exact(s, n)


# ---- PNG encoder & framebuffer screenshot helper ----

def encode_png_rgb(w, h, rgb_rows):
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + r for r in rgb_rows)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 4))
        + chunk(b"IEND", b"")
    )


def capture_framebuffer_png(client, diag=None, scale=1, fmt="bgra"):
    if diag is None:
        diag = client.diag()
    disp = diag.get("display", {})
    pa = int(disp.get("panel_pa") or "0x9d000000", 0)
    w = int(disp.get("panel_w") or 1080)
    h = int(disp.get("panel_h") or 2400)
    stride = int(disp.get("panel_stride") or (w * 4))
    scale = max(1, int(scale))

    t0 = time.time()
    raw = client.read_range("/dev/pmem", pa, stride * h, timeout=30.0)
    dt = time.time() - t0

    order = (2, 1, 0) if fmt == "bgra" else (0, 1, 2)
    out_w = w // scale
    out_h = h // scale
    step = 4 * scale
    rows = []
    nonblack = 0
    for oy in range(out_h):
        y = oy * scale
        row = raw[y * stride : y * stride + out_w * step]
        rgb = bytearray(out_w * 3)
        r_ch = row[order[0]::step]
        g_ch = row[order[1]::step]
        b_ch = row[order[2]::step]
        rgb[0::3] = r_ch
        rgb[1::3] = g_ch
        rgb[2::3] = b_ch
        if any(r_ch) or any(g_ch) or any(b_ch):
            nonblack += 1
        rows.append(bytes(rgb))

    png_bytes = encode_png_rgb(out_w, out_h, rows)
    centre_off = (h // 2) * stride + (w // 2) * 4
    meta = {
        "pa": pa,
        "panel_w": w,
        "panel_h": h,
        "stride": stride,
        "out_w": out_w,
        "out_h": out_h,
        "bytes_read": len(raw),
        "elapsed_s": dt,
        "centre_hex": raw[centre_off : centre_off + 4].hex(),
        "nonblack_row_pct": round(nonblack * 100.0 / max(1, out_h), 1),
    }
    return png_bytes, raw, meta


# ---- Doctor: automated root-cause diagnostics ----

SIG_NAMES = {
    4: "SIGILL (illegal instruction / PAC / UDF)",
    5: "SIGTRAP (brk / os_crash / Swift fatalError)",
    6: "SIGABRT (abort / abort_with_payload / missing dylib or entitlement)",
    9: "SIGKILL (jetsam / AMFI / watchdog)",
    10: "SIGBUS (bus alignment / unmapped page)",
    11: "SIGSEGV (bad memory access at FAR)",
}


def run_doctor_checks(diag, mtl2_log=None, wland_log=None):
    findings = []  # list of dict(level="OK"|"WARN"|"FAIL", area=..., summary=..., fix=...)

    def add(level, area, summary, fix=""):
        findings.append({"level": level, "area": area, "summary": summary, "fix": fix})

    sys_d = diag.get("system", {})
    disp = diag.get("display", {})
    gpu = diag.get("gpu", {})
    inp = diag.get("input", {})
    net = diag.get("network", {})
    per = diag.get("peripherals", {})
    rt = diag.get("runtime", {})

    # 1. Watchdog
    rem_s = sys_d.get("reboot_remaining_s", -1)
    if sys_d.get("wdt_enabled") and 0 <= rem_s <= 90:
        add("WARN", "Watchdog", f"flamereboot countdown has only {rem_s}s left before hardware reset.",
            "Boot with flamereboot=0 (or a larger window like flamereboot=1800) once bring-up is stable.")
    else:
        add("OK", "Watchdog", f"CPU online={sys_d.get('cpus_online')} clocks={sys_d.get('cpufreq_mhz')} MHz"
            + (f", flamereboot={rem_s}s remaining" if rem_s >= 0 else ", continuous WDT pet"))

    # 2. Display & Frame Rate
    pmode = disp.get("present_mode", "unknown")
    fps1 = disp.get("fps_1s", 0.0)
    fps5 = disp.get("fps_5s", 0.0)
    avg_us = disp.get("avg_present_us", 0)
    ls = disp.get("last_swap", {})
    fourcc = ls.get("fourcc", "none")
    reason = disp.get("last_fallback_reason", "none")

    if not disp.get("registered"):
        add("FAIL", "Display", "AppleCLCD framebuffer is not registered.",
            "Check device-tree display node and SweetFramebuffer::start in klog.")
    elif not disp.get("power_on"):
        add("WARN", "Display", "Display power state is OFF (screen asleep / dimmed out).",
            "Press Power button or run: python tools/sweet-dbg.py cmd 'tap 540 1200'")
    elif disp.get("presents_total", 0) == 0:
        add("FAIL", "Display", "0 frames presented to panel — backboardd has not swapped a surface yet.",
            "Check crashes & klog for backboardd / RenderBox / CoreAnimation startup errors.")
    elif pmode == "cpu_fallback":
        add("WARN", "Display",
            f"Display is using slow CPU copyRow fallback ({fps1:.1f} fps, avg {avg_us} us/frame); "
            f"reason='{reason}', surface FourCC='{fourcc}'.",
            "If forced via fbmode, run: python tools/sweet-dbg.py cmd 'fbmode direct'. "
            "If FourCC is unexpected, inspect presentGPU() in SweetFramebuffer.cpp.")
    elif avg_us > 16667:
        add("WARN", "Display",
            f"Present time {avg_us} us exceeds 60 Hz 16.6 ms budget (1s={fps1:.1f} fps, 5s={fps5:.1f} fps).",
            "Check sweetddr=1 DDR bandwidth vote and GPU busy %.")
    else:
        add("OK", "Display",
            f"{pmode} active: {fps1:.1f} fps (1s) / {fps5:.1f} fps (5s), avg {avg_us} us, "
            f"surf#{ls.get('surface_id')} {ls.get('w')}x{ls.get('h')} {fourcc}, "
            f"brightness {disp.get('backlight', {}).get('brightness_pct', 100)}%.")

    # 3. GPU & Turnip Metal2 Clients
    clients = gpu.get("clients", [])
    v2_names = {c.get("name") for c in clients if c.get("abi") == "v2_turnip"}
    if not gpu.get("enabled"):
        add("WARN", "GPU/Metal", "Adreno 640 driver is disabled (sweetgpu=1 not in boot-args).",
            "Boot with sweetgpu=1 sweetmtl=2 for hardware GPU blits and Turnip Metal2.")
    elif not gpu.get("ready"):
        add("FAIL", "GPU/Metal", "Adreno 640 enabled in boot-args but gpu.ready is false (init/GMU/zap failed).",
            "Run: python tools/sweet-dbg.py klog -g 'SWEET GPU|a6xx' to inspect firmware/zap init.")
    elif "backboardd" not in v2_names:
        add("WARN", "GPU/Metal",
            f"Adreno 640 ready, but backboardd is NOT attached as a v2_turnip client (open clients: {[c.get('name') for c in clients]}).",
            "Ensure sweetmtl=2 is set and /System/Library/Extensions/SweetMetal2.bundle is installed.")
    else:
        add("OK", "GPU/Metal",
            f"Adreno 640 @ {gpu.get('clock_mhz')} MHz, {len(clients)} Metal client(s) open "
            f"(v2_turnip: {sorted(v2_names)}), busy={gpu.get('busy_pct_live')}%.")

    # 4. GPU Pool & Recoveries
    p_free = gpu.get("pool_free_mb", 0)
    p_res = gpu.get("pool_reserve_mb", 160)
    p_fail = gpu.get("pool_alloc_failures", 0)
    cap_hit = [c.get("name") for c in clients if c.get("cap_refused")]
    if gpu.get("recoveries", 0) > 0 or gpu.get("window_5s", {}).get("missed", 0) > 0:
        add("WARN", "GPU Stability",
            f"GPU recoveries={gpu.get('recoveries')}, 5s missed fences={gpu.get('window_5s', {}).get('missed', 0)}.",
            "Check klog for hang/fault IOVA and /var/local/mtl2.log for pipeline issues.")
    if p_fail > 0 or cap_hit or (gpu.get("pool_total_mb", 0) > 0 and p_free < p_res):
        add("WARN", "GPU Pool",
            f"Pool free={p_free} MB (reserve={p_res} MB), alloc_failures={p_fail}, cap_hit={cap_hit}.",
            "Tune sweetgpucap= / sweetgpureserve= boot-args or close heavy background apps.")

    if mtl2_log:
        err_lines = [
            ln.strip() for ln in mtl2_log.splitlines()
            if any(k in ln for k in ("ERROR", "VK_ERROR", "abort", "ABORT", "fault", "UNSUPPORTED"))
        ]
        if err_lines:
            add("WARN", "mtl2.log", f"{len(err_lines)} error/warning line(s) in /var/local/mtl2.log; latest: {err_lines[-1][:120]}",
                "Inspect /var/local/mtl2.log via: python tools/flame-files.py get /var/local/mtl2.log logs/mtl2.log")

    # 5. Input
    if not inp.get("touch_ready"):
        add("WARN", "Touch", f"Touch controller ({inp.get('touch_driver')}) is not marked ready.",
            "Check RMX TOUCH / SWEET FTS lines in klog.")
    elif inp.get("touch_errors", 0) > 0:
        add("WARN", "Touch", f"{inp.get('touch_errors')} touch bus read error(s) on {inp.get('touch_driver')}.",
            "Check I2C/SPI pin state in klog.")
    else:
        add("OK", "Touch", f"{inp.get('touch_driver')} ready ({inp.get('touch_events')} touch events, {inp.get('button_events')} key events).")

    # 6. Network
    wifi = net.get("wifi", {})
    usb = net.get("usb", {})
    if wifi.get("link_up") and not wifi.get("en0_ip"):
        add("WARN", "Wi-Fi", f"Associated to '{wifi.get('ssid')}' ({wifi.get('rssi_dbm')} dBm) but en0 has no IPv4 address yet.",
            "Wait for configd DHCP or run 'netsvc en0' on the phone.")
    elif wifi.get("radio_ready") and not wifi.get("link_up"):
        hint = ""
        if wland_log:
            tail = [ln.strip() for ln in wland_log.splitlines() if ln.strip()][-2:]
            if tail:
                hint = f" (wland.log: {' | '.join(tail)[:110]})"
        add("WARN", "Wi-Fi", f"WCN3990 radio ready ({wifi.get('bss_heard')} BSS heard) but 802.11 link is down{hint}.",
            "Verify /var/local/wlan.conf SSID/PSK on the phone.")
    elif wifi.get("link_up") and wifi.get("en0_ip"):
        add("OK", "Wi-Fi", f"Connected to '{wifi.get('ssid')}' ({wifi.get('rssi_dbm')} dBm, {wifi.get('freq_mhz')} MHz), en0={wifi.get('en0_ip')}.")
    else:
        add("WARN", "Wi-Fi", f"wland_open={wifi.get('wland_open')}, radio_ready={wifi.get('radio_ready')} (USB configured={usb.get('configured')}).",
            "Check /var/local/wland.log if Wi-Fi was expected.")

    # 7. Peripherals
    bat = per.get("battery", {})
    aud = per.get("audio", {})
    if bat.get("gauge_online") and bat.get("soc_pct", 100) <= 10 and not bat.get("charging"):
        add("WARN", "Battery", f"Battery low: {bat.get('soc_pct')}% ({bat.get('voltage_mv')} mV), discharging.",
            "Connect USB power so voltage does not droop under GPU load.")
    elif bat.get("gauge_online"):
        add("OK", "Peripherals",
            f"Battery {bat.get('soc_pct')}% ({bat.get('voltage_mv')} mV, {bat.get('current_ma'):+d} mA), "
            f"Audio={'ready' if aud.get('ready') else 'off'} ({aud.get('nonsilent_ms', 0)} ms audio), "
            f"Torch={'ready' if per.get('torch', {}).get('ready') else 'off'}, "
            f"Haptic={'ready' if per.get('haptic', {}).get('ready') else 'off'}.")

    # 8. Crashes & Sandbox
    crashes = rt.get("recent_crashes", [])
    c_total = rt.get("crashes_total", 0)
    if c_total > 0:
        counts = {}
        for c in crashes:
            nm = c.get("name", "?")
            counts[nm] = counts.get(nm, 0) + 1
        top = ", ".join(f"{k}x{v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1]))
        last = crashes[-1] if crashes else {}
        sig_desc = SIG_NAMES.get(last.get("sig"), f"sig {last.get('sig')}")
        add("WARN" if "SpringBoard" not in counts and "backboardd" not in counts else "FAIL",
            "Crashes",
            f"{c_total} fatal userland signal(s) since boot ({top}); latest {last.get('name')}[{last.get('pid')}] {sig_desc} pc={last.get('pc')} far={last.get('far')}.",
            "Run: python tools/sweet-dbg.py crashes   (symbolicates automatically if ipsw + dyld_shared_cache are present).")
    else:
        add("OK", "Crashes", "0 fatal userland crashes since boot.")

    denied = rt.get("sandbox_denied", [])
    if denied:
        summary = ", ".join(f"{d.get('proc')}(priv {d.get('priv')})" for d in denied[:6])
        add("WARN", "Sandbox", f"{rt.get('sandbox_denied_total', len(denied))} sandbox privilege denial(s): {summary}",
            "Check sweet_amfi.c allowlist if a daemon is failing a required syscall.")

    return findings


# ---- Pretty terminal status & TUI formatters ----

def format_uptime(ms):
    s = int(ms) // 1000
    return f"{s // 3600}h {(s % 3600) // 60}m {s % 60}s"


def unslide_hex(addr_s, slide_s):
    try:
        a = int(addr_s, 0)
        sl = int(slide_s, 0)
        u = (a - sl) if (sl and a > sl) else a
        return f"0x{u:x}"
    except ValueError:
        return addr_s


def render_status_text(diag, host_label=""):
    lines = []
    sys_d = diag.get("system", {})
    disp = diag.get("display", {})
    gpu = diag.get("gpu", {})
    inp = diag.get("input", {})
    net = diag.get("network", {})
    per = diag.get("peripherals", {})
    rt = diag.get("runtime", {})

    cf = sys_d.get("cpufreq_mhz", [0, 0, 0])
    rem_s = sys_d.get("reboot_remaining_s", -1)
    wdt_str = (
        badge("WARN" if rem_s < 90 else "OK", f"WDT {rem_s}s left")
        if (sys_d.get("wdt_enabled") and rem_s >= 0)
        else (badge("OK", "WDT petted") if sys_d.get("wdt_enabled") else badge("INFO", "WDT off"))
    )
    hdr_host = f" ({host_label})" if host_label else ""
    lines.append(
        f"{B('=== SWEET LIVE OS DEBUGGER' + hdr_host + ' ===')}  "
        f"up {format_uptime(sys_d.get('uptime_ms', 0))}  "
        f"CPUs={sys_d.get('cpus_online', 0)} ({'/'.join(str(x) for x in cf)} MHz)  {wdt_str}"
    )
    lines.append(f"  boot-args   : {DIM(sys_d.get('boot_args', ''))}")

    # Display
    pmode = disp.get("present_mode", "unknown")
    d_state = "OK" if pmode.startswith("gpu_") else ("WARN" if disp.get("power_on") else "INFO")
    fps1 = disp.get("fps_1s", 0.0)
    fps5 = disp.get("fps_5s", 0.0)
    ls = disp.get("last_swap", {})
    bl = disp.get("backlight", {})
    lines.append("")
    lines.append(
        f"{CYN('[DISPLAY]')} {badge(d_state, pmode)}  "
        f"FPS: {B(f'{fps1:.1f}')} (1s) / {fps5:.1f} (5s)  "
        f"vsync={disp.get('vsync_hz', 60)}Hz (#{disp.get('vsync_count', 0)})  "
        f"avg_present={disp.get('avg_present_us', 0)} us"
    )
    lines.append(
        f"  Geometry    : panel {disp.get('panel_w')}x{disp.get('panel_h')} "
        f"(stride {disp.get('panel_stride')}, PA {disp.get('panel_pa')})  "
        f"logical {disp.get('logical_w')}x{disp.get('logical_h')}  fit={disp.get('fit_rect')}"
    )
    lines.append(
        f"  Last Swap   : surf#{ls.get('surface_id')} {ls.get('w')}x{ls.get('h')} "
        f"bpr={ls.get('bpr')} fourcc={B(ls.get('fourcc', 'none'))} age={ls.get('age_ms')}ms  "
        f"presents: gpu={disp.get('gpu_presents')} cpu={disp.get('cpu_presents')}"
    )
    lines.append(
        f"  Backlight   : {bl.get('brightness_pct', 100)}% "
        f"({bl.get('millinits', 0) / 1000.0:.1f} nits, dim_alpha={bl.get('dim_alpha', 0)}/255)  "
        f"CPU fallback: count={disp.get('gpu_fallback_count', 0)} last='{disp.get('last_fallback_reason', 'none')}'"
    )

    # GPU
    g_state = "OK" if gpu.get("ready") else ("WARN" if not gpu.get("enabled") else "FAIL")
    busy = gpu.get("busy_pct_live", 0)
    w5 = gpu.get("window_5s", {})
    lines.append("")
    lines.append(
        f"{CYN('[GPU & METAL]')} {badge(g_state, 'Adreno 640 ' + ('READY' if gpu.get('ready') else 'OFF'))}  "
        f"{gpu.get('clock_mhz', 0)} MHz (L{gpu.get('perf_level', 0)})  "
        f"busy {bar(busy)} {busy}%  recoveries={gpu.get('recoveries', 0)}"
    )
    lines.append(
        f"  Pool        : {gpu.get('pool_used_mb', 0)} / {gpu.get('pool_total_mb', 0)} MB "
        f"(free {gpu.get('pool_free_mb', 0)} MB, peak {gpu.get('pool_peak_mb', 0)} MB, "
        f"cap {gpu.get('pool_cap_mb', 0)} MB, reserve {gpu.get('pool_reserve_mb', 0)} MB, "
        f"fail={gpu.get('pool_alloc_failures', 0)})"
    )
    owners = ", ".join(
        f"{o.get('name')}:{o.get('submits')}/{o.get('busy_ms')}ms" for o in gpu.get("owners_5s", [])
    ) or "idle"
    lines.append(
        f"  5s Window   : {w5.get('submits', 0)} submits (missed {w5.get('missed', 0)}), "
        f"mean {w5.get('mean_us', 0)} us, max {w5.get('longest_us', 0)} us, "
        f"blit {gpu.get('blit_us', 0)} us | owners: {owners}"
    )
    clients = gpu.get("clients", [])
    if clients:
        lines.append(f"  Clients ({len(clients)}) : {'PID':<6} {'PROCESS':<20} {'ABI':<11} {'POOL_MB':>8} {'PEAK_MB':>8} {'SUBMITS':>9}  FLAGS")
        for c in clients:
            flags = []
            if c.get("system"):
                flags.append(GRN("SYS"))
            if c.get("cap_refused"):
                flags.append(YLW("CAP_HIT"))
            if c.get("faults", 0):
                flags.append(RED(f"FAULTS={c['faults']}"))
            lines.append(
                f"                {c.get('pid', 0):<6} {c.get('name', '?'):<20} "
                f"{c.get('abi', '?'):<11} {c.get('pool_kb', 0) / 1024.0:>8.1f} "
                f"{c.get('pool_peak_kb', 0) / 1024.0:>8.1f} {c.get('submits', 0):>9}  "
                f"{' '.join(flags)}"
            )
    else:
        lines.append(f"  Clients (0) : {YLW('no IOAccelerator user clients open (CoreAnimation using CPU software renderer)')}")

    # Input & Network
    wifi = net.get("wifi", {})
    usb = net.get("usb", {})
    lines.append("")
    lines.append(f"{CYN('[INPUT & NETWORK]')}")
    t_badge = badge("OK" if inp.get("touch_ready") and inp.get("touch_errors", 0) == 0 else "WARN", inp.get("touch_driver", "none"))
    lines.append(
        f"  Touch/Keys  : {t_badge}  events={inp.get('touch_events', 0)} err={inp.get('touch_errors', 0)} "
        f"fingers=0x{inp.get('fingers_mask', 0):x} last=({inp.get('last_x', 0)},{inp.get('last_y', 0)}) "
        f"{inp.get('last_touch_age_ms', 0)}ms ago | keys={inp.get('button_events', 0)}"
    )
    w_state = "OK" if wifi.get("link_up") and wifi.get("en0_ip") else ("WARN" if wifi.get("radio_ready") else "INFO")
    lines.append(
        f"  Wi-Fi (en0) : {badge(w_state, 'LINK_UP' if wifi.get('link_up') else 'DOWN')}  "
        f"ssid='{wifi.get('ssid', '')}' rssi={wifi.get('rssi_dbm', 0)} dBm {wifi.get('freq_mhz', 0)} MHz  "
        f"ip={B(wifi.get('en0_ip') or 'none')} (rx={wifi.get('en0_rx_pkts', 0)} tx={wifi.get('en0_tx_pkts', 0)})"
    )
    lines.append(
        f"  USB (ue0)   : {badge('OK' if usb.get('configured') else 'INFO', 'CONFIGURED' if usb.get('configured') else 'idle')}  "
        f"ip={usb.get('ue0_ip') or 'none'} (rx={usb.get('rx_frames', 0)} tx={usb.get('tx_frames', 0)})  TCP=2223"
    )

    # Peripherals & Runtime
    bat = per.get("battery", {})
    aud = per.get("audio", {})
    lines.append("")
    lines.append(f"{CYN('[PERIPHERALS & RUNTIME]')}")
    if bat.get("gauge_online"):
        tc = bat.get("temp_deci_c", 0) / 10.0
        b_mode = "charging" if bat.get("charging") else ("full" if bat.get("full") else "discharging")
        lines.append(
            f"  Battery     : {B(str(bat.get('soc_pct', 0)) + '%')}  {bat.get('voltage_mv', 0)} mV  "
            f"{bat.get('current_ma', 0):+d} mA  {tc:.1f} C  "
            f"({bat.get('rem_mah', 0)}/{bat.get('fcc_mah', 0)} mAh, {b_mode}, i2c_err={bat.get('i2c_errors', 0)})"
        )
    else:
        lines.append("  Battery     : gauge offline")
    lines.append(
        f"  Audio/Misc  : audio={'STREAMING' if aud.get('streaming_now') else ('ready' if aud.get('ready') else 'off')} "
        f"(sound {aud.get('nonsilent_ms', 0) / 1000.0:.1f}s / {aud.get('written_ms', 0) / 1000.0:.1f}s, peak={aud.get('peak_pct', 0)}%)  "
        f"torch={per.get('torch', {}).get('level', 0)}  haptics={per.get('haptic', {}).get('activations', 0)}"
    )
    lines.append(
        f"  Runtime     : exec_inject hits={rt.get('exec_inject_hits', 0)} "
        f"rule={DIM(repr(rt.get('exec_inject', '')))}  sandbox_denied={rt.get('sandbox_denied_total', 0)}"
    )
    c_total = rt.get("crashes_total", 0)
    if c_total == 0:
        lines.append(f"  Crashes     : {GRN('0 userland fatal signals since boot')}")
    else:
        lines.append(f"  Crashes     : {RED(f'{c_total} fatal userland signals')} (recent {len(rt.get('recent_crashes', []))}):")
        for c in rt.get("recent_crashes", [])[-5:]:
            unslid = unslide_hex(c.get("pc", "0x0"), c.get("slide", "0x0"))
            lines.append(
                f"    #{c.get('seq')} [+{c.get('uptime_ms', 0) / 1000.0:.2f}s] "
                f"{B(c.get('name', '?'))}[{c.get('pid')}] sig={c.get('sig')} "
                f"pc={c.get('pc')} (unslid {unslid}) lr={c.get('lr')} far={c.get('far')}"
            )
    return "\n".join(lines)


# ---- Crash symbolication via ipsw (optional) ----

def symbolicate_addrs(addrs, cache_path):
    ipsw = shutil.which("ipsw")
    if not ipsw or not cache_path or not os.path.isfile(cache_path) or not addrs:
        return {}
    out_map = {}
    for a in sorted(set(addrs)):
        try:
            cp = subprocess.run(
                [ipsw, "dyld", "a2s", cache_path, a],
                capture_output=True, text=True, timeout=6
            )
            txt = (cp.stdout or cp.stderr or "").strip().splitlines()
            if txt:
                out_map[a] = txt[-1].strip()
        except Exception:
            break
    return out_map


# ---- Embedded zero-dependency live Web Dashboard ----

WEB_HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sweet Live iOS Debugger</title>
<style>
  :root {
    --bg: #0b0f17; --card: #131a27; --border: #233047; --text: #e6edf7;
    --muted: #8b9bb4; --ok: #2dd4bf; --warn: #f59e0b; --fail: #f43f5e; --accent: #38bdf8;
  }
  * { box-sizing: border-box; }
  body { margin: 0; font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, sans-serif; background: var(--bg); color: var(--text); }
  header { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 12px; padding: 12px 18px; background: #0f1522; border-bottom: 1px solid var(--border); position: sticky; top: 0; z-index: 10; }
  .title { font-weight: 700; font-size: 16px; display: flex; align-items: center; gap: 10px; }
  .pill { padding: 3px 9px; border-radius: 999px; font-size: 12px; font-weight: 600; background: #1e293b; color: var(--muted); }
  .pill.ok { background: rgba(45,212,191,0.16); color: var(--ok); }
  .pill.warn { background: rgba(245,158,11,0.18); color: var(--warn); }
  .pill.fail { background: rgba(244,63,94,0.2); color: var(--fail); }
  .grid { display: grid; grid-template-columns: 1fr 340px; gap: 14px; padding: 14px 18px; }
  @media (max-width: 1100px) { .grid { grid-template-columns: 1fr; } }
  .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(270px, 1fr)); gap: 12px; margin-bottom: 12px; }
  .card { background: var(--card); border: 1px solid var(--border); border-radius: 10px; padding: 12px 14px; }
  .card h3 { margin: 0 0 8px 0; font-size: 13px; text-transform: uppercase; letter-spacing: 0.05em; color: var(--muted); display: flex; justify-content: space-between; align-items: center; }
  .big { font-size: 26px; font-weight: 700; font-variant-numeric: tabular-nums; }
  .kv { font-size: 12.5px; color: var(--muted); line-height: 1.55; font-variant-numeric: tabular-nums; }
  .kv b { color: var(--text); font-weight: 600; }
  svg.spark { width: 100%; height: 46px; display: block; margin-top: 6px; background: rgba(0,0,0,0.22); border-radius: 6px; }
  table { width: 100%; border-collapse: collapse; font-size: 12.5px; font-variant-numeric: tabular-nums; }
  th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid rgba(255,255,255,0.06); }
  th { color: var(--muted); font-weight: 600; }
  button, input, select { background: #1e293b; color: var(--text); border: 1px solid var(--border); border-radius: 6px; padding: 5px 10px; font-size: 12px; cursor: pointer; }
  button:hover { border-color: var(--accent); }
  .controls { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; margin-top: 8px; }
  #klog { height: 270px; overflow-y: auto; background: #070a10; border: 1px solid var(--border); border-radius: 8px; padding: 8px 10px; font: 11.5px/1.45 ui-monospace, SFMono-Regular, Consolas, monospace; white-space: pre-wrap; word-break: break-all; color: #cbd5e1; }
  .screen-wrap { display: flex; flex-direction: column; align-items: center; gap: 8px; }
  #screenImg { width: 100%; max-width: 310px; border-radius: 14px; border: 2px solid var(--border); background: #05070b; cursor: crosshair; user-select: none; -webkit-user-drag: none; }
  .finding { padding: 6px 9px; border-radius: 6px; margin-bottom: 6px; font-size: 12px; border-left: 3px solid var(--ok); background: rgba(255,255,255,0.03); }
  .finding.WARN { border-left-color: var(--warn); }
  .finding.FAIL { border-left-color: var(--fail); }
</style>
</head>
<body>
<header>
  <div class="title">
    <span>Sweet Live iOS Debugger</span>
    <span id="connPill" class="pill">Connecting...</span>
    <span id="hostPill" class="pill"></span>
  </div>
  <div class="kv" id="sysBar">--</div>
</header>

<div class="grid">
  <div>
    <div class="cards">
      <div class="card">
        <h3>Display &amp; Compositor <span id="dispBadge" class="pill">--</span></h3>
        <div class="big"><span id="fps1">0.0</span> <span style="font-size:13px;color:var(--muted)">fps (1s) / <span id="fps5">0.0</span> (5s)</span></div>
        <svg id="fpsSpark" class="spark" viewBox="0 0 240 46" preserveAspectRatio="none"></svg>
        <div class="kv" id="dispKv"></div>
        <div class="controls">
          <button onclick="sendCmd('fbmode direct')">Direct Swap</button>
          <button onclick="sendCmd('fbmode vsync')">Vsync</button>
          <button onclick="sendCmd('fbmode cpu')">Force CPU</button>
          <button onclick="sendCmd('fbmode gpu')">Allow GPU</button>
        </div>
        <div class="controls">
          <span class="kv">Brightness:</span>
          <input type="range" min="5" max="100" value="80" id="briSlider" onchange="sendCmd('brightness ' + this.value)">
          <span class="kv" id="briVal">80%</span>
        </div>
      </div>

      <div class="card">
        <h3>Adreno 640 &amp; Pool <span id="gpuBadge" class="pill">--</span></h3>
        <div class="big"><span id="gpuBusy">0</span>% <span style="font-size:13px;color:var(--muted)">busy @ <span id="gpuMhz">0</span> MHz</span></div>
        <svg id="gpuSpark" class="spark" viewBox="0 0 240 46" preserveAspectRatio="none"></svg>
        <div class="kv" id="gpuKv"></div>
      </div>

      <div class="card">
        <h3>Input, Wi-Fi &amp; Peripherals <span id="netBadge" class="pill">--</span></h3>
        <div class="kv" id="ioKv"></div>
        <div class="controls">
          <button onclick="sendCmd('sync')">Sync FS</button>
          <button onclick="sendCmd('rtc')">Read RTC</button>
        </div>
      </div>
    </div>

    <div class="card" style="margin-bottom:12px">
      <h3>Automated Doctor Findings</h3>
      <div id="doctorBox" class="kv">Waiting for first snapshot...</div>
    </div>

    <div class="card" style="margin-bottom:12px">
      <h3>Active Metal / Turnip Clients (<span id="clientCount">0</span>)</h3>
      <table>
        <thead><tr><th>PID</th><th>Process</th><th>ABI</th><th>Pool MB</th><th>Peak MB</th><th>Submits</th><th>Flags</th></tr></thead>
        <tbody id="clientsBody"></tbody>
      </table>
    </div>

    <div class="card" style="margin-bottom:12px">
      <h3>Userland Crash Ring (<span id="crashCount">0</span>)</h3>
      <table>
        <thead><tr><th>#</th><th>Uptime</th><th>Process</th><th>Sig</th><th>PC (Unslid)</th><th>LR</th><th>FAR</th></tr></thead>
        <tbody id="crashBody"></tbody>
      </table>
    </div>

    <div class="card">
      <h3>
        <span>Live Kernel Console (sweet_log_ring)</span>
        <span style="display:flex;gap:6px;align-items:center">
          <input id="klogFilter" placeholder="Filter substring..." style="width:150px" oninput="renderKlog()">
          <button onclick="klogPaused=!klogPaused;this.textContent=klogPaused?'Resume':'Pause'">Pause</button>
          <button onclick="klogLines=[];renderKlog()">Clear</button>
        </span>
      </h3>
      <div id="klog"></div>
      <div class="controls">
        <input id="customCmd" placeholder="Kernel CMD (e.g. r 0x05000000 4, tap 540 1200, fbmode direct)" style="flex:1" onkeydown="if(event.key==='Enter')runCustomCmd()">
        <button onclick="runCustomCmd()">Run CMD</button>
      </div>
    </div>
  </div>

  <div class="card screen-wrap">
    <h3 style="width:100%">Live Framebuffer &amp; Touch <span id="shotMeta" class="pill">idle</span></h3>
    <img id="screenImg" alt="Click 'Refresh Screen' to read /dev/pmem" draggable="false">
    <div class="controls" style="justify-content:center;width:100%">
      <button onclick="refreshShot()">Refresh Screen</button>
      <label class="kv"><input type="checkbox" id="autoShot"> Auto (1.5s)</label>
      <select id="shotScale"><option value="2" selected>1/2x fast</option><option value="4">1/4x</option><option value="1">1x full</option></select>
    </div>
    <div class="kv" style="text-align:center">Click image to <b>tap</b>, or drag to <b>swipe</b> on the booted phone.</div>
  </div>
</div>

<script>
const fpsHist = [], gpuHist = [];
let klogCursor = -1, klogPaused = false, klogLines = [];
let panelW = 1080, panelH = 2400, dragStart = null;

function pushHist(arr, v) { arr.push(v); if (arr.length > 60) arr.shift(); }

function drawSpark(id, arr, maxV, color) {
  const svg = document.getElementById(id);
  if (!arr.length) { svg.innerHTML = ''; return; }
  const pts = arr.map((v, i) => {
    const x = (i / Math.max(1, 59)) * 240;
    const y = 44 - Math.min(1, Math.max(0, v / maxV)) * 40;
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(' ');
  svg.innerHTML = `<polyline fill="none" stroke="${color}" stroke-width="2" points="${pts}"/>`;
}

function unslide(pcStr, slideStr) {
  try {
    const pc = BigInt(pcStr), sl = BigInt(slideStr);
    const u = (sl > 0n && pc > sl) ? (pc - sl) : pc;
    return '0x' + u.toString(16);
  } catch (e) { return pcStr; }
}

async function pollDiag() {
  try {
    const r = await fetch('/api/diag');
    const j = await r.json();
    if (!j.ok) throw new Error(j.error || 'diag error');
    const d = j.diag;
    document.getElementById('connPill').className = 'pill ok';
    document.getElementById('connPill').textContent = 'LIVE';
    document.getElementById('hostPill').textContent = j.host;

    const s = d.system || {}, disp = d.display || {}, g = d.gpu || {}, inp = d.input || {}, net = d.network || {}, per = d.peripherals || {}, rt = d.runtime || {};
    panelW = disp.panel_w || 1080; panelH = disp.panel_h || 2400;
    const upS = Math.floor((s.uptime_ms || 0) / 1000);
    const wdt = (s.wdt_enabled && s.reboot_remaining_s >= 0) ? ` | WDT ${s.reboot_remaining_s}s` : '';
    document.getElementById('sysBar').innerHTML = `Uptime <b>${Math.floor(upS/60)}m ${upS%60}s</b> | CPUs <b>${s.cpus_online}</b> (${(s.cpufreq_mhz||[]).join('/')} MHz)${wdt}`;

    const pm = disp.present_mode || 'unknown';
    const db = document.getElementById('dispBadge');
    db.textContent = pm; db.className = 'pill ' + (pm.startsWith('gpu_') ? 'ok' : 'warn');
    document.getElementById('fps1').textContent = (disp.fps_1s || 0).toFixed(1);
    document.getElementById('fps5').textContent = (disp.fps_5s || 0).toFixed(1);
    pushHist(fpsHist, disp.fps_1s || 0);
    drawSpark('fpsSpark', fpsHist, 65, '#2dd4bf');

    const ls = disp.last_swap || {}, bl = disp.backlight || {};
    document.getElementById('briVal').textContent = (bl.brightness_pct ?? 100) + '%';
    document.getElementById('dispKv').innerHTML =
      `Panel <b>${disp.panel_w}x${disp.panel_h}</b> (${disp.panel_pa}) | Logical <b>${disp.logical_w}x${disp.logical_h}</b><br>` +
      `Swap surf#${ls.surface_id} <b>${ls.w}x${ls.h}</b> fmt=<b>${ls.fourcc}</b> | avg <b>${disp.avg_present_us} us</b><br>` +
      `GPU/CPU presents: <b>${disp.gpu_presents}</b> / <b>${disp.cpu_presents}</b> | fallback: <b>${disp.last_fallback_reason}</b>`;

    const gb = document.getElementById('gpuBadge');
    gb.textContent = g.ready ? 'READY' : 'OFF'; gb.className = 'pill ' + (g.ready ? 'ok' : 'fail');
    document.getElementById('gpuBusy').textContent = g.busy_pct_live || 0;
    document.getElementById('gpuMhz').textContent = g.clock_mhz || 0;
    pushHist(gpuHist, g.busy_pct_live || 0);
    drawSpark('gpuSpark', gpuHist, 100, '#38bdf8');
    const w5 = g.window_5s || {};
    document.getElementById('gpuKv').innerHTML =
      `Pool <b>${g.pool_used_mb}/${g.pool_total_mb} MB</b> (free ${g.pool_free_mb} MB, peak ${g.pool_peak_mb} MB, fail ${g.pool_alloc_failures})<br>` +
      `5s submits: <b>${w5.submits||0}</b> (mean ${w5.mean_us||0} us, max ${w5.longest_us||0} us) | recoveries: <b>${g.recoveries||0}</b><br>` +
      `Owners: <b>${(g.owners_5s||[]).map(o => o.name + ':' + o.busy_ms + 'ms').join(', ') || 'idle'}</b>`;

    const wf = (net.wifi || {}), bat = (per.battery || {}), aud = (per.audio || {});
    const nb = document.getElementById('netBadge');
    nb.textContent = wf.link_up ? (wf.en0_ip || 'DHCP...') : 'Wi-Fi down';
    nb.className = 'pill ' + ((wf.link_up && wf.en0_ip) ? 'ok' : 'warn');
    document.getElementById('ioKv').innerHTML =
      `Touch <b>${inp.touch_driver}</b>: ev=${inp.touch_events} err=${inp.touch_errors} last=(${inp.last_x},${inp.last_y})<br>` +
      `Wi-Fi <b>${wf.ssid || '-'}</b> (${wf.rssi_dbm} dBm) en0=<b>${wf.en0_ip || 'none'}</b><br>` +
      `Battery <b>${bat.soc_pct}%</b> (${bat.voltage_mv} mV, ${bat.current_ma} mA) | Audio: <b>${aud.streaming_now ? 'STREAMING' : (aud.ready ? 'ready' : 'off')}</b> (${((aud.nonsilent_ms||0)/1000).toFixed(1)}s)<br>` +
      `exec_inject hits: <b>${rt.exec_inject_hits}</b> | sandbox denied: <b>${rt.sandbox_denied_total}</b>`;

    // Doctor
    document.getElementById('doctorBox').innerHTML = (j.findings || []).map(f =>
      `<div class="finding ${f.level}"><b>[${f.level}] ${f.area}:</b> ${f.summary}${f.fix ? '<br><span style="color:var(--muted)">Fix: ' + f.fix + '</span>' : ''}</div>`
    ).join('');

    // Clients
    const cls = g.clients || [];
    document.getElementById('clientCount').textContent = cls.length;
    document.getElementById('clientsBody').innerHTML = cls.map(c =>
      `<tr><td>${c.pid}</td><td><b>${c.name}</b></td><td>${c.abi}</td><td>${(c.pool_kb/1024).toFixed(1)}</td><td>${(c.pool_peak_kb/1024).toFixed(1)}</td><td>${c.submits}</td><td>${c.system?'SYS ':''}${c.cap_refused?'CAP_HIT ':''}${c.faults?'FAULT('+c.faults+')':''}</td></tr>`
    ).join('') || '<tr><td colspan="7" style="color:var(--muted)">No active Metal user clients</td></tr>';

    // Crashes
    const crs = rt.recent_crashes || [];
    document.getElementById('crashCount').textContent = rt.crashes_total || 0;
    document.getElementById('crashBody').innerHTML = crs.slice().reverse().map(c =>
      `<tr><td>${c.seq}</td><td>+${(c.uptime_ms/1000).toFixed(1)}s</td><td><b>${c.name}</b>[${c.pid}]</td><td>${c.sig}</td><td>${c.pc} (${unslide(c.pc, c.slide)})</td><td>${c.lr}</td><td>${c.far}</td></tr>`
    ).join('') || '<tr><td colspan="7" style="color:var(--ok)">0 fatal userland signals since boot</td></tr>';

    if (klogCursor < 0 && s.klog_bytes_total != null) {
      klogCursor = Math.max(0, s.klog_bytes_total - 32768);
    }
  } catch (e) {
    document.getElementById('connPill').className = 'pill fail';
    document.getElementById('connPill').textContent = 'OFFLINE';
  }
}

async function pollKlog() {
  if (klogPaused || klogCursor < 0) return;
  try {
    const r = await fetch('/api/klog?cursor=' + klogCursor);
    const j = await r.json();
    if (j.ok && j.text) {
      klogCursor = j.cursor;
      const parts = j.text.split('\n');
      for (const ln of parts) { if (ln.length) klogLines.push(ln); }
      if (klogLines.length > 1500) klogLines = klogLines.slice(-1500);
      renderKlog();
    } else if (j.ok) {
      klogCursor = j.cursor;
    }
  } catch (e) {}
}

function renderKlog() {
  const box = document.getElementById('klog');
  const f = document.getElementById('klogFilter').value.trim().toLowerCase();
  const view = f ? klogLines.filter(l => l.toLowerCase().includes(f)) : klogLines;
  box.textContent = view.slice(-400).join('\n');
  box.scrollTop = box.scrollHeight;
}

async function sendCmd(cmd) {
  const r = await fetch('/api/cmd', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({cmd}) });
  const j = await r.json();
  if (j.output) {
    klogLines.push('>>> CMD ' + cmd + ' -> ' + j.output.trim());
    renderKlog();
  }
  setTimeout(pollDiag, 150);
}

function runCustomCmd() {
  const el = document.getElementById('customCmd');
  if (el.value.trim()) { sendCmd(el.value.trim()); el.value = ''; }
}

let shotBusy = false;
async function refreshShot() {
  if (shotBusy) return;
  shotBusy = true;
  const scale = document.getElementById('shotScale').value;
  const t0 = performance.now();
  const img = document.getElementById('screenImg');
  img.onload = () => {
    document.getElementById('shotMeta').textContent = Math.round(performance.now() - t0) + ' ms';
    shotBusy = false;
  };
  img.onerror = () => { document.getElementById('shotMeta').textContent = 'err'; shotBusy = false; };
  img.src = '/api/shot?scale=' + scale + '&t=' + Date.now();
}

const imgEl = document.getElementById('screenImg');
function imgCoords(ev) {
  const r = imgEl.getBoundingClientRect();
  const x = Math.round(((ev.clientX - r.left) / Math.max(1, r.width)) * panelW);
  const y = Math.round(((ev.clientY - r.top) / Math.max(1, r.height)) * panelH);
  return [Math.max(0, Math.min(panelW - 1, x)), Math.max(0, Math.min(panelH - 1, y))];
}
imgEl.addEventListener('mousedown', ev => { dragStart = imgCoords(ev); });
imgEl.addEventListener('mouseup', ev => {
  if (!dragStart) return;
  const [x1, y1] = dragStart;
  const [x2, y2] = imgCoords(ev);
  dragStart = null;
  if (Math.hypot(x2 - x1, y2 - y1) < 18) sendCmd(`tap ${x1} ${y1}`);
  else sendCmd(`swipe ${x1} ${y1} ${x2} ${y2} 220`);
  setTimeout(refreshShot, 350);
});

pollDiag();
setInterval(pollDiag, 1000);
setInterval(pollKlog, 600);
setInterval(() => { if (document.getElementById('autoShot').checked) refreshShot(); }, 1500);
</script>
</body>
</html>
"""


def make_http_handler(client):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            return

        def _send_json(self, obj, code=200):
            body = json.dumps(obj).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(u.query)
            if u.path in ("/", "/index.html"):
                body = WEB_HTML.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if u.path == "/api/diag":
                try:
                    d = client.diag(timeout=3.0)
                    findings = run_doctor_checks(d)
                    self._send_json({"ok": True, "host": client.label, "diag": d, "findings": findings})
                except Exception as e:
                    self._send_json({"ok": False, "host": client.label, "error": str(e)}, code=502)
                return
            if u.path == "/api/klog":
                try:
                    cur = int(qs.get("cursor", ["0"])[0])
                    new_cur, raw = client.klog(cur, 65536, timeout=3.0)
                    self._send_json({
                        "ok": True,
                        "cursor": new_cur,
                        "text": raw.decode("utf-8", errors="replace"),
                    })
                except Exception as e:
                    self._send_json({"ok": False, "error": str(e)}, code=502)
                return
            if u.path == "/api/shot":
                try:
                    scale = max(1, min(8, int(qs.get("scale", ["2"])[0])))
                    png_bytes, _, _ = capture_framebuffer_png(client, scale=scale)
                    self.send_response(200)
                    self.send_header("Content-Type", "image/png")
                    self.send_header("Cache-Control", "no-store")
                    self.send_header("Content-Length", str(len(png_bytes)))
                    self.end_headers()
                    self.wfile.write(png_bytes)
                except Exception as e:
                    self._send_json({"ok": False, "error": str(e)}, code=502)
                return
            self.send_error(404)

        def do_POST(self):
            if self.path == "/api/cmd":
                try:
                    ln = int(self.headers.get("Content-Length", "0"))
                    payload = json.loads(self.rfile.read(ln).decode("utf-8"))
                    cmd_s = str(payload.get("cmd", "")).strip()
                    if not cmd_s:
                        self._send_json({"ok": False, "error": "empty cmd"}, code=400)
                        return
                    out = client.cmd(cmd_s, timeout=12.0)
                    self._send_json({"ok": True, "output": out})
                except Exception as e:
                    self._send_json({"ok": False, "error": str(e)}, code=502)
                return
            self.send_error(404)

    return Handler


# ---- LAN scanner for connect --scan ----

def detect_local_prefix():
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            parts = ip.split(".")
            if len(parts) == 4 and parts[0] != "127":
                return ".".join(parts[:3])
    except OSError:
        pass
    return None


def scan_subnet_for_phone(prefix, port=DEFAULT_PORT):
    def probe(ip):
        c = SweetClient(ip, port=port, timeout=0.35)
        try:
            d = c.diag(timeout=0.35)
            if isinstance(d, dict) and "display" in d and "system" in d:
                return (ip, d)
        except Exception:
            return None
        return None

    candidates = [f"{prefix}.{i}" for i in range(1, 255)]
    hits = []
    with ThreadPoolExecutor(max_workers=64) as ex:
        futs = [ex.submit(probe, ip) for ip in candidates]
        for fut in as_completed(futs):
            res = fut.result()
            if res:
                hits.append(res)
    return hits


# ---- Redaction helper for snapshot --redact ----

def redact_bundle(obj):
    wf = obj.get("diag", {}).get("network", {}).get("wifi", {})
    orig_ssid = wf.get("ssid", "")
    orig_ip = wf.get("en0_ip", "")
    obj = dict(obj)
    obj["host"] = "<redacted-host>"
    text = json.dumps(obj, indent=2)
    if orig_ssid:
        text = text.replace(json.dumps(orig_ssid)[1:-1], "<redacted-ssid>")
    if orig_ip:
        text = text.replace(orig_ip, "<redacted-ip>")
    # Scrub MACs/BSSIDs across diag, findings, klog_tail, and wland_log_tail
    text = re.sub(r"\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b", "xx:xx:xx:xx:xx:xx", text)
    return json.loads(text)


# ---- CLI Subcommand Implementations ----

def cmd_connect(args):
    port = DEFAULT_PORT
    target_ip = args.target
    if args.scan is not None or not target_ip:
        prefix = args.scan if isinstance(args.scan, str) and args.scan != "auto" else detect_local_prefix()
        if not prefix:
            raise SystemExit("Could not detect LAN /24 prefix; pass e.g. --scan 192.168.1 or an explicit IP.")
        print(f"Scanning {prefix}.1..254:{port} for a booted Sweet iOS kernel...")
        hits = scan_subnet_for_phone(prefix, port=port)
        if not hits:
            raise SystemExit(f"No booted Sweet iOS device responded on {prefix}.0/24:{port}.")
        target_ip, d = hits[0]
        spec = f"{target_ip}:{port}" if port != DEFAULT_PORT else target_ip
    else:
        h, p = parse_host_port(target_ip)
        c = SweetClient(h, p, timeout=4.0)
        d = c.diag(timeout=4.0)
        spec = f"{h}:{p}" if p != DEFAULT_PORT else h

    os.makedirs(os.path.dirname(SAVED_HOST_PATH), exist_ok=True)
    with open(SAVED_HOST_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(spec + "\n")
    disp = d.get("display", {})
    gpu = d.get("gpu", {})
    print(
        f"{badge('OK', 'CONNECTED')} Saved {B(spec)} to work/.sweet-host — "
        f"panel {disp.get('panel_w')}x{disp.get('panel_h')}, "
        f"{disp.get('present_mode')} ({disp.get('fps_1s', 0):.1f} fps), "
        f"GPU={'ready' if gpu.get('ready') else 'off'} ({len(gpu.get('clients', []))} Metal clients)"
    )


def cmd_status(args, client):
    d = client.diag()
    if args.json:
        print(json.dumps(d, indent=2))
        return
    print(render_status_text(d, client.label))


def cmd_doctor(args, client):
    d = client.diag()
    mtl2_raw = client.get_file("/var/local/mtl2.log")
    wland_raw = client.get_file("/var/local/wland.log")
    mtl2_txt = mtl2_raw.decode("utf-8", errors="replace") if mtl2_raw else None
    wland_txt = wland_raw.decode("utf-8", errors="replace") if wland_raw else None
    findings = run_doctor_checks(d, mtl2_log=mtl2_txt, wland_log=wland_txt)
    if args.json:
        print(json.dumps(findings, indent=2))
        return
    print(f"{B('=== SWEET AUTOMATED DOCTOR (' + client.label + ') ===')}")
    for f in findings:
        print(f"  {badge(f['level'])} {B(f['area'] + ':')} {f['summary']}")
        if f.get("fix"):
            print(f"         {DIM('-> ' + f['fix'])}")


def cmd_watch(args, client):
    cursor = None
    recent_klog = []
    try:
        while True:
            try:
                d = client.diag(timeout=2.5)
                if cursor is None:
                    total = d.get("system", {}).get("klog_bytes_total", 0)
                    cursor = max(0, total - 4096)
                cursor, raw = client.klog(cursor, 32768, timeout=2.5)
                if raw:
                    for ln in raw.decode("utf-8", errors="replace").splitlines():
                        if ln.strip():
                            recent_klog.append(ln)
                    recent_klog = recent_klog[-8:]
                out = "\033[H\033[2J" + render_status_text(d, client.label)
                out += f"\n\n{CYN('[LIVE KERNEL LOG TAIL]')} (cursor={cursor})\n"
                out += "\n".join("  " + ln[:140] for ln in recent_klog) if recent_klog else "  (quiet)"
                sys.stdout.write(out + "\n")
                sys.stdout.flush()
            except Exception as e:
                sys.stdout.write(f"\033[H\033[2J{badge('FAIL', 'DISCONNECTED')} {client.label}: {e}\nRetrying...\n")
                sys.stdout.flush()
            time.sleep(max(0.2, args.interval))
    except KeyboardInterrupt:
        print()


def cmd_klog(args, client):
    d = client.diag()
    total = d.get("system", {}).get("klog_bytes_total", 0)
    cursor = 0 if args.all else max(0, total - args.tail)
    rx = re.compile(args.grep) if args.grep else None
    save_f = open(args.save, "ab") if args.save else None
    carry = ""
    try:
        while True:
            new_cur, raw = client.klog(cursor, 131072)
            if raw:
                if save_f:
                    save_f.write(raw)
                    save_f.flush()
                txt = raw.decode("utf-8", errors="replace")
                if rx is None:
                    sys.stdout.write(txt)
                    sys.stdout.flush()
                else:
                    combined = carry + txt
                    lines = combined.splitlines(keepends=True)
                    carry = ""
                    if lines and not lines[-1].endswith("\n"):
                        carry = lines.pop()
                    for ln in lines:
                        if rx.search(ln):
                            sys.stdout.write(ln)
                    sys.stdout.flush()
                cursor = new_cur
                continue
            cursor = new_cur
            if not args.follow:
                if carry and rx and rx.search(carry):
                    sys.stdout.write(carry + "\n")
                break
            time.sleep(0.25)
    except KeyboardInterrupt:
        pass
    finally:
        if save_f:
            save_f.close()


def cmd_crashes(args, client):
    d = client.diag()
    rt = d.get("runtime", {})
    crashes = rt.get("recent_crashes", [])
    total = rt.get("crashes_total", 0)
    if total == 0:
        print(f"{badge('OK')} 0 fatal userland crashes since boot on {client.label}.")
        return

    cache = args.cache
    if not cache:
        default_cache = os.path.join(REPO_ROOT, "research", "dsc", "dyld_shared_cache_arm64")
        if os.path.isfile(default_cache):
            cache = default_cache

    to_sym = []
    for c in crashes:
        sl = c.get("slide", "0x0")
        to_sym.append(unslide_hex(c.get("pc", "0x0"), sl))
        to_sym.append(unslide_hex(c.get("lr", "0x0"), sl))
        for b in c.get("bt", []):
            to_sym.append(unslide_hex(b, sl))
    sym_map = symbolicate_addrs(to_sym, cache)

    print(f"{B(f'=== USERLAND CRASH RING ({total} total, showing {len(crashes)}) ===')}")
    for c in crashes:
        sl = c.get("slide", "0x0")
        upc = unslide_hex(c.get("pc", "0x0"), sl)
        ulr = unslide_hex(c.get("lr", "0x0"), sl)
        sig_desc = SIG_NAMES.get(c.get("sig"), f"sig={c.get('sig')}")
        pc_sym = f" -> {sym_map[upc]}" if upc in sym_map else ""
        lr_sym = f" -> {sym_map[ulr]}" if ulr in sym_map else ""
        print(
            f"\n#{c.get('seq')} [+{c.get('uptime_ms', 0) / 1000.0:.3f}s] "
            f"{B(c.get('name', '?'))}[{c.get('pid')}]  {RED(sig_desc)}"
        )
        print(f"  code={c.get('code')} sub={c.get('subcode')} far={c.get('far')} mh={c.get('mh')} slide={sl}")
        print(f"  pc = {c.get('pc')} (unslid {upc}){pc_sym}")
        print(f"  lr = {c.get('lr')} (unslid {ulr}){lr_sym}")
        bt = c.get("bt", [])
        if bt:
            print("  bt :")
            for idx, addr in enumerate(bt):
                ua = unslide_hex(addr, sl)
                bs = f"  {sym_map[ua]}" if ua in sym_map else ""
                print(f"    [{idx}] {addr} (unslid {ua}){bs}")


def cmd_shot(args, client):
    out_path = args.out
    if not out_path:
        os.makedirs(LOGS_DIR, exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        out_path = os.path.join(LOGS_DIR, f"shot-{ts}.png")
    else:
        parent = os.path.dirname(os.path.abspath(out_path))
        if parent:
            os.makedirs(parent, exist_ok=True)

    png_bytes, raw, meta = capture_framebuffer_png(client, scale=args.scale, fmt=args.fmt)
    with open(out_path, "wb") as f:
        f.write(png_bytes)
    if args.raw:
        with open(args.raw, "wb") as f:
            f.write(raw)
    mb = meta["bytes_read"] / 1e6
    dt = max(meta["elapsed_s"], 1e-6)
    print(
        f"{badge('OK', 'SHOT')} {out_path}: {meta['out_w']}x{meta['out_h']} "
        f"(panel {meta['panel_w']}x{meta['panel_h']} stride {meta['stride']} @ {meta['pa']:#x}) "
        f"{mb:.1f} MB in {dt:.2f}s ({mb / dt:.1f} MB/s), "
        f"centre={meta['centre_hex']}, non-black rows={meta['nonblack_row_pct']}%"
    )


def cmd_run_cmd(args, client):
    text = open(args.file, "r", encoding="utf-8").read() if args.file else " ".join(args.command)
    lines = [ln.strip() for ln in text.splitlines() if ln.strip() and not ln.strip().startswith("#")]
    if not lines:
        raise SystemExit("Nothing to send.")
    for ln in lines:
        out = client.cmd(ln)
        if out:
            sys.stdout.write(out if out.endswith("\n") else out + "\n")
        else:
            print(f"OK ({ln})")


def cmd_snapshot(args, client):
    out_path = args.out
    if not out_path:
        os.makedirs(LOGS_DIR, exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        out_path = os.path.join(LOGS_DIR, f"diag-{ts}.json")
    else:
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)

    d = client.diag()
    total = d.get("system", {}).get("klog_bytes_total", 0)
    _, klog_raw = client.klog(max(0, total - 131072), 131072)
    mtl2_raw = client.get_file("/var/local/mtl2.log")
    mtl2_cli = client.get_file("/var/local/mtl2-clients")
    wland_raw = client.get_file("/var/local/wland.log")

    mtl2_s = mtl2_raw.decode("utf-8", errors="replace") if mtl2_raw else None
    wland_s = wland_raw.decode("utf-8", errors="replace") if wland_raw else None
    bundle = {
        "captured_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": client.label,
        "diag": d,
        "findings": run_doctor_checks(d, mtl2_log=mtl2_s, wland_log=wland_s),
        "klog_tail": klog_raw.decode("utf-8", errors="replace"),
        "mtl2_clients": mtl2_cli.decode("utf-8", errors="replace") if mtl2_cli else None,
        "mtl2_log_tail": mtl2_s[-32768:] if mtl2_s else None,
        "wland_log_tail": wland_s[-16384:] if wland_s else None,
    }
    if args.redact:
        bundle = redact_bundle(bundle)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(bundle, f, indent=2)
    print(f"{badge('OK', 'SNAPSHOT')} Wrote diagnostic bundle to {out_path} ({os.path.getsize(out_path)} bytes)")


def cmd_web(args, client):
    srv = ThreadingHTTPServer((args.bind, args.port), make_http_handler(client))
    url = f"http://{args.bind}:{args.port}"
    print(f"{badge('OK', 'WEB')} Live dashboard listening on {B(url)} -> phone {B(client.label)}")
    print("  Open the URL in your browser for live FPS/GPU charts, display controls, remote touch & KLOG.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped web server.")
    finally:
        srv.server_close()


def build_parser():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--host", "-H", help="Phone IP[:PORT] (overrides $SWEET_HOST and work/.sweet-host)")
    sub = ap.add_subparsers(dest="sub")

    p_con = sub.add_parser("connect", help="Probe phone on TCP 2223 and save IP to work/.sweet-host")
    p_con.add_argument("target", nargs="?", help="Phone IP[:PORT]")
    p_con.add_argument("--scan", nargs="?", const="auto", default=None,
                       help="Scan /24 LAN subnet (e.g. 192.168.1, or auto-detect)")

    p_st = sub.add_parser("status", help="Show live OS, display FPS, GPU/Turnip clients & crash status")
    p_st.add_argument("--json", action="store_true", help="Emit raw JSON from DIAG")

    p_doc = sub.add_parser("doctor", help="Run automated root-cause checks & suggest fixes")
    p_doc.add_argument("--json", action="store_true", help="Emit findings as JSON")

    p_w = sub.add_parser("watch", help="Live terminal dashboard + kernel log tail")
    p_w.add_argument("-i", "--interval", type=float, default=1.0, help="Refresh interval seconds")

    p_web = sub.add_parser("web", help="Start local browser dashboard with live charts & remote touch")
    p_web.add_argument("--bind", default="127.0.0.1", help="Bind address (default 127.0.0.1)")
    p_web.add_argument("--port", type=int, default=8765, help="HTTP port (default 8765)")

    p_kl = sub.add_parser("klog", help="Read or follow lossless XNU console log over TCP 2223")
    p_kl.add_argument("-f", "--follow", action="store_true", help="Follow new log output continuously")
    p_kl.add_argument("-n", "--tail", type=int, default=65536, help="Initial tail bytes (default 65536)")
    p_kl.add_argument("--all", action="store_true", help="Read entire buffered ring from earliest byte")
    p_kl.add_argument("-g", "--grep", help="Filter lines matching regex")
    p_kl.add_argument("--save", help="Also append raw log bytes to file")

    p_cr = sub.add_parser("crashes", help="Inspect userland crash ring (+ optional ipsw symbolication)")
    p_cr.add_argument("--cache", help="Path to dyld_shared_cache_arm64 for ipsw symbolication")

    p_sh = sub.add_parser("shot", help="Capture auto-geometry PNG screenshot from /dev/pmem")
    p_sh.add_argument("out", nargs="?", help="Output PNG path (default logs/shot-TIMESTAMP.png)")
    p_sh.add_argument("--scale", type=int, default=1, help="Downsample factor 1, 2, or 4 (default 1)")
    p_sh.add_argument("--fmt", choices=["bgra", "rgba"], default="bgra")
    p_sh.add_argument("--raw", help="Also save raw framebuffer bytes")

    p_cmd = sub.add_parser("cmd", help="Run synchronous kernel command (fbmode, brightness, tap, swipe, poke)")
    p_cmd.add_argument("command", nargs="*", help="Command words (e.g. fbmode cpu, brightness 60, tap 540 1200)")
    p_cmd.add_argument("-f", "--file", help="Run commands from poke script file")

    p_sn = sub.add_parser("snapshot", help="Save full diagnostic bundle (DIAG + KLOG + userland logs) to JSON")
    p_sn.add_argument("out", nargs="?", help="Output JSON path (default logs/diag-TIMESTAMP.json)")
    p_sn.add_argument("--redact", action="store_true", help="Scrub Wi-Fi SSID, BSSID, MACs & IPs")

    return ap


def main(argv=None):
    ap = build_parser()
    args = ap.parse_args(argv)
    if args.sub == "connect":
        cmd_connect(args)
        return

    host, port = resolve_endpoint(args.host)
    client = SweetClient(host, port)
    sub = args.sub or "status"

    if sub == "status":
        cmd_status(args, client)
    elif sub == "doctor":
        cmd_doctor(args, client)
    elif sub == "watch":
        cmd_watch(args, client)
    elif sub == "web":
        cmd_web(args, client)
    elif sub == "klog":
        cmd_klog(args, client)
    elif sub == "crashes":
        cmd_crashes(args, client)
    elif sub == "shot":
        cmd_shot(args, client)
    elif sub == "cmd":
        cmd_run_cmd(args, client)
    elif sub == "snapshot":
        cmd_snapshot(args, client)
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
