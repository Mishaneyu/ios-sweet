#!/usr/bin/env python3
"""Send commands to the phone's kernel command runner over TCP 2223 ("CMD ...",
Kernel 0132+, works over Wi-Fi or USB on Windows/macOS/Linux) or fallback UDP 5141.

  tools/flame-cmd.py [--host IP] 'r 0x15000020 3'      one poke line (see bsd/dev/sweet_poke.c)
  tools/flame-cmd.py [--host IP] 'fbmode cpu'          switch live display mode (direct|vsync|cpu|gpu)
  tools/flame-cmd.py [--host IP] 'brightness 50'       set display brightness 0..100%
  tools/flame-cmd.py [--host IP] -f tools/poke/X.txt   a poke script file
  tools/flame-cmd.py [--host IP] reboot                sync + PSCI reset into fastboot
"""
import argparse, importlib.util, os, socket, sys, time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('flame_files', os.path.join(here, 'flame-files.py'))
ff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ff)


def try_tcp_cmd(host, port, text):
    """Run synchronous CMD over TCP 2223 (Kernel 0132+). Returns output str, or None on pre-0132."""
    lines = [ln.strip() for ln in text.splitlines() if ln.strip() and not ln.strip().startswith('#')]
    outputs = []
    for ln in lines:
        with ff.conn(host=host, port=port, timeout=10) as s:
            s.sendall(f'CMD {ln}\n'.encode())
            hdr = b''
            while not hdr.endswith(b'\n'):
                c = s.recv(1)
                if not c:
                    return None
                hdr += c
            hline = hdr.decode(errors='replace').strip()
            if not hline.startswith('OK '):
                return None
            n = int(hline.split()[1])
            buf = bytearray()
            while len(buf) < n:
                chunk = s.recv(min(65536, n - len(buf)))
                if not chunk:
                    break
                buf += chunk
            outputs.append(buf.decode('utf-8', errors='replace'))
    return ''.join(outputs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', nargs='*')
    ap.add_argument('--host', '-H', help='phone IP[:PORT] (else $SWEET_HOST or work/.sweet-host)')
    ap.add_argument('-f', '--file')
    ap.add_argument('--udp', action='store_true', help='force legacy UDP 5141/5140 path')
    ap.add_argument('--wait', type=float, default=3)
    ap.add_argument('--all', action='store_true', help='print every log line, not just results')
    a = ap.parse_args()
    text = open(a.file).read() if a.file else ' '.join(a.cmd)
    if not text.strip():
        ap.error('nothing to send')

    th, tp = ff.resolve_target(a.host) or (None, None)
    if not a.udp:
        try:
            out = try_tcp_cmd(th, tp, text)
            if out is not None:
                os.makedirs('logs', exist_ok=True)
                with open('logs/live.txt', 'ab') as f:
                    f.write(out.encode('utf-8', errors='replace'))
                sys.stdout.write(out)
                sys.stdout.flush()
                return
        except OSError:
            pass

    udp_host = th or '192.168.7.2'
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        rx.bind(('0.0.0.0', 5140))
        rx.settimeout(0.5)
    except OSError:
        print('(live log port busy: results go to the running live recorder)', file=sys.stderr)
        rx = None
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        tx.sendto(text.encode(), (udp_host, 5141))
    except OSError:
        tx.sendto(text.encode(), ('192.168.7.255', 5141))
    end = time.time() + a.wait if rx else 0
    os.makedirs('logs', exist_ok=True)
    with open('logs/live.txt', 'ab') as f:
        while time.time() < end:
            try:
                data, _ = rx.recvfrom(65536)
            except socket.timeout:
                continue
            f.write(data)
            for line in data.decode(errors='replace').splitlines():
                if a.all or 'POKE:' in line or 'SWEET CMD' in line:
                    print(line, flush=True)


if __name__ == '__main__':
    main()
