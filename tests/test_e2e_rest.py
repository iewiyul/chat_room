#!/usr/bin/env python3
"""
REST API 端到端测试：login → users → messages → logout
"""
import os, signal, socket, subprocess, sys, time, json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.environ.get("CHAT_SERVER", os.path.join(ROOT, "build", "chat_server"))
HTTP_PORT = int(os.environ.get("HTTP_PORT", "19982"))
TCP_PORT  = int(os.environ.get("CHAT_PORT",  "19903"))
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

def req(method, path, body=b"", token_in_query=True):
    headers = [f"{method} {path} HTTP/1.1", "Host: 127.0.0.1",
               "Connection: close", f"Content-Length: {len(body)}"]
    if body and method in ("POST", "PUT"):
        headers.insert(3, "Content-Type: application/json")
    head = ("\r\n".join(headers) + "\r\n\r\n").encode()
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
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status_line = lines[0]
    headers_out = {}
    for line in lines[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers_out[k.strip().lower()] = v.strip()
    try:
        body_json = json.loads(body) if body else {}
    except Exception:
        body_json = {}
    return status_line, headers_out, body_json

def must(name, cond, hint=""):
    if cond:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}  {hint}")
        sys.exit(1)

def main():
    print("=== REST e2e ===")
    p = subprocess.Popen(
        [SERVER, str(TCP_PORT), str(HTTP_PORT), WEBROOT],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    try:
        if not wait_listen(HTTP_PORT):
            out = p.communicate(timeout=1)[1]
            print("server didn't open HTTP port:", out); sys.exit(1)

        # 1. login alice
        st, h, b = req("POST", "/api/login", b'{"username":"alice"}', token_in_query=False)
        must("alice login 200", b"200" in st, st)
        must("ok=true",        b.get("ok") is True, str(b))
        must("token issued (starts with tok_)", isinstance(b.get("token"), str)
             and b["token"].startswith("tok_"), str(b))
        alice_token = b["token"]

        # 2. login duplicate -> 409
        st, h, b = req("POST", "/api/login", b'{"username":"alice"}')
        must("duplicate user 409", b"409" in st, st)

        # 3. login bob
        st, h, b = req("POST", "/api/login", b'{"username":"bob"}')
        must("bob login 200", b"200" in st, st)
        bob_token = b["token"]

        # 4. users list (alice's token)
        st, h, b = req("GET", f"/api/users?token={alice_token}")
        must("GET /api/users 200", b"200" in st, st)
        users = b.get("users", [])
        must("users contains alice and bob",
             set(users) >= {"alice", "bob"}, str(users))

        # 5. users list no token -> 401
        st, h, b = req("GET", "/api/users")
        must("no token -> 401", b"401" in st, st)

        # 6. users list bad token -> 401
        st, h, b = req("GET", "/api/users?token=tok_deadbeefdeadbeef")
        must("bad token -> 401", b"401" in st, st)

        # 7. POST messages missing token -> 401
        st, h, b = req("POST", "/api/messages", b'{"content":"hi"}')
        must("POST /api/messages no token -> 401", b"401" in st, st)

        # 8. POST messages empty content -> 400
        st, h, b = req("POST", "/api/messages",
                       ('{"token":"'+alice_token+'","content":""}').encode())
        must("empty content -> 400", b"400" in st, st)

        # 9. POST messages bad json (token 通过 query 传，避免坏 body 拿不到) -> 400
        st, h, b = req("POST", f"/api/messages?token={alice_token}", b'{not json}')
        must("bad json (with token) -> 400", b"400" in st, st)

        # 10. POST a real message from alice + one from bob
        st, h, b = req("POST", "/api/messages",
                       ('{"token":"'+alice_token+'","content":"hello web"}').encode())
        must("alice POST message 200", b"200" in st, st)

        time.sleep(0.05)   # 让 timestamp 不同
        st, h, b = req("POST", "/api/messages",
                       ('{"token":"'+bob_token+'","content":"web hi"}').encode())
        must("bob POST message 200", b"200" in st, st)

        # 11. GET messages 拿两条
        st, h, b = req("GET", f"/api/messages?token={alice_token}")
        must("GET messages 200", b"200" in st, st)
        msgs = b.get("messages", [])
        must("got 2 messages", len(msgs) == 2, str(len(msgs)))
        contents = [m["content"] for m in msgs]
        must("contents match", "hello web" in contents and "web hi" in contents, str(contents))
        # 字段齐全
        must("message has from/room/content/time",
             all({"from","room","content","time"}.issubset(m.keys()) for m in msgs),
             str(msgs))

        # 12. GET messages with since=第二段时间戳，应只剩那条
        if len(msgs) >= 2:
            mid_ts = (msgs[0]["time"] + msgs[1]["time"]) // 2
            st, h, b = req("GET", f"/api/messages?token={alice_token}&since={mid_ts}")
            got = b.get("messages", [])
            must("since filters older", len(got) == 1, str(len(got)))

        # 13. login method not allowed
        st, h, b = req("GET", "/api/login")
        must("GET /api/login -> 405", b"405" in st, st)

        # 14. logout alice
        st, h, b = req("POST", "/api/logout",
                       ('{"token":"'+alice_token+'"}').encode())
        must("logout 200", b"200" in st, st)

        # 15. alice token 失效
        st, h, b = req("GET", f"/api/users?token={alice_token}")
        must("revoked token -> 401", b"401" in st, st)

        # 16. alice 不在用户列表了
        st, h, b = req("GET", f"/api/users?token={bob_token}")
        users = b.get("users", [])
        must("alice removed from users list",
             "alice" not in users, str(users))
        must("bob still in users list", "bob" in users, str(users))

        print("=== ALL REST E2E PASS ===")
    finally:
        p.send_signal(signal.SIGINT)
        try: p.wait(timeout=2)
        except subprocess.TimeoutExpired: p.kill(); p.wait()

if __name__ == "__main__":
    main()
