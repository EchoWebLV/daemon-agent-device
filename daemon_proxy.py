#!/usr/bin/env python3
"""
Tiny HTTP proxy: localhost:8080 -> http://172.20.10.2 (the Daemon board).

Chrome / Safari refuse to grant microphone permission for SpeechRecognition
over plain HTTP on a LAN IP because those URLs aren't a "secure context".
Localhost IS a secure context, even over HTTP. Running the web UI through
this proxy side-steps the browser's security model entirely — no HTTPS,
no self-signed certs, no Chrome flags — and lets the TAP TO TALK button
behave normally.

Usage:
    python3 daemon_proxy.py
    # then open http://localhost:8080 in Chrome

Set DAEMON_HOST in the environment if your board is on a different IP.
"""
import http.server
import os
import socketserver
import sys
import urllib.request
import urllib.error

BOARD = os.environ.get("DAEMON_HOST", "http://172.20.10.2")
PORT  = int(os.environ.get("DAEMON_PROXY_PORT", "8080"))

HOP_BY_HOP = {
    "connection", "transfer-encoding", "content-encoding",
    "content-length", "keep-alive", "te", "trailers", "upgrade",
    "proxy-authenticate", "proxy-authorization",
}


class Proxy(http.server.BaseHTTPRequestHandler):
    def _forward(self, method: str) -> None:
        url = BOARD + self.path
        body = None
        if method in ("POST", "PUT", "PATCH"):
            length = int(self.headers.get("content-length", 0))
            body = self.rfile.read(length) if length else b""

        headers = {}
        for k, v in self.headers.items():
            if k.lower() in ("host", "content-length"):
                continue
            headers[k] = v

        req = urllib.request.Request(url, data=body, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=60) as upstream:
                payload = upstream.read()
                self.send_response(upstream.status)
                for k, v in upstream.getheaders():
                    if k.lower() in HOP_BY_HOP:
                        continue
                    self.send_header(k, v)
                self.send_header("content-length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
        except urllib.error.HTTPError as e:
            payload = e.read() if hasattr(e, "read") else b""
            self.send_response(e.code)
            for k, v in e.headers.items():
                if k.lower() in HOP_BY_HOP:
                    continue
                self.send_header(k, v)
            self.send_header("content-length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except Exception as e:
            msg = f"proxy error: {e}".encode()
            try:
                self.send_response(502)
                self.send_header("content-type", "text/plain")
                self.send_header("content-length", str(len(msg)))
                self.end_headers()
                self.wfile.write(msg)
            except Exception:
                pass

    def do_GET(self):    self._forward("GET")
    def do_POST(self):   self._forward("POST")
    def do_PUT(self):    self._forward("PUT")
    def do_PATCH(self):  self._forward("PATCH")
    def do_DELETE(self): self._forward("DELETE")

    def log_message(self, fmt, *args):
        sys.stdout.write("%s %s\n" % (self.command, self.path))
        sys.stdout.flush()


class ThreadingServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    print(f"daemon proxy: http://localhost:{PORT}  ->  {BOARD}")
    print("open http://localhost:%d in Chrome and click TAP TO TALK." % PORT)
    with ThreadingServer(("127.0.0.1", PORT), Proxy) as server:
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
