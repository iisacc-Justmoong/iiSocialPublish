"""Exercise the real TLS transport without contacting a social account."""
import http.server
import json
from pathlib import Path
import ssl
import subprocess
import sys
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, value):
        data = json.dumps(value).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(data)

    def do_PUT(self):
        data = self.rfile.read(int(self.headers["Content-Length"]))
        self.reply({"bytes": data.decode()})

    def do_POST(self):
        data = self.rfile.read(int(self.headers["Content-Length"]))
        self.reply({"matched": b'name="json"' in data and b'application/json' in data
                    and b'name="file"; filename="media"' in data and b'\r\n\r\nabcd\r\n' in data
                    and b'EFGH' not in data})

    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "https://127.0.0.1:1/must-not-follow")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/large":
            self.send_response(200)
            self.end_headers()
            try:
                self.wfile.write(b"x" * (2 * 1024 * 1024 + 1))
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass
        else:
            self.reply({"ok": True})


def main():
    executable, folder, openssl = sys.argv[1:]
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    certificate, key = folder / "certificate.pem", folder / "key.pem"
    subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
                    "-keyout", str(key), "-out", str(certificate)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run([executable, "transport", f"https://127.0.0.1:{server.server_port}", str(certificate)],
                       check=True, timeout=45, cwd=folder.parent)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
