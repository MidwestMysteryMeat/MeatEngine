#!/usr/bin/env python3
"""MeatEngine master server — a public server list, stdlib only.

Anyone can run one anywhere; the engine takes --master host[:port] and no
default master is baked in. Protocol (plain HTTP by design; see HttpTiny.h):

  POST /announce  {"name","port","players","maxPlayers"}
                  -> records the entry under the announcer's SOURCE ip + port
  GET  /servers   -> JSON array of entries seen within the last 60 s
  GET  /          -> plaintext status

Usage: python master_server.py [port]   (default 27000)
"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TTL_SECONDS = 60.0
MAX_BODY_BYTES = 4096
MAX_NAME_CHARS = 64

_servers = {}  # (address, port) -> entry dict with "lastSeen"
_lock = threading.Lock()


def live_entries():
    """Drop expired entries and return the survivors (wire shape, no lastSeen)."""
    now = time.monotonic()
    with _lock:
        for key in [k for k, e in _servers.items() if now - e["lastSeen"] > TTL_SECONDS]:
            del _servers[key]
        return [{k: e[k] for k in ("name", "address", "port", "players", "maxPlayers")}
                for e in _servers.values()]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # matches the engine's HttpTiny client

    def log_message(self, fmt, *args):  # quiet: heartbeats every 30 s would spam
        pass

    def _reply(self, code, payload, content_type="application/json"):
        data = payload.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/servers":
            self._reply(200, json.dumps(live_entries()))
        elif self.path == "/":
            self._reply(200, f"meat master: {len(live_entries())} live servers\n", "text/plain")
        else:
            self._reply(404, '{"error":"not found"}')

    def do_POST(self):
        if self.path != "/announce":
            return self._reply(404, '{"error":"not found"}')
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length <= 0 or length > MAX_BODY_BYTES:
            return self._reply(400, '{"error":"bad content length"}')
        try:
            body = json.loads(self.rfile.read(length))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return self._reply(400, '{"error":"invalid json"}')
        if not isinstance(body, dict):
            return self._reply(400, '{"error":"expected object"}')

        name = body.get("name")
        port = body.get("port")
        players = body.get("players")
        max_players = body.get("maxPlayers")
        if (not isinstance(name, str) or not name
                or not isinstance(port, int) or isinstance(port, bool)
                or not 0 < port <= 0xFFFF
                or not isinstance(players, int) or isinstance(players, bool)
                or not isinstance(max_players, int) or isinstance(max_players, bool)):
            return self._reply(400, '{"error":"bad fields"}')

        address = self.client_address[0]  # source ip is authoritative, never the body
        entry = {
            "name": name[:MAX_NAME_CHARS],
            "address": address,
            "port": port,
            "players": max(0, players),
            "maxPlayers": max(0, max_players),
            "lastSeen": time.monotonic(),
        }
        with _lock:
            _servers[(address, port)] = entry
        self._reply(200, '{"ok":true}')


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 27000
    httpd = ThreadingHTTPServer(("", port), Handler)
    print(f"meat master server on port {port} (ctrl-c to stop)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down")
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
