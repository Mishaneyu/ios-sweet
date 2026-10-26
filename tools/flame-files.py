#!/usr/bin/env python3
"""Files on the running phone (bsd/dev/sweet_files.c, TCP 2223 over Wi-Fi or USB/FlameBridge).

  tools/flame-files.py [--host IP[:PORT]] put LOCAL REMOTE [MODE]   (mode octal, default from LOCAL)
  tools/flame-files.py [--host IP[:PORT]] get REMOTE LOCAL
  tools/flame-files.py [--host IP[:PORT]] read REMOTE OFFSET LEN LOCAL   (range; works on devices: /dev/pmem at a PA)
  tools/flame-files.py [--host IP[:PORT]] mkdir REMOTE [MODE]
  tools/flame-files.py [--host IP[:PORT]] link TARGET REMOTE        (symlink)
  tools/flame-files.py [--host IP[:PORT]] rm REMOTE
  tools/flame-files.py [--host IP[:PORT]] chmod REMOTE MODE

Target host resolution: --host > $SWEET_HOST > work/.sweet-host > 127.0.0.1:2223 (or 192.168.7.2:2223).
"""
import os, socket, stat, sys

HOST, PORT = '127.0.0.1', 2223
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_SAVED_HOST_FILE = os.path.join(_REPO_ROOT, 'work', '.sweet-host')


def parse_host_port(spec, default_port=PORT):
    spec = spec.strip()
    if ':' in spec:
        h, p = spec.rsplit(':', 1)
        return h.strip(), int(p)
    return spec, default_port


def resolve_target(explicit=None):
    if explicit:
        return parse_host_port(explicit)
    env = os.environ.get('SWEET_HOST', '').strip()
    if env:
        return parse_host_port(env)
    if os.path.isfile(_SAVED_HOST_FILE):
        try:
            saved = open(_SAVED_HOST_FILE, 'r', encoding='utf-8').read().strip()
            if saved:
                return parse_host_port(saved)
        except OSError:
            pass
    return None


def conn(host=None, port=None, timeout=30):
    if host:
        return socket.create_connection((host, port or PORT), timeout=timeout)
    target = resolve_target()
    if target:
        return socket.create_connection(target, timeout=timeout)
    # Try FlameBridge localhost tunnel first, then direct USB CDC-ECM IP.
    last_err = None
    for cand in ((HOST, PORT), ('192.168.7.2', PORT)):
        try:
            return socket.create_connection(cand, timeout=2.0 if cand[0] == '127.0.0.1' else timeout)
        except OSError as e:
            last_err = e
    raise last_err


def reply(s):
    line = b''
    while not line.endswith(b'\n'):
        c = s.recv(1)
        if not c:
            raise SystemExit('connection closed without a reply')
        line += c
    line = line.decode(errors='replace').strip()
    if not line.startswith('OK'):
        raise SystemExit(f'phone: {line}')
    return line


def read_range(s, path, off, n):
    """READ (kernel 0094+): n bytes of path at off; one request per connection."""
    s.sendall(f'READ {path} {off} {n}\n'.encode())
    n = int(reply(s).split()[1])
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(min(1 << 20, n - len(buf)))
        if not chunk:
            raise SystemExit(f'short read: {len(buf)} of {n} bytes')
        buf += chunk
    return bytes(buf)


def diag_json(host=None, port=None, timeout=5):
    """DIAG (kernel 0132+): live kernel diagnostic JSON snapshot."""
    with conn(host=host, port=port, timeout=timeout) as s:
        s.sendall(b'DIAG\n')
        n = int(reply(s).split()[1])
        buf = bytearray()
        while len(buf) < n:
            chunk = s.recv(min(65536, n - len(buf)))
            if not chunk:
                raise SystemExit('short read on DIAG')
            buf += chunk
        return buf.decode('utf-8', errors='replace')


def main(argv):
    explicit_host = None
    while len(argv) >= 2 and argv[0] in ('--host', '-H'):
        explicit_host = argv[1]
        argv = argv[2:]
    if not argv or argv[0] in ('-h', '--help'):
        raise SystemExit(__doc__)
    th, tp = resolve_target(explicit_host) or (None, None)
    cmd, args = argv[0], argv[1:]
    s = conn(host=th, port=tp)
    if cmd == 'put':
        local, remote = args[0], args[1]
        mode = int(args[2], 8) if len(args) > 2 else (stat.S_IMODE(os.stat(local).st_mode) or 0o644)
        data = open(local, 'rb').read()
        s.sendall(f'PUT {remote} {mode:o} {len(data)}\n'.encode() + data)
        print(remote, reply(s))
    elif cmd == 'get':
        s.sendall(f'GET {args[0]}\n'.encode())
        n = int(reply(s).split()[1])
        buf = bytearray()
        while len(buf) < n:
            chunk = s.recv(min(65536, n - len(buf)))
            if not chunk:
                raise SystemExit('short read')
            buf += chunk
        open(args[1], 'wb').write(buf)
        print(args[0], n, 'bytes ->', args[1])
    elif cmd == 'read':
        data = read_range(s, args[0], int(args[1], 0), int(args[2], 0))
        open(args[3], 'wb').write(data)
        print(args[0], len(data), 'bytes ->', args[3])
    elif cmd in ('mkdir', 'link', 'rm', 'chmod'):
        verb = {'mkdir': 'MKDIR', 'link': 'LINK', 'rm': 'RM', 'chmod': 'CHMOD'}[cmd]
        if cmd == 'mkdir' and len(args) == 1:
            args = args + ['755']
        s.sendall(f'{verb} {" ".join(args)}\n'.encode())
        print(cmd, ' '.join(args), reply(s))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
