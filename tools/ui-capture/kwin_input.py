#!/usr/bin/env python3
"""Real input through the session's KWin: a small Wayland client of KWin's fake-input protocol
(org_kde_kwin_fake_input), written against the wire format so that it needs no library.

The probe's events go to the app's windows directly, past the compositor. KWin opens a window popup (a menu,
since #642) only for a client that has had input from the compositor, so a session gives the app one real
key press through this client first (Session.prime_input), and one pointer enter: Qt on Wayland knows the
pointer only from where it last entered a window, and takes (0, 0), the menu bar's first item, until it has.

KWin offers the protocol only to a client whose executable a .desktop file names with
X-KDE-Wayland-Interfaces=org_kde_kwin_fake_input; driver.py writes one for this Python in the session's
own data folder, as for KWin's screenshot interface.
"""
import os
import socket
import struct

# org_kde_kwin_fake_input requests (fake-input.xml in plasma-wayland-protocols)
_AUTHENTICATE, _POINTER_MOTION_ABSOLUTE, _KEYBOARD_KEY = 0, 9, 10
KEY_LEFTSHIFT = 42
KEYS = {"Menu": 127}  # (evdev's KEY_COMPOSE, the Menu key)


def _string(s):
    b = s.encode() + b"\0"
    return struct.pack("<I", len(b)) + b + b"\0" * (-len(b) % 4)


class KWinInput:
    def __init__(self, socket_path, timeout=5.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(socket_path)
        self.buf = b""
        self.next_id = 2
        registry = self._new_id()
        self._send(1, 1, struct.pack("<I", registry))  # wl_display.get_registry
        globals_ = {}
        self._roundtrip(lambda obj, op, body: obj == registry and op == 0 and self._global(body, globals_))
        if "org_kde_kwin_fake_input" not in globals_:
            raise RuntimeError("KWin offers no org_kde_kwin_fake_input to this client (is its .desktop file "
                               "with X-KDE-Wayland-Interfaces in the session's data folder?)")
        name, version = globals_["org_kde_kwin_fake_input"]
        if version < 4:
            raise RuntimeError(f"org_kde_kwin_fake_input version {version}: keys need 4")
        self.fake = self._new_id()
        # wl_registry.bind with an untyped new_id: name, interface, version, id
        self._send(registry, 0, struct.pack("<I", name) + _string("org_kde_kwin_fake_input")
                   + struct.pack("<II", 4, self.fake))
        self._send(self.fake, _AUTHENTICATE, _string("Ferrous UI capture") + _string("drives a test session"))
        self._roundtrip()

    def _new_id(self):
        self.next_id += 1
        return self.next_id - 1

    def _send(self, obj, opcode, body=b""):
        self.sock.sendall(struct.pack("<II", obj, ((8 + len(body)) << 16) | opcode) + body)

    @staticmethod
    def _global(body, out):
        name, n = struct.unpack_from("<II", body)
        iface = body[8:8 + n - 1].decode()
        (version,) = struct.unpack_from("<I", body, 8 + n + (-n % 4))
        out[iface] = (name, version)

    def _roundtrip(self, on_event=None):
        """Until KWin has handled every request sent so far (wl_display.sync's done), passing each event on."""
        callback = self._new_id()
        self._send(1, 0, struct.pack("<I", callback))  # wl_display.sync
        while True:
            while len(self.buf) < 8 or len(self.buf) < (struct.unpack_from("<I", self.buf, 4)[0] >> 16):
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("KWin closed the connection")
                self.buf += chunk
            obj, word = struct.unpack_from("<II", self.buf)
            size, op = word >> 16, word & 0xFFFF
            body, self.buf = self.buf[8:size], self.buf[size:]
            if obj == 1 and op == 0:  # wl_display.error
                (n,) = struct.unpack_from("<I", body, 8)
                raise RuntimeError("KWin: " + body[12:12 + n - 1].decode())
            if obj == callback and op == 0:  # wl_callback.done
                return
            if on_event:
                on_event(obj, op, body)

    def move(self, x, y):
        """The pointer to (x, y) of the desktop, in logical px."""
        self._send(self.fake, _POINTER_MOTION_ABSOLUTE, struct.pack("<ii", round(x * 256), round(y * 256)))
        self._roundtrip()

    def key(self, code):
        """A key (evdev code) pressed and released."""
        for state in (1, 0):
            self._send(self.fake, _KEYBOARD_KEY, struct.pack("<II", code, state))
        self._roundtrip()

    def close(self):
        self.sock.close()


def socket_path(runtime_dir, name="shotwl"):
    return os.path.join(runtime_dir, name)
