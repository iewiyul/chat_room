#!/usr/bin/env python3
"""
WebSocket 端到端测试：spawn chat_server，raw socket 模拟 WS 客户端。
main.cpp: chat_server <http_port> <webroot>

用例：
  1. POST /api/login alice/bob → 拿 token
  2. raw socket 发 WS 握手 → 验证 101 + Sec-WebSocket-Accept 正确
  3. alice 先 POST 一条消息 → bob 连上 → bob 收到这条历史
  4. bob 通过 ws text 发 chat 帧 → alice 实时收到（广播）
  5. server 关闭一条连接后,另一条仍能正常收发（不互相影响）
  6. 无效 token 拒绝
  7. 错误 Sec-WebSocket-Version 拒绝
"""
import os, signal, socket, subprocess, sys, time, json, base64, struct, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.environ.get("CHAT_SERVER", os.path.join(ROOT, "build", "chat_server"))
HTTP_PORT = int(os.environ.get("HTTP_PORT", "19983"))
WEBROOT   = os.path.join(ROOT, "tests", "fixtures", "webroot")

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# ---------- HTTP（拿 token / 发消息） ----------

def http_req(method, path, body=b""):
    s = socket.create_connection(("127.0.0.1", HTTP_PORT), timeout=2.0)
    req = (f"{method} {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n"
           f"Connection: close\r\nContent-Length: {len(body)}\r\n\r\n").encode() + body
    s.sendall(req)
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
    return data

def login(name):
    data = http_req("POST", "/api/login", json.dumps({"username":name}).encode())
    head, _, body = data.partition(b"\r\n\r\n")
    assert b"200" in head.split(b"\r\n")[0], head
    return json.loads(body)["token"]

def post_message(token, content):
    body = json.dumps({"token":token,"content":content}).encode()
    data = http_req("POST", "/api/messages", body)
    head = data.partition(b"\r\n\r\n")[0]
    assert b"200" in head.split(b"\r\n")[0], head

# ---------- WS 客户端（手写） ----------

class WsClient:
    def __init__(self, host, port, token):
        self.sock = socket.create_connection((host, port), timeout=2.0)
        self.sock.settimeout(2.0)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET /ws?token={token} HTTP/1.1\r\n"
               f"Host: {host}:{port}\r\n"
               f"Upgrade: websocket\r\n"
               f"Connection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode()
        self.sock.sendall(req)
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk: raise RuntimeError("server closed during handshake")
            resp += chunk
        head, _, _ = resp.partition(b"\r\n\r\n")
        lines = head.split(b"\r\n")
        self.status_line = lines[0].decode(errors="replace")
        self.headers = {}
        for line in lines[1:]:
            if b":" in line:
                k, v = line.split(b":", 1)
                self.headers[k.strip().lower()] = v.strip()
        expected = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        if self.headers.get(b"sec-websocket-accept", b"").decode() != expected:
            raise RuntimeError("Sec-WebSocket-Accept mismatch")

    def send_text(self, s):
        data = s.encode()
        header = bytes([0x81])
        if len(data) < 126:
            header += bytes([0x80 | len(data)])
        elif len(data) < 65536:
            header += bytes([0x80 | 126]) + struct.pack("!H", len(data))
        else:
            header += bytes([0x80 | 127]) + struct.pack("!Q", len(data))
        mkey = os.urandom(4)
        masked = bytes(b ^ mkey[i % 4] for i, b in enumerate(data))
        self.sock.sendall(header + mkey + masked)

    def _recv_exact(self, n, timeout=2.0):
        out = b""
        end = time.time() + timeout
        while len(out) < n and time.time() < end:
            try:
                chunk = self.sock.recv(n - len(out))
            except socket.timeout:
                break
            if not chunk: break
            out += chunk
        return out

    def recv_frame(self, timeout=2.0):
        head = self._recv_exact(2, timeout)
        if len(head) < 2: return None
        b0, b1 = head[0], head[1]
        opcode = b0 & 0x0F
        masked = (b1 & 0x80) != 0
        length = b1 & 0x7F
        if length == 126:
            ext = self._recv_exact(2, timeout)
            length = struct.unpack("!H", ext)[0]
        elif length == 127:
            ext = self._recv_exact(8, timeout)
            length = struct.unpack("!Q", ext)[0]
        mkey = self._recv_exact(4, timeout) if masked else b""
        payload = self._recv_exact(length, timeout)
        if masked:
            payload = bytes(b ^ mkey[i % 4] for i, b in enumerate(payload))
        return opcode, payload

    def recv_text(self, timeout=2.0):
        """只返回 text 帧的 payload,跳过 ping/pong/close"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            f = self.recv_frame(timeout=max(0.05, deadline - time.time()))
            if f is None: return None
            op, payload = f
            if op == 0x1: return payload
            if op == 0x8: return None  # close
            if op == 0x9: continue  # ping:服务端会自己回 pong,这里不处理
            if op == 0xA: continue  # pong
        return None

    def close(self):
        try: self.sock.close()
        except Exception: pass

# ---------- 工具 ----------

def wait_listen(port, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False

def must(name, cond, hint=""):
    if cond:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}  {hint}")
        sys.exit(1)

# ---------- 主流程 ----------

def main():
    print("=== WS e2e ===")
    p = subprocess.Popen(
        [SERVER, str(HTTP_PORT), WEBROOT],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    try:
        if not wait_listen(HTTP_PORT):
            out = p.communicate(timeout=1)[1]
            print("server didn't open HTTP port:", out); sys.exit(1)

        # 1. login
        alice_tok = login("alice")
        bob_tok   = login("bob")
        must("login alice + bob", bool(alice_tok) and bool(bob_tok))

        # 2. WS 握手基本测试（先在空 room 上,验证 101 + accept 正确）
        a = WsClient("127.0.0.1", HTTP_PORT, alice_tok)
        must("alice handshake 101", "101" in a.status_line, a.status_line)
        must("Upgrade: websocket",
             a.headers.get(b"upgrade", b"").lower() == b"websocket", str(a.headers))
        must("Connection: Upgrade",
             a.headers.get(b"connection", b"").lower() == b"upgrade", str(a.headers))
        must("Sec-WebSocket-Accept present",
             b"sec-websocket-accept" in a.headers, str(a.headers))

        # 3. alice POST 一条消息
        post_message(alice_tok, "from alice")
        time.sleep(0.05)

        # 4. alice 应该在自己的 WS 收不到回环(她不订阅自己发的?取决于实现)
        #    实际 WsHub::broadcast 是发给所有 conn 包括自己 -- 我们只检查历史
        #    alice 重连拿历史
        a.close()
        a = WsClient("127.0.0.1", HTTP_PORT, alice_tok)

        # 5. alice 收到 1 条历史
        first = a.recv_text(timeout=1.0)
        must("alice receives 1 history frame", first is not None)
        if first is not None:
            j = json.loads(first)
            must("history type=chat", j.get("type") == "chat", str(j))
            must("history content matches",
                 j.get("content") == "from alice", str(j))
            must("history from=alice", j.get("from") == "alice", str(j))

        # 6. bob 连上,收历史
        b = WsClient("127.0.0.1", HTTP_PORT, bob_tok)
        first_b = b.recv_text(timeout=1.0)
        if first_b is not None:
            j = json.loads(first_b)
            must("bob history content", j.get("content") == "from alice", str(j))

        # 7. bob 通过 ws 发 chat 帧 → alice 实时收到
        b.send_text(json.dumps({"type":"chat","room":"lobby","content":"from bob"}))
        got = a.recv_text(timeout=1.0)
        must("alice receives bob's broadcast", got is not None)
        if got is not None:
            j = json.loads(got)
            must("broadcast content matches",
                 j.get("content") == "from bob", str(j))
            must("broadcast from=bob", j.get("from") == "bob", str(j))

        # 7b. 排空 bob 自己的回环(broadcast 会发回 sender 自己也发一份)
        time.sleep(0.1)
        b.recv_text(timeout=0.3)  # 丢掉"from bob"的回环
        b.recv_text(timeout=0.3)  # 再丢一条保险

        # 8. alice 主动关连接后,bob 仍能收到 alice 后续发的消息
        a.close()
        time.sleep(0.1)
        post_message(alice_tok, "after alice close")
        time.sleep(0.1)
        got = b.recv_text(timeout=1.0)
        must("bob still receives after alice closes", got is not None)
        if got is not None:
            j = json.loads(got)
            must("late content matches",
                 j.get("content") == "after alice close", str(j))

        # 9. bob 发第二条 → 应该没人收(bob 也关掉) — 这里只验证不崩
        b.send_text(json.dumps({"type":"chat","content":"nobody home"}))
        time.sleep(0.2)

        b.close()

        # 10. 无效 token
        try:
            s = socket.create_connection(("127.0.0.1", HTTP_PORT), timeout=2.0)
            key = base64.b64encode(os.urandom(16)).decode()
            req = (f"GET /ws?token=tok_deadbeefdeadbeef HTTP/1.1\r\n"
                   f"Host: 127.0.0.1\r\nUpgrade: websocket\r\n"
                   f"Connection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\n"
                   f"Sec-WebSocket-Version: 13\r\n\r\n").encode()
            s.sendall(req)
            resp = b""
            while b"\r\n\r\n" not in resp:
                chunk = s.recv(4096)
                if not chunk: break
                resp += chunk
            s.close()
            status = resp.split(b"\r\n", 1)[0].decode(errors="replace")
            must("bad token -> 401", "401" in status, status)
        except Exception as e:
            must("bad token -> 401", False, str(e))

        # 11. 错误 version
        try:
            s = socket.create_connection(("127.0.0.1", HTTP_PORT), timeout=2.0)
            key = base64.b64encode(os.urandom(16)).decode()
            req = (f"GET /ws?token={alice_tok} HTTP/1.1\r\n"
                   f"Host: 127.0.0.1\r\nUpgrade: websocket\r\n"
                   f"Connection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\n"
                   f"Sec-WebSocket-Version: 8\r\n\r\n").encode()
            s.sendall(req)
            resp = b""
            while b"\r\n\r\n" not in resp:
                chunk = s.recv(4096)
                if not chunk: break
                resp += chunk
            s.close()
            status = resp.split(b"\r\n", 1)[0].decode(errors="replace")
            must("bad version -> 400", "400" in status, status)
        except Exception as e:
            must("bad version -> 400", False, str(e))

        print("=== ALL WS E2E PASS ===")
    finally:
        p.send_signal(signal.SIGINT)
        try: p.wait(timeout=2)
        except subprocess.TimeoutExpired: p.kill(); p.wait()

if __name__ == "__main__":
    main()
