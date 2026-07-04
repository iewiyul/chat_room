#!/usr/bin/env python3
"""
1.8 端到端集成测试

跑法:
  ./build/chat_server 编译完
  python3 tests/test_e2e.py

覆盖场景:
  1. alice / bob 登录
  2. CHAT 广播（bob 收到 alice 的消息）
  3. PRIVATE 单播
  4. PING/PONG
  5. USER_LIST
  6. LOGOUT 触发离线广播
  7. 心跳超时掉线（短 timeout，需要修改 server，或传环境变量）
"""

import os, sys, time, signal, socket, struct, json, subprocess, threading

HOST = '127.0.0.1'
PORT = int(os.environ.get('CHAT_PORT', '19001'))
SERVER_BIN = os.environ.get('CHAT_SERVER', './build/chat_server')

# ---------- 协议工具 ----------

def enc(t, body=b''):
    if isinstance(body, str): body = body.encode()
    return struct.pack('!IB', len(body) + 1, t) + body

def dec(sock, timeout=1.0):
    sock.settimeout(timeout)
    try:
        h = b''
        while len(h) < 4:
            d = sock.recv(4 - len(h))
            if not d: return None
            h += d
        body_len = struct.unpack('!I', h)[0]
        rest = b''
        while len(rest) < body_len:
            d = sock.recv(body_len - len(rest))
            if not d: return None
            rest += d
        return rest[0], rest[1:].decode('utf-8', 'replace')
    except socket.timeout:
        return None

def recv_until(sock, type_, timeout=2.0):
    """循环收包直到拿到指定 type_；超时返回 None"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        remain = max(0.05, deadline - time.time())
        r = dec(sock, remain)
        if not r: return None
        if r[0] == type_: return r
    return None

# ---------- 测试用例 ----------

class TestRunner:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, cond, msg):
        if cond:
            print(f'  ✅ {msg}')
            self.passed += 1
        else:
            print(f'  ❌ {msg}')
            self.failed += 1

    def section(self, name):
        print(f'\n▶ {name}')

def login(name, server_port=PORT):
    s = socket.create_connection((HOST, server_port))
    s.sendall(enc(1, json.dumps({'username': name})))
    return s

def setup_two(server_port):
    """登录 alice/bob，返回 (alice, bob)；登录阶段已 drain 干净"""
    a = login('alice', server_port)
    b = login('bob',   server_port)
    recv_until(a, 2)
    recv_until(b, 2)
    for _ in range(4):
        for s in (a, b): dec(s, 0.1)
    return a, b

def t_login(server_port):
    r = TestRunner()
    r.section('登录')
    a = login('alice', server_port)
    b = login('bob',   server_port)
    ack_a = recv_until(a, 2)
    r.check(ack_a is not None, 'alice 收到 LOGIN_ACK')
    if ack_a: r.check(json.loads(ack_a[1])['ok'] is True, 'alice ack.ok == true')
    ack_b = recv_until(b, 2)
    r.check(ack_b is not None, 'bob 收到 LOGIN_ACK')
    if ack_b: r.check(json.loads(ack_b[1])['ok'] is True, 'bob ack.ok == true')
    return r

def t_chat(server_port):
    r = TestRunner()
    a, b = setup_two(server_port)
    r.section('CHAT 广播')
    a.sendall(enc(3, json.dumps({'content': 'hello-bob'})))
    chat = recv_until(b, 4)
    r.check(chat is not None, 'bob 收到 CHAT_BROAD')
    if chat:
        body = json.loads(chat[1])
        r.check(body['from'] == 'alice', 'from == alice')
        r.check(body['content'] == 'hello-bob', 'content 正确')
    return r

def t_private(server_port):
    r = TestRunner()
    a, b = setup_two(server_port)
    r.section('PRIVATE 单播')
    a.sendall(enc(6, json.dumps({'to': 'bob', 'content': 'secret'})))
    priv = recv_until(b, 6)
    r.check(priv is not None, 'bob 收到 PRIVATE')
    if priv:
        body = json.loads(priv[1])
        r.check(body['from'] == 'alice', 'from == alice')
        r.check(body['to']   == 'bob',   'to == bob')
        r.check(body['content'] == 'secret', 'content 正确')
    return r

def t_ping(server_port):
    r = TestRunner()
    a, _ = setup_two(server_port)
    r.section('PING/PONG')
    a.sendall(enc(7, b''))
    pong = recv_until(a, 8)
    r.check(pong is not None, 'alice 收到 PONG')
    return r

def t_user_list(server_port):
    r = TestRunner()
    a, _ = setup_two(server_port)
    r.section('USER_LIST 查询')
    a.sendall(enc(5, b''))
    ulist = recv_until(a, 5)
    r.check(ulist is not None, 'alice 收到 USER_LIST')
    if ulist:
        names = set(json.loads(ulist[1])['users'])
        r.check('alice' in names and 'bob' in names,
                f'users 包含 alice 和 bob (got {names})')
    return r

def t_logout_broadcast(server_port):
    r = TestRunner()
    a, b = setup_two(server_port)
    r.section('LOGOUT 触发离线广播')
    a.sendall(enc(9, b''))
    sys_pkt = recv_until(b, 10, timeout=2.0)
    r.check(sys_pkt is not None, 'bob 收到 SYSTEM')
    if sys_pkt:
        body = json.loads(sys_pkt[1])
        r.check('alice' in body['content'], f'内容含 alice (got "{body["content"]}")')
    ulist = recv_until(b, 5, timeout=2.0)
    r.check(ulist is not None, 'bob 收到 USER_LIST')
    if ulist:
        names = json.loads(ulist[1])['users']
        r.check(names == ['bob'], f'只剩 bob (got {names})')
    return r

def t_peer_disconnect(server_port):
    """对端关掉 clientFd → server 广播离线"""
    r = TestRunner()
    a, b = setup_two(server_port)
    r.section('对端断线 → 广播离线')
    a.close()
    sys_pkt = recv_until(b, 10, timeout=2.0)
    r.check(sys_pkt is not None, 'bob 收到 SYSTEM')
    if sys_pkt:
        body = json.loads(sys_pkt[1])
        r.check('alice' in body['content'], f'内容含 alice (got "{body["content"]}")')
    return r

def t_duplicate_login(server_port):
    r = TestRunner()
    _a, _b = setup_two(server_port)   # 必须接住，否则 socket 立刻 GC 关闭
    r.section('重名登录被拒')
    c = login('alice', server_port)
    ack = recv_until(c, 2)
    r.check(ack is not None, '第二个 alice 收到 LOGIN_ACK')
    if ack:
        body = json.loads(ack[1])
        r.check(body['ok'] is False, 'ok == false')
        r.check('已存在' in body.get('error', ''), f'error 含"已存在" (got "{body.get("error")}")')
    return r

def t_chat_requires_login(server_port):
    """未登录就发 CHAT → 应该被拒（LOGIN_ACK false "未登录"）"""
    r = TestRunner()
    r.section('未登录发 CHAT 被拒')
    s = socket.create_connection((HOST, server_port))
    s.sendall(enc(3, json.dumps({'content': 'unauth'})))
    ack = recv_until(s, 2)
    r.check(ack is not None, '收到 LOGIN_ACK')
    if ack:
        body = json.loads(ack[1])
        r.check(body['ok'] is False, 'ok == false')
        r.check('未登录' in body.get('error', ''), f'error 含"未登录" (got "{body.get("error")}")')
    return r

# ---------- main ----------

def main():
    # 启动 server
    server = subprocess.Popen(
        [SERVER_BIN, str(PORT)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    try:
        # 等 server 起来
        time.sleep(0.5)
        if server.poll() is not None:
            print('server died:', server.stdout.read().decode())
            return 1

        total = TestRunner()
        for fn in [t_login, t_chat, t_private, t_ping, t_user_list,
                   t_logout_broadcast, t_peer_disconnect,
                   t_duplicate_login, t_chat_requires_login]:
            r = fn(PORT)
            total.passed += r.passed
            total.failed += r.failed
            a = b = None  # cleanup hint

        print('\n========== E2E 结果 ==========')
        print(f'通过: {total.passed}')
        print(f'失败: {total.failed}')
        return 0 if total.failed == 0 else 1
    finally:
        server.send_signal(signal.SIGINT)
        try: server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()

if __name__ == '__main__':
    sys.exit(main())