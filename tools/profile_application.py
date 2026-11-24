#!/usr/bin/env python3
"""Measure Linux process CPU after startup; optionally focus its X11 window and send one key.

Example: profile_application.py --cwd /path/to/tool --key space -- ./build/tool model.fbx
CPU is a percentage of one core, summed across the application's threads. Child processes and GPU
load are excluded. The application is terminated after sampling; use an unsaved test instance.
"""
import argparse
import ctypes
import os
import re
import signal
import subprocess
import time


def window_for(pid):
    clients = subprocess.run(['xprop', '-root', '_NET_CLIENT_LIST'], capture_output=True, text=True)
    for window in re.findall(r'0x[0-9a-fA-F]+', clients.stdout):
        prop = subprocess.run(['xprop', '-id', window, '_NET_WM_PID'], capture_output=True, text=True)
        if re.search(r'=\s*' + str(pid) + r'\s*$', prop.stdout):
            return int(window, 16)
    # Some window managers omit EWMH's client list. Walk the X tree in that case.
    x = ctypes.CDLL('libX11.so.6')
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x.XDefaultRootWindow.restype = ctypes.c_ulong
    x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                            ctypes.POINTER(ctypes.c_uint)]
    x.XFree.argtypes = [ctypes.c_void_p]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x.XOpenDisplay(None)
    if not display:
        raise RuntimeError('Cannot open X display')
    try:
        pending = [(x.XDefaultRootWindow(display), 0)]
        while pending:
            window, depth = pending.pop()
            prop = subprocess.run(['xprop', '-id', hex(window), '_NET_WM_PID'], capture_output=True, text=True)
            if re.search(r'=\s*' + str(pid) + r'\s*$', prop.stdout):
                return window
            if depth >= 4:
                continue
            root, parent, count = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_uint()
            children = ctypes.POINTER(ctypes.c_ulong)()
            if x.XQueryTree(display, window, ctypes.byref(root), ctypes.byref(parent),
                            ctypes.byref(children), ctypes.byref(count)):
                pending.extend((children[i], depth + 1) for i in range(count.value))
                if children:
                    x.XFree(children)
    finally:
        x.XCloseDisplay(display)
    raise RuntimeError('Cannot find the test application window; no input was sent')


def focus_and_key(pid, key, hold=False):
    x = ctypes.CDLL('libX11.so.6')
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    x.XFlush.argtypes = [ctypes.c_void_p]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x.XOpenDisplay(None)
    if not display:
        raise RuntimeError('Cannot open X display')
    try:
        x.XSetInputFocus(display, window_for(pid), 2, 0)
        x.XFlush(display)
        time.sleep(.2)
        if key:
            x.XStringToKeysym.argtypes = [ctypes.c_char_p]
            x.XStringToKeysym.restype = ctypes.c_ulong
            x.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            code = x.XKeysymToKeycode(display, x.XStringToKeysym(key.encode()))
            if not code:
                raise RuntimeError('Unknown key: ' + key)
            xt = ctypes.CDLL('libXtst.so.6')
            xt.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
            xt.XTestFakeKeyEvent(display, code, 1, 0)
            x.XFlush(display)
            if not hold:
                time.sleep(.15)
                xt.XTestFakeKeyEvent(display, code, 0, 0)
                x.XFlush(display)
    finally:
        x.XCloseDisplay(display)


def release_key(key):
    x = ctypes.CDLL('libX11.so.6')
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XStringToKeysym.argtypes = [ctypes.c_char_p]
    x.XStringToKeysym.restype = ctypes.c_ulong
    x.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x.XFlush.argtypes = [ctypes.c_void_p]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x.XOpenDisplay(None)
    if display:
        xt = ctypes.CDLL('libXtst.so.6')
        xt.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        xt.XTestFakeKeyEvent(display, x.XKeysymToKeycode(display, x.XStringToKeysym(key.encode())), 0, 0)
        x.XFlush(display)
        x.XCloseDisplay(display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cwd', default='.')
    parser.add_argument('--warmup', type=float, default=5)
    parser.add_argument('--duration', type=float, default=5)
    parser.add_argument('--focus', action='store_true')
    parser.add_argument('--key')
    parser.add_argument('--hold-key', help='Hold this key during sampling, then release it')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or args.duration <= 0 or args.warmup < 0:
        parser.error('Supply a command, positive duration and nonnegative warmup')
    process = subprocess.Popen(command, cwd=args.cwd, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, start_new_session=True)
    held = False
    def ticks():
        with open('/proc/%d/stat' % process.pid) as source:
            fields = source.read().rsplit(')', 1)[1].split()
        return int(fields[11]) + int(fields[12])
    try:
        time.sleep(args.warmup)
        if args.focus or args.key or args.hold_key:
            focus_and_key(process.pid, args.hold_key or args.key, bool(args.hold_key))
            held = bool(args.hold_key)
            time.sleep(1)
        previous = ticks()
        started = time.monotonic()
        samples = []
        while time.monotonic() - started < args.duration:
            before = time.monotonic()
            time.sleep(min(.5, args.duration - (before - started)))
            current = ticks()
            samples.append((current - previous) / os.sysconf('SC_CLK_TCK') /
                           (time.monotonic() - before) * 100)
            previous = current
        print('CPU mean=%.1f%% peak_half_second=%.1f%% samples=%d' %
              (sum(samples) / len(samples), max(samples), len(samples)))
    finally:
        if held:
            release_key(args.hold_key)
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
        process.wait()


if __name__ == '__main__':
    main()
