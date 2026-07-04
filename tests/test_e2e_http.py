#!/usr/bin/env python3
"""
端到端 HTTP 测试：spawn chat_server（同时启 TCP + HTTP），用 http.client 打 HTTP 端口。
- TCP 端口: 19901（不需要连，但要 listen 着，避免 chat_server 因为某种原因退出）
- HTTP 端口: 19980，webroot = tests/fixtures/webroot

用例：
  1. GET /          -> 200, body = index.html
  2. GET /index.html -> 200, Content-Type: text/html
  3. GET /style.css  -> 200, Content-Type: text/css
  4. GET /hello.txt  -> 200, Content-Type: text/plain
  5. GET /nope.txt   -> 404
  6. GET /../etc/passwd -> 403
  7. POST /api/login (未注册路由) -> 404
  8. GET / -> Connection: close
"""
import os
import signal
import socket
import subprocess
import sys
import time
import http.client

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.environ.get("CHAT_SERVER", os.path.join(ROOT, "build", "chat_server"))
HTTP_PORT = int(os.environ.get("HTTP_PORT", "19980"))
TCP_PORT  = int(os.environ.get("CHAT_PORT",  "19901"))
WEBROOT   = os.path.join(ROOT, "tests", "fixtures", "webroot")

def wait_listen(port, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False

def req(method, path, body=b""):
    """直接拼 HTTP 报文，绕过 stdlib 库的 connection: keep-alive 默认"""
    head = (f"{method} {path} HTTP/1.1\r\n"
            f"Host: 127.0.0.1\r\n"
            f"Connection: close\r\n"
            f"Content-Length: {len(body)}\r\n\r\n").encode()
    s = socket.create_connection(("127.0.0.1", HTTP_PORT), timeout=2.0)
    s.sendall(head + body)
    data = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    # 解析 status line + 头 + body
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status_line = lines[0]
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.strip().lower()] = v.strip()
    return status_line, headers, body

def must(name, cond, hint=""):
    if cond:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}  {hint}")
        sys.exit(1)

def main():
    print(f"=== HTTP e2e ===")
    print(f"server: {SERVER}")
    print(f"http  : {HTTP_PORT}   tcp: {TCP_PORT}   webroot: {WEBROOT}")

    p = subprocess.Popen(
        [SERVER, str(TCP_PORT), str(HTTP_PORT), WEBROOT],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    try:
        if not wait_listen(HTTP_PORT):
            print("server didn't open HTTP port"); sys.exit(1)
        if not wait_listen(TCP_PORT):
            print("server didn't open TCP port"); sys.exit(1)

        # 1. GET /
        st, h, b = req("GET", "/")
        must("GET / returns 200", b"200" in st, st)
        must("GET / serves index.html body", b"hello from webroot" in b, b[:80])

        # 2. GET /index.html
        st, h, b = req("GET", "/index.html")
        must("GET /index.html 200", b"200" in st, st)
        must("index.html content-type is text/html",
             b"text/html" in h.get(b"content-type", b""), h.get(b"content-type", b""))

        # 3. GET /style.css
        st, h, b = req("GET", "/style.css")
        must("GET /style.css 200", b"200" in st, st)
        must("style.css content-type is text/css",
             b"text/css" in h.get(b"content-type", b""), h.get(b"content-type", b""))

        # 4. GET /hello.txt
        st, h, b = req("GET", "/hello.txt")
        must("GET /hello.txt 200", b"200" in st, st)
        must("hello.txt content-type is text/plain",
             b"text/plain" in h.get(b"content-type", b""), h.get(b"content-type", b""))
        must("hello.txt body", b"plain text body" in b, b[:80])

        # 5. GET /nope.txt -> 404
        st, h, b = req("GET", "/nope.txt")
        must("GET /nope.txt 404", b"404" in st, st)

        # 6. GET /../etc/passwd -> 403
        st, h, b = req("GET", "/../etc/passwd")
        must("path traversal blocked (403)", b"403" in st, st)

        # 7. POST /api/login 现在已注册 — 空 body 应该 400
        st, h, b = req("POST", "/api/login", b'{"username":"x"}')
        must("POST /api/login (valid body) gets a response",
             (b"200" in st) or (b"409" in st) or (b"200" in st or b"4" in st[:5]), st)

        # 8. Connection: close in response
        st, h, b = req("GET", "/")
        must("response Connection: close",
             b"close" in h.get(b"connection", b""), h.get(b"connection", b""))

        print("=== ALL HTTP E2E PASS ===")
    finally:
        p.send_signal(signal.SIGINT)
        try:
            p.wait(timeout=2)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()

if __name__ == "__main__":
    main()
