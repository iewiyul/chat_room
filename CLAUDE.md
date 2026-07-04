# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目状态

`IMPLEMENTATION_GUIDE.md` 是按 5 个阶段设计的课程项目。当前已实现到 **阶段 1 末尾**（手写 TCP 聊天室：自定义包协议 + Buffer + epoll Reactor + Connection + UserManager + MessageHandler + ConnectionManager + 心跳超时）。

**未实现**：阶段 2（HTTP）、阶段 3（WebSocket）、阶段 4（前端）。`src/server/` 下**没有** `http_server.*`、`websocket.*`、`http_handler.*`；**没有** `src/frontend/` 目录；`main.cpp` 只 listen TCP 端口（argv[1]，默认 9000），没有 8080。修改或扩展时按 IMPLEMENTATION_GUIDE 推进。

## 构建与测试

**用 CMake**（`CMakeLists.txt` 在项目根）。测试源码统一在 `tests/unit/`，被测代码在 `src/`，输出到 `build/`。

**构建 + 跑所有测试**（日常用这一行）：
```bash
cmake -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

**只跑单个测试**：
```bash
ctest --test-dir build -R test_user_manager --output-on-failure
# 或直接跑 binary：
./build/test_user_manager
```

**运行服务端**：
```bash
./build/chat_server 9000      # argv[1] = TCP 端口
```

**E2E 集成测试**（`tests/test_e2e.py`）：
```bash
python3 tests/test_e2e.py
# 默认启 ./build/chat_server，端口 19001
# 自定义：CHAT_SERVER=./build/chat_server CHAT_PORT=19001 python3 tests/test_e2e.py
```
脚本会自己 spawn server、跑 9 个用例（登录 / 广播 / 私聊 / ping / 用户列表 / 登出广播 / 对端断线 / 重名 / 未登录发消息）、最后 SIGINT 关 server。

**E2E 集成测试**（`tests/test_e2e.py`）：
```bash
python3 tests/test_e2e.py
# 默认启 ./build/chat_server，端口 19001
# 自定义：CHAT_SERVER=./build/chat_server CHAT_PORT=19001 python3 tests/test_e2e.py
```
脚本会自己 spawn server、跑 9 个用例（登录 / 广播 / 私聊 / ping / 用户列表 / 登出广播 / 对端断线 / 重名 / 未登录发消息）、最后 SIGINT 关 server。

**交互客户端**：`tools/chat_client.py <username> [host] [port]`，支持 `/msg <to> <text>` `/list` `/ping` `/quit`。

**注意**：`test_reactor.cpp` 硬编码使用了端口 `19997/19998/19999`。如果之前有 `chat_server` 占着这些端口，对应 `test_tcp_echo_*` 用例会失败——跑测试前先 `pkill -f chat_server`。

## 架构

### 单线程 epoll Reactor（`src/server/reactor.cpp`）

- `Reactor::loop()` 单线程跑 `epoll_wait(1000ms)`；超时（n==0）时调用 `tickCb_`（被 `main.cpp` 接给 `ConnectionManager::tick()`，用于心跳淘汰）
- `addFd` / `modifyFd` / `removeFd` 维护 `unordered_map<int, FdContext>`，回调用 `std::function`
- **实际是 level-triggered**（没设 `EPOLLET`），与 IMPLEMENTATION_GUIDE 1.4 的"边缘触发"建议不一致——`Connection::onRead` 仍按 ET 写法（读到 EAGAIN 才退出 while），所以工作正确但每次 epoll 唤醒可能被多次通知
- `running_` 标志由 SIGINT/SIGTERM 经 `g_reactor->stop()` 翻 false，**跨线程安全**

### 包协议（`src/protocol/packet.cpp` + `message.h`）

线缆格式：
```
┌──────────────┬──────────┬────────────────┐
│ 4 字节长度    │ 1 字节类型│ N 字节载荷      │
│ (type+body)  │          │ JSON UTF-8     │
└──────────────┴──────────┴────────────────┘
```
长度字段是大端（`htonl` / `ntohl`），**包含** 1 字节 type + body 的总字节数。`MAX_BODY_LENGTH = 64*1024`。`decode()` 返回 `consumed == -1` 表示数据不足（粘包半包），`consumed == 0 && !ok` 表示协议错（关闭连接）。

`MessageType` 枚举见 `message.h`（MSG_LOGIN=1 ... MSG_SYSTEM=10）。JSON 字段名集中在 `chat::protocol::field::*`，handler 用 `json.value(field::XXX, default)` 取。

### Connection + Buffer（粘包/半包封装）

`Connection::onRead()` 循环 `recv` 到 `readBuf`，再循环 `decode` 出完整包，每包调 `onPacket_(conn, pkt)` 回调。`Connection::send()` 把包 `encode` 后 append 到 `writeBuf`，首次发送时 `modifyFd(EPOLLIN|EPOLLOUT)`；`onWrite()` 把 writeBuf 写完后再改回 `EPOLLIN`。`writing_` 标志防止重复注册 EPOLLOUT。

`Buffer`（`src/common/buffer.cpp`）是 Netty ByteBuf 风格：`std::vector<uint8_t>` + `readPos_`/`writePos_`；`append` 触发 `makeSpace`（先压缩再 2 倍扩容）。`peek() + readableBytes()` 给 `decode()`，`consume(n)` 推进读指针。

### MessageHandler 状态机

- `MSG_LOGIN`：解析 username，`UserManager::add` 失败回 `LOGIN_ACK ok=false "用户名已存在"`，成功则 setUsername + 广播 MSG_SYSTEM("xxx上线") + MSG_USER_LIST
- `MSG_CHAT`：未登录 → LOGIN_ACK 拒；构造 `{from, content, time}` JSON，`UserManager::broadcast(bp, conn)` **除自己外**广播
- `MSG_PRIVATE`：`UserManager::get(to)` 拿目标 `Connection*`，只发给它一份
- `MSG_USER_LIST`：只回给请求者（**不广播**当前列表）
- `MSG_PING` → `MSG_PONG`
- `MSG_LOGOUT`：从 `UserManager` 移除 + `Connection::close()`（close 回调里 `handler.onClose` 会再广播一次"xxx离线"+ USER_LIST —— 见下方陷阱）
- 未实现的：MSG_CHAT_BROAD、MSG_PONG、MSG_LOGIN_ACK 由 server 主动发出，不在 dispatch switch 中

### 心跳与离线（`ConnectionManager::tick`）

每 1 秒 Reactor 超时 → `connMgr.tick(30)` 遍历 `unordered_set<Connection*>`，`now - lastActiveTime > 30s` 关闭。

⚠️ **`tick()` 里有陷阱注释**：`c->close()` 会触发 `onClose_` → `handler.onClose` → `users_.remove(conn)` + 广播 SYSTEM/USER_LIST → 回到 `connMgr.remove(c)` → 在迭代中 `erase` 当前元素 → 迭代器失效。修复是先把要关的 conn 收集到 `vector`，迭代完再关。

### 关闭路径的两条线

1. **对端断开**：`Connection::onRead` 收到 0 字节 → `close()` → `onClose_` 回调
2. **超时淘汰**：`ConnectionManager::tick` → `c->close()` → 同上
3. **主动 LOGOUT**：`handler.handleLogOut` → `users_.remove` + `conn->close()` → onClose_ 还会再触发一次"离线"广播

`onClose` 里只在 `AUTHENTICATED` 才广播，避免给未登录的连接发 SYSTEM。

## 关键不变量

- `UserManager` **没有加锁**，依赖 Reactor 单线程模型；任何多线程访问要先加 `mutex`
- `ConnectionManager::tick` 必须在 Reactor 线程里跑（与 epoll 同一线程），否则 fd 状态竞争
- `ChatRoom` 测试 fixture（`tests/unit/test_chat_fixture.h`）里**故意 sleep 10ms** 等 `Reactor::loop()` 跑起来再 `stop()`——`stop()` 写 `running_=false`，但 `loop()` 启动时会再写回 `true`，注释里写了原因
- 测试用 `socketpair(AF_UNIX)` 模拟 client，**peer 端的 fd 走 `MSG_DONTWAIT recv`**，与生产环境的真实 socket 行为略有不同（不会有 EAGAIN 后又来数据的真实场景）

## 调试提示

- 协议帧：`tcpdump` / `wireshark` 看二进制（端口 9000）
- EAGAIN 是正常"读完"的信号，不是错误
- 客户端用 `nc` 也能发：`python3 -c 'import struct,json,sys; sys.stdout.buffer.write(struct.pack("!IB",len(json.dumps({"username":"alice"}).encode())+1,1)+json.dumps({"username":"alice"}).encode())' | nc localhost 9000`
