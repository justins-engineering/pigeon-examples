#!/usr/bin/env python3
"""A stand-in for the PidgeIoT edge, just complete enough to run pigeonhole.

The MQTT broker is a thin bridge: it terminates the session and turns every
publish into an HTTP call against the platform's device routes, and it holds
one device WebSocket per session as that session's authentication, its shadow
feed and its QoS 0 telemetry path. This serves exactly those routes, in
process, with no Cloudflare account, no Postgres and no credentials -- so
`native-sim-e2e.sh` can run the whole device-to-platform path on one
workstation and assert on what actually arrived.

What it is NOT: an authorization model. Device tokens are recorded and
checked for presence, never verified -- the real platform verifies an Ed25519
signature per request, which is the whole reason the broker is not a trusted
proxy. Nothing here should ever be pointed at real device credentials.

Routes served (dovecote's `docs/api.md` is the contract):
  POST /device/pigeons/<id>/telemetry     -> 202
  POST /device/pigeons/<id>/shadow        -> 200
  POST /device/pigeons/<id>/logs          -> 200
  GET  /device/pigeons/<id>/ws            -> 101, then a shadow snapshot
  GET  /internal/device-psk/<identity>    -> the PSK and token for a pigeon

Plus a control surface the test driver uses, which the real platform has no
equivalent of:
  GET  /_control/state                    -> everything recorded so far
  POST /_control/shadow                   -> set a new target and push it
"""

import argparse
import base64
import hashlib
import json
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

STATE_LOCK = threading.Lock()
STATE = {
    "requests": [],   # every device-route call, in arrival order
    "frames": [],     # every text frame the device socket received
    "upgrades": 0,    # accepted WebSocket upgrades
    "refusals": 0,    # refused ones
    "shadow": {
        "target_version": 1,
        "current_version": 0,
        "target_config": '{"telemetry_interval":15}',
        "current_config": "{}",
        "updated_at": int(time.time()),
    },
}

SOCKETS_LOCK = threading.Lock()
SOCKETS = []          # open device sockets, as WsPeer


def record(entry):
    with STATE_LOCK:
        STATE["requests"].append(entry)


def snapshot():
    with STATE_LOCK:
        return json.loads(json.dumps(STATE))


class WsPeer:
    """One accepted device socket, writable from any thread."""

    def __init__(self, sock):
        self.sock = sock
        self.lock = threading.Lock()
        self.closed = False

    def send(self, opcode, payload=b""):
        # Server-to-client frames are never masked (RFC 6455 sec 5.1).
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(length)
        elif length < (1 << 16):
            header.append(126)
            header += struct.pack("!H", length)
        else:
            header.append(127)
            header += struct.pack("!Q", length)
        with self.lock:
            if self.closed:
                return
            try:
                self.sock.sendall(bytes(header) + payload)
            except OSError:
                self.closed = True

    def send_text(self, text):
        self.send(0x1, text.encode())

    def close(self, code=1000):
        self.send(0x8, struct.pack("!H", code))
        with self.lock:
            self.closed = True


def push_shadow_update(shadow):
    frame = json.dumps({"type": "shadow_update", "shadow": shadow})
    with SOCKETS_LOCK:
        peers = list(SOCKETS)
    for peer in peers:
        peer.send_text(frame)
    return len(peers)


def read_exact(sock, count):
    buf = b""
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_frame(sock):
    """One client frame, unmasked. Returns (opcode, payload) or None."""
    head = read_exact(sock, 2)
    if not head:
        return None
    opcode = head[0] & 0x0F
    masked = head[1] & 0x80
    length = head[1] & 0x7F
    if length == 126:
        extended = read_exact(sock, 2)
        if not extended:
            return None
        length = struct.unpack("!H", extended)[0]
    elif length == 127:
        extended = read_exact(sock, 8)
        if not extended:
            return None
        length = struct.unpack("!Q", extended)[0]
    mask = read_exact(sock, 4) if masked else None
    payload = read_exact(sock, length) if length else b""
    if payload is None:
        return None
    if mask:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    # Quiet by default: the driver reads /_control/state, not this log.
    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write("mock-dovecote: " + (fmt % args) + "\n")

    def _json(self, status, body):
        payload = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _empty(self, status, text=""):
        payload = text.encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if payload:
            self.wfile.write(payload)

    def _bearer(self):
        value = self.headers.get("Authorization", "")
        return value[len("Bearer "):] if value.startswith("Bearer ") else None

    def do_GET(self):
        parts = [p for p in self.path.split("/") if p]

        if self.path == "/_control/state":
            self._json(200, snapshot())
            return

        if len(parts) == 2 and parts[0] == "internal" and parts[1].startswith("device-psk"):
            self._empty(400, "identity missing")
            return

        if len(parts) == 3 and parts[0] == "internal" and parts[1] == "device-psk":
            if self._bearer() != self.server.service_secret:
                self._empty(403, "service secret")
                return
            if parts[2] != self.server.pigeon_id:
                self._empty(404, "unknown identity")
                return
            self._json(200, {
                "identity": parts[2],
                "secret": self.server.psk_secret,
                "token": self.server.token,
            })
            return

        if len(parts) == 4 and parts[0] == "device" and parts[1] == "pigeons" and parts[3] == "ws":
            self._device_socket(parts[2])
            return

        self._empty(404, "no such route")

    def do_POST(self):
        parts = [p for p in self.path.split("/") if p]
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b""

        if self.path == "/_control/shadow":
            self._set_shadow(body)
            return

        if len(parts) == 4 and parts[0] == "device" and parts[1] == "pigeons":
            leaf = parts[3]
            if leaf not in ("telemetry", "shadow", "logs"):
                self._empty(404, "no such route")
                return
            record({
                "leaf": leaf,
                "pigeon": parts[2],
                "content_type": self.headers.get("Content-Type", ""),
                "bearer_present": self._bearer() is not None,
                "len": len(body),
                "body": describe(body),
                "at": time.time(),
            })
            if leaf == "shadow":
                self._converge(body)

            # Telemetry is queued for a durable write, which is why the real
            # platform answers 202 rather than 200; the other two complete
            # their write before answering.
            self._empty(202 if leaf == "telemetry" else 200)
            return

        self._empty(404, "no such route")

    def _converge(self, body):
        """A device's shadow report moves current_version/current_config, and
        the platform then pushes the whole shadow back out -- the Durable
        Object pushes on ANY shadow write, including this one. That is worth
        reproducing rather than skipping: the broker is supposed to notice
        that target_version did not change and NOT re-publish to the device,
        which is the rule that keeps a device's own report-back from looking
        like a fresh config to it."""
        try:
            report = json.loads(body or b"{}")
        except ValueError:
            return
        with STATE_LOCK:
            shadow = STATE["shadow"]
            if "current_version" in report:
                shadow["current_version"] = int(report["current_version"])
            if "current_config" in report:
                shadow["current_config"] = json.dumps(report["current_config"]) \
                    if not isinstance(report["current_config"], str) \
                    else report["current_config"]
            shadow["updated_at"] = int(time.time())
            pushed = json.loads(json.dumps(shadow))
        push_shadow_update(pushed)

    def _set_shadow(self, body):
        request = json.loads(body or b"{}")
        with STATE_LOCK:
            shadow = STATE["shadow"]
            shadow["target_version"] = int(
                request.get("target_version", shadow["target_version"] + 1)
            )
            if "target_config" in request:
                shadow["target_config"] = request["target_config"]
            shadow["updated_at"] = int(time.time())
            pushed = json.loads(json.dumps(shadow))
        delivered = push_shadow_update(pushed)
        self._json(200, {"shadow": pushed, "pushed_to": delivered})

    def _device_socket(self, pigeon):
        key = self.headers.get("Sec-WebSocket-Key")
        token = self._bearer()

        # The upgrade IS the session's authentication on the real platform,
        # so a refusal here is what a bad token looks like to the broker.
        if not key or not token or (self.server.token and token != self.server.token):
            with STATE_LOCK:
                STATE["refusals"] += 1
            self._empty(401, "unauthorized")
            return

        accept = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode()).digest()
        ).decode()

        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.wfile.flush()

        sock = self.connection
        peer = WsPeer(sock)
        with SOCKETS_LOCK:
            SOCKETS.append(peer)
        with STATE_LOCK:
            STATE["upgrades"] += 1
            snapshot_shadow = json.loads(json.dumps(STATE["shadow"]))

        # Snapshot on accept: this is what seeds the broker's retained
        # pigeon/shadow/target, so a device sees its config without asking.
        peer.send_text(json.dumps({"type": "shadow_update", "shadow": snapshot_shadow}))

        try:
            while True:
                frame = read_frame(sock)
                if frame is None:
                    break
                opcode, payload = frame
                if opcode == 0x8:      # close
                    break
                if opcode == 0x9:      # ping -> pong, the liveness the bridge owns
                    peer.send(0xA, payload)
                    continue
                if opcode == 0xA:      # pong
                    continue
                if opcode == 0x1:      # text
                    text = payload.decode("utf-8", "replace")
                    with STATE_LOCK:
                        STATE["frames"].append({"text": text, "at": time.time()})
                    try:
                        parsed = json.loads(text)
                    except ValueError:
                        continue
                    if parsed.get("type") == "ping":
                        peer.send_text(json.dumps({"type": "pong"}))
        except OSError:
            pass
        finally:
            with SOCKETS_LOCK:
                if peer in SOCKETS:
                    SOCKETS.remove(peer)
            peer.closed = True
            self.close_connection = True


def describe(body):
    """Bodies are JSON for two leaves and opaque dictionary-log bytes for the
    third, so keep text readable and everything else base64."""
    try:
        return {"text": body.decode("utf-8")}
    except UnicodeDecodeError:
        return {"base64": base64.b64encode(body).decode()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8788)
    parser.add_argument("--pigeon-id", required=True,
                        help="the one pigeon this mock knows about")
    parser.add_argument("--token", default="",
                        help="device bearer token to require on the upgrade; "
                             "empty accepts any non-empty token")
    parser.add_argument("--psk-secret", default="",
                        help="PSK handed to the broker for --pigeon-id")
    parser.add_argument("--service-secret", default="",
                        help="what the broker must present on the internal route")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    server.pigeon_id = args.pigeon_id
    server.token = args.token
    server.psk_secret = args.psk_secret
    server.service_secret = args.service_secret
    server.verbose = args.verbose

    print(f"mock dovecote listening on http://127.0.0.1:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
