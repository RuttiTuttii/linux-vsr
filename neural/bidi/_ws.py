# minimal websocket client for bidi without origin header quirks
# firefox remote agent rejects any origin value, raw socket omits it

import base64
import json
import os
import socket
import struct


# sync framing transport over raw tcp socket
class RawWs:
    def __init__(self, host, port, path, timeout=10.0):
        # open tcp connection to agent
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        # perform handshake without origin header
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        self.sock.sendall(req.encode())
        # read handshake response headers
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise ConnectionError("handshake closed early")
            head += chunk
        # validate switching protocols status
        status = head.split(b"\r\n", 1)[0]
        if b"101" not in status:
            raise ConnectionError(f"handshake refused: {status[:80]!r}")

    # read exact byte count from socket
    def _readn(self, n):
        # accumulate until full frame part present
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("socket closed mid-frame")
            buf += chunk
        return buf

    # send one masked text frame
    def send_text(self, text):
        # encode payload bytes
        data = text.encode() if isinstance(text, str) else text
        mask = os.urandom(4)
        # build header for payload length class
        if len(data) < 126:
            head = struct.pack("!BB", 0x81, 0x80 | len(data))
        elif len(data) < 65536:
            head = struct.pack("!BBH", 0x81, 0x80 | 126, len(data))
        else:
            head = struct.pack("!BBQ", 0x81, 0x80 | 127, len(data))
        # mask payload bytes with key
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(head + mask + masked)

    # receive one text frame, answer pings inline
    def recv_text(self):
        while True:
            # parse frame header bytes
            h = self._readn(2)
            op = h[0] & 0x0F
            ln = h[1] & 0x7F
            # resolve extended length forms
            if ln == 126:
                ln = struct.unpack("!H", self._readn(2))[0]
            elif ln == 127:
                ln = struct.unpack("!Q", self._readn(8))[0]
            # read optional mask and payload
            masked = bool(h[1] & 0x80)
            key = self._readn(4) if masked else b""
            data = self._readn(ln) if ln else b""
            if masked:
                data = bytes(b ^ key[i % 4] for i, b in enumerate(data))
            # answer ping with pong
            if op == 0x9:
                self.sock.sendall(b"\x8a\x00")
                continue
            # close frame ends transport
            if op == 0x8:
                raise ConnectionError("server closed session")
            # return text payloads
            if op == 0x1:
                return data.decode(errors="replace")
            # ignore continuation and binary frames

    # send bidi command dict, wait for matching id
    def cmd(self, cid, method, params=None):
        # serialize command frame
        self.send_text(json.dumps({"id": cid, "method": method, "params": params or {}}))
        # wait for response with same id
        while True:
            msg = json.loads(self.recv_text())
            if msg.get("id") == cid:
                if "error" in msg:
                    raise RuntimeError(f"bidi error: {msg['error']}")
                return msg.get("result", {})

    # close tcp connection
    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass
