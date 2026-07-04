#!/usr/bin/env python3
"""
命令行聊天客户端（测试用）

协议帧: 4字节长度(包含type字节) + 1字节type + body
类型:
  1 LOGIN      2 LOGIN_ACK     3 CHAT     4 CHAT_BROAD
  5 USER_LIST  6 PRIVATE       7 PING     8 PONG
  9 LOGOUT    10 SYSTEM

用法:
  ./chat_client.py <username> [host] [port]
  默认 127.0.0.1:9000

交互命令:
  普通文字           → CHAT
  /msg <to> <text>  → PRIVATE
  /list             → USER_LIST
  /ping             → PING
  /quit             → LOGOUT + 退出
"""

import socket, struct, json, sys, threading, select

NAMES = {1:'LOGIN',2:'ACK',3:'CHAT',4:'CHAT_BROAD',5:'USER_LIST',
         6:'PRIVATE',7:'PING',8:'PONG',9:'LOGOUT',10:'SYSTEM'}

def enc(t, body=b''):
    if isinstance(body, str): body = body.encode()
    return struct.pack('!IB', len(body) + 1, t) + body

def dec(sock):
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

def reader(sock, user):
    while True:
        r = dec(sock)
        if not r:
            print(f'\r[{user}] [disconnected]\n> ', end='', flush=True)
            return
        t, body = r
        tag = NAMES.get(t, f'TYPE{t}')
        print(f'\r[{user}] <- {tag}: {body}\n> ', end='', flush=True)

def main():
    user = sys.argv[1] if len(sys.argv) > 1 else 'alice'
    host = sys.argv[2] if len(sys.argv) > 2 else '127.0.0.1'
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 9000

    s = socket.create_connection((host, port))
    s.sendall(enc(1, json.dumps({'username': user})))

    threading.Thread(target=reader, args=(s, user), daemon=True).start()

    print(f'[{user}] connected to {host}:{port}. Type /quit to exit.')
    print('> ', end='', flush=True)
    try:
        while True:
            line = sys.stdin.readline()
            if not line: break
            line = line.rstrip('\n')
            if not line: continue
            if line.startswith('/msg '):
                _, to, content = line.split(' ', 2)
                s.sendall(enc(6, json.dumps({'to': to, 'content': content})))
            elif line == '/list':
                s.sendall(enc(5, b''))
            elif line == '/ping':
                s.sendall(enc(7, b''))
            elif line == '/quit':
                s.sendall(enc(9, b''))
                break
            else:
                s.sendall(enc(3, json.dumps({'content': line})))
            print('> ', end='', flush=True)
    except (EOFError, KeyboardInterrupt):
        pass

if __name__ == '__main__':
    main()