"""Exercise the real metadata HTTP transport on private loopback listeners only."""

import gzip
import http.server
import os
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


PAYLOAD = (
    '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
    'xmlns:p="http://schemas.microsoft.com/windows/pub/2005/07">'
    '<s:Body><p:Computer>café &amp; 家/Workgroup:TEST</p:Computer></s:Body></s:Envelope>'
).encode()
LIMIT = 262144
STOP = threading.Event()
REQUESTS = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        REQUESTS.append((self.server, self.path, self.headers, body))
        try:
            payload = gzip.compress(PAYLOAD) if self.path == "/gzip" else PAYLOAD
            if self.path == "/stall":
                STOP.wait(15)
                return
            self.send_response(302 if self.path == "/redirect" else 503 if self.path == "/status" else 200)
            self.send_header("Connection", "close")
            if self.path == "/gzip":
                self.send_header("Content-Encoding", "gzip")
            if self.path == "/redirect":
                self.send_header("Location", self.server.trap)
            if self.path == "/headers":
                for _ in range(40):
                    self.send_header("X-Padding", "x" * 2048)
            if self.path == "/oversize-length":
                self.send_header("Content-Length", str(LIMIT + 1))
            elif self.path == "/truncated":
                self.send_header("Content-Length", str(len(PAYLOAD) + 100))
            elif self.path == "/chunked":
                self.send_header("Transfer-Encoding", "chunked")
            elif self.path != "/oversize-stream":
                self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            if self.path == "/oversize-length":
                return
            if self.path == "/oversize-stream":
                self.wfile.write(b"x" * (LIMIT + 1))
            elif self.path == "/drip":
                for byte in PAYLOAD:
                    self.wfile.write(bytes([byte]))
                    self.wfile.flush()
                    if STOP.wait(0.1):
                        break
            elif self.path == "/chunked":
                for piece in (PAYLOAD[:53], PAYLOAD[53:]):
                    self.wfile.write(f"{len(piece):x}\r\n".encode() + piece + b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
            else:
                self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass


class IPv6Server(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6


def main():
    binary = sys.argv[1]
    subprocess.run([binary], check=True, timeout=10)
    servers = []
    try:
        trap = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        trap.trap = ""
        servers.append(trap)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.trap = f"http://127.0.0.1:{trap.server_port}/trap"
        servers.append(server)
        for listener in servers:
            threading.Thread(target=listener.serve_forever, daemon=True).start()
        env = dict(os.environ)
        for name in ("http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"):
            env[name] = server.trap
        env["no_proxy"] = env["NO_PROXY"] = ""

        def request(path, expected="", cancel=0, modern="legacy", listener=server, address="127.0.0.1"):
            subprocess.run(
                [binary, "--metadata", f"http://{address}:{listener.server_port}{path}",
                 "unresolvable.invalid:5357", str(cancel), expected, modern],
                env=env, check=True, timeout=12,
            )

        request("/ok", "café & 家")
        request("/ok-modern", "café & 家", modern="modern")
        request("/chunked", "café & 家")
        request("/gzip", "café & 家")
        for path in ("/redirect", "/status", "/headers", "/oversize-length", "/oversize-stream", "/truncated"):
            request(path)
        request("/stall", cancel=100)
        request("/drip")
        count = len(REQUESTS)
        request("/precancelled", cancel=-1)
        assert len(REQUESTS) == count
        with tempfile.TemporaryDirectory(prefix="luciole-metadata-") as directory:
            payload = Path(directory) / "metadata.xml"
            payload.write_bytes(PAYLOAD)
            subprocess.run(
                [binary, "--metadata", payload.as_uri(), "unused.invalid", "0", "", "legacy"],
                env=env, check=True, timeout=5,
            )
        try:
            ipv6 = IPv6Server(("::1", 0), Handler)
        except OSError:
            print("IPv6 loopback unavailable; IPv6 HTTP check skipped", flush=True)
        else:
            ipv6.trap = server.trap
            servers.append(ipv6)
            threading.Thread(target=ipv6.serve_forever, daemon=True).start()
            request("/ipv6", "café & 家", listener=ipv6, address="[::1]")
            request("/ipv6-scoped", "café & 家", listener=ipv6, address=f"[::1%25{socket.if_nametoindex('lo')}]")
        assert REQUESTS
        for listener, path, headers, body in REQUESTS:
            assert listener is not trap, "Proxy or redirect destination was contacted"
            assert headers["Host"] == "unresolvable.invalid:5357"
            assert headers["Content-Type"] == "application/soap+xml; charset=utf-8"
            assert b"urn:uuid:fixture" in body and b"Get" in body
            namespace = (
                b"http://www.w3.org/2005/08/addressing" if path == "/ok-modern"
                else b"http://schemas.xmlsoap.org/ws/2004/08/addressing"
            )
            assert namespace in body
        print("Discovery metadata HTTP fixtures passed", flush=True)
    finally:
        STOP.set()
        for listener in servers:
            listener.shutdown()
            listener.server_close()


if __name__ == "__main__":
    main()
