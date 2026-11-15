#!/usr/bin/env python3
"""Read the iOS console straight off the phone's CDC-ACM port.

The kernel presents itself as a CDC-ACM device (VID 0x1209, PID 0x0001, the
pid.codes test pair) and pushes the console ring out of bulk IN as a plain
byte stream, so whatever the COM port delivers is exactly the text the
screen shows.  Windows binds usbser.sys to it from the box -- its
usbser.inf matches "USB\\Class_02&SubClass_02&Prot_01", which is what
patch 0101 describes the comm interface as -- so no extra driver is needed.

    python console_log.py                 # auto-detect the port, write ios-console.log
    python console_log.py COM7 out.log    # use a specific port / file
    python console_log.py --pyserial      # use pyserial instead of the raw reader

Why this does not simply use pyserial
-------------------------------------
Opening a COM port through pyserial means SetCommState, SetCommTimeouts and
friends.  On this phone those hang.  Measured here:

    pyserial.Serial("COM5", 115200)        ->  OSError(22) "the semaphore timeout
                                               period has expired" (Win32 121)
    CreateFileW("\\\\.\\COM5") + ReadFile  ->  works

Windows sends three class requests when it opens a COM port -- SET_LINE_CODING,
GET_LINE_CODING, SET_CONTROL_LINE_STATE -- and the kernel's control-transfer
state machine does not complete the status stage of the first one, so the host
waits for a reply that never arrives.

The bulk IN path is a different endpoint and is unaffected, which is why the
raw reader still gets the log.  So the default path here opens the port and
reads it without touching any of the line settings.  pyserial is only a
fallback (--pyserial) for a kernel that does answer.

The read uses overlapped I/O with a real event, so a read that times out is
cancelled instead of leaving a pending operation on the handle -- otherwise
the second read on the same handle would fail.

Survives the device disappearing (reboot, fastboot) and keeps retrying.
"""
import ctypes
import os
import sys
import time
from ctypes import wintypes

VID = 0x1209          # pid.codes test VID the kernel presents
PID = 0x0001

OUT = "ios-console.log"
USE_PYSERIAL = "--pyserial" in sys.argv
REOPEN_IDLE = 1.0     # seconds without data before the raw port is reopened

GENERIC_READ = 0x80000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
FILE_ATTRIBUTE_NORMAL = 0x00000080
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
ERROR_IO_PENDING = 997
ERROR_OPERATION_ABORTED = 995
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
INFINITE = 0xFFFFFFFF


class OVERLAPPED(ctypes.Structure):
    """Windows OVERLAPPED; ctypes.wintypes does not define it."""

    _fields_ = [
        ("Internal", ctypes.c_ulong),
        ("InternalHigh", ctypes.c_ulong),
        ("Offset", wintypes.DWORD),
        ("OffsetHigh", wintypes.DWORD),
        ("hEvent", ctypes.c_void_p),
    ]


def find_port():
    """The COM port of the phone, by USB VID/PID, or by name."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    ports = list(list_ports.comports())
    for p in ports:
        if (p.vid, p.pid) == (VID, PID):
            return p.device
    cands = [p.device for p in ports
             if "USB 串行" in (p.description or "")
             or "USB Serial" in (p.description or "")]
    return cands[0] if cands else None


class RawPort:
    """A COM port read through Win32, without configuring the line."""

    def __init__(self, name):
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32 = self.k32

        k32.CreateFileW.restype = ctypes.c_void_p
        k32.CreateFileW.argtypes = [
            ctypes.c_wchar_p, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
            wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
        k32.ReadFile.restype = wintypes.BOOL
        k32.ReadFile.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(OVERLAPPED)]
        k32.GetOverlappedResult.restype = wintypes.BOOL
        k32.GetOverlappedResult.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(OVERLAPPED),
            ctypes.POINTER(wintypes.DWORD), wintypes.BOOL]
        k32.CreateEventW.restype = ctypes.c_void_p
        k32.CreateEventW.argtypes = [
            ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, ctypes.c_wchar_p]
        k32.SetEvent.restype = wintypes.BOOL
        k32.SetEvent.argtypes = [ctypes.c_void_p]
        k32.ResetEvent.restype = wintypes.BOOL
        k32.ResetEvent.argtypes = [ctypes.c_void_p]
        k32.WaitForSingleObject.restype = wintypes.DWORD
        k32.WaitForSingleObject.argtypes = [ctypes.c_void_p, wintypes.DWORD]
        k32.CancelIo.restype = wintypes.BOOL
        k32.CancelIo.argtypes = [ctypes.c_void_p]
        k32.CloseHandle.restype = wintypes.BOOL
        k32.CloseHandle.argtypes = [ctypes.c_void_p]

        self.name = name
        self.buf = ctypes.create_string_buffer(65536)
        self.nread = wintypes.DWORD()
        self.ov = OVERLAPPED()

        h = k32.CreateFileW(
            "\\\\.\\" + name,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL,
            None)
        if h is None or h == INVALID_HANDLE_VALUE:
            raise OSError(ctypes.get_last_error(),
                          "CreateFileW(\\\\.\\%s) failed" % name)
        self.h = h
        # Without explicit timeouts a ReadFile only completes once the whole
        # 64 KiB buffer is full; the kernel sends 1 KiB every 100 ms, so every
        # read timed out and was cancelled. MAXDWORD/MAXDWORD/500 = return as
        # soon as any byte has arrived, or after 500 ms. SetCommTimeouts is a
        # local driver setting: unlike SetCommState it sends no USB request.
        to = (wintypes.DWORD * 5)(0xFFFFFFFF, 0xFFFFFFFF, 500, 0, 0)
        if not k32.SetCommTimeouts(ctypes.c_void_p(self.h), ctypes.byref(to)):
            print("SetCommTimeouts failed: %d" % ctypes.get_last_error(), flush=True)
        self.ev = k32.CreateEventW(None, True, False, None)
        if not self.ev:
            k32.CloseHandle(self.h)
            raise OSError(ctypes.get_last_error(), "CreateEventW failed")

    def read(self, timeout=0.5):
        """Read whatever is waiting; returns b"" on timeout."""
        k32 = self.k32
        k32.ResetEvent(self.ev)
        self.nread.value = 0
        ctypes.memset(ctypes.byref(self.ov), 0, ctypes.sizeof(self.ov))
        self.ov.hEvent = self.ev

        ok = k32.ReadFile(self.h, self.buf, 65536,
                          ctypes.byref(self.nread), ctypes.byref(self.ov))
        if not ok:
            err = ctypes.get_last_error()
            if err == ERROR_IO_PENDING:
                r = k32.WaitForSingleObject(self.ev, int(timeout * 1000))
                if r == WAIT_TIMEOUT:
                    # Cancel, then reap the completion so the handle stays
                    # usable for the next read.
                    k32.CancelIo(self.h)
                    k32.WaitForSingleObject(self.ev, INFINITE)
                    k32.GetOverlappedResult(self.h, ctypes.byref(self.ov),
                                            ctypes.byref(self.nread), False)
                    # keep what the cancelled read had already received
                    return self.buf.raw[:self.nread.value]
                # completed: the byte count of an overlapped read is only
                # available from GetOverlappedResult (nread stays 0)
                if not k32.GetOverlappedResult(self.h, ctypes.byref(self.ov),
                                               ctypes.byref(self.nread), False):
                    err = ctypes.get_last_error()
                    if err in (ERROR_OPERATION_ABORTED, 1167, 6):
                        raise OSError(err, "the device disappeared")
                    raise OSError(err, "GetOverlappedResult failed")
            elif err in (ERROR_OPERATION_ABORTED, 1167, 6):
                raise OSError(err, "the device disappeared")
            else:
                raise OSError(err, "ReadFile failed")
        n = self.nread.value
        if not n:
            return b""
        return self.buf.raw[:n]

    def close(self):
        try:
            self.k32.CloseHandle(self.ev)
        except Exception:
            pass
        try:
            self.k32.CloseHandle(self.h)
        except Exception:
            pass


def open_port(name):
    if USE_PYSERIAL:
        import serial
        return serial.Serial(name, 115200, timeout=0.5)
    return RawPort(name)


def main():
    # positional: [port] [outfile]
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want = None
    out_path = OUT
    if len(args) >= 1 and args[0] != "-":
        want = args[0]
    if len(args) >= 2:
        out_path = args[1]
    out = open(out_path, "ab", buffering=0)
    mode = "pyserial" if USE_PYSERIAL else "raw win32 (no line config)"
    print("writing to %s (Ctrl-C to stop), mode: %s"
          % (os.path.abspath(out_path), mode), flush=True)
    total = 0
    reopened = False
    while True:
        port = want or find_port()
        if not port:
            time.sleep(1.0)
            continue
        try:
            ser = open_port(port)
        except Exception as e:
            print("open %s failed: %s" % (port, e), flush=True)
            time.sleep(1.0)
            continue
        if not reopened:
            print("=== %s open ===" % port, flush=True)
        reopened = False
        raw = isinstance(ser, RawPort)
        idle_since = time.time()
        try:
            while True:
                # RawPort.read takes a timeout in seconds, pyserial a size.
                buf = ser.read(0.5) if raw else ser.read(4096)
                if buf:
                    out.write(buf)
                    total += len(buf)
                    sys.stdout.write(buf.decode("utf-8", "replace"))
                    sys.stdout.flush()
                    idle_since = time.time()
                elif raw and time.time() - idle_since > REOPEN_IDLE:
                    # Windows' usbser stops polling bulk IN after one
                    # completed transfer from this gadget; a fresh open
                    # makes it read the next one the phone has queued.
                    ser.close()
                    reopened = True
                    break
        except KeyboardInterrupt:
            try:
                ser.close()
            except Exception:
                pass
            out.close()
            print("\n[stopped, %d bytes]" % total, flush=True)
            return 0
        except Exception as e:
            print("\n=== lost %s: %s (%d bytes so far) ===" % (port, e, total),
                  flush=True)
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(1.0)


if __name__ == "__main__":
    sys.exit(main())
