# 聊天室项目实施指南

> 目标：巩固网络编程 + 理解前后端分离
> 技术栈：C++ + 手写 socket/epoll + 手写 HTTP + 手写 WebSocket + 原生前端

---

## 项目最终效果

- 浏览器打开 `http://localhost:8080/`，输入用户名登录
- 进入聊天室，看到在线用户列表和历史消息
- 输入消息，所有在线用户实时收到
- 支持私聊、心跳、断线重连

```
┌──────────────────────────────────────────────┐
│ 当前用户: alice              [退出]            │
├──────────┬───────────────────────────────────┤
│ 在线用户  │  [系统] bob 上线                   │
│ ● alice  │  [系统] charlie 上线                │
│ ● bob    │  alice: 大家好                     │
│ ● charlie│  bob: 你好 alice                   │
│          │  charlie: hi                       │
│          │                                   │
│          ├───────────────────────────────────┤
│          │ [输入消息...]              [发送]   │
└──────────┴───────────────────────────────────┘
```

---

## 推荐项目结构

```
6_chat_room/
├── docs/
│   └── PROTOCOL.md              # 各阶段协议说明
├── third_party/
│   └── json.hpp                # nlohmann/json 单头文件
├── src/
│   ├── common/                 # 公共代码
│   │   ├── log.h               # 日志宏
│   │   ├── buffer.h/.cpp       # 读/写缓冲区（自动处理粘包）
│   │   └── utils.h/.cpp        # 字节序、字符串分割等工具
│   ├── protocol/               # TCP 自定义协议
│   │   ├── packet.h/.cpp       # 消息包定义 + 编解码
│   │   └── message.h           # 消息结构 + JSON 字段常量
│   ├── server/
│   │   ├── connection.h/.cpp   # 单个客户端连接
│   │   ├── user_manager.h/.cpp # 在线用户表
│   │   ├── room.h/.cpp         # 房间 / 消息存储
│   │   ├── reactor.h/.cpp      # epoll 事件循环
│   │   ├── tcp_server.h/.cpp   # TCP 监听 + 接受连接
│   │   ├── http_server.h/.cpp  # HTTP 请求解析 + 响应
│   │   ├── websocket.h/.cpp    # WS 握手 + 帧解析
│   │   ├── message_handler.h/.cpp   # TCP 消息分发
│   │   ├── http_handler.h/.cpp     # HTTP API 处理
│   │   └── main.cpp            # 入口
│   └── frontend/
│       ├── index.html          # 登录页
│       ├── chat.html           # 聊天室页
│       ├── style.css
│       └── app.js
├── Makefile
└── README.md
```

每个阶段会逐步把上面这些文件填满。

---

## 阶段 1：TCP 聊天室（纯 socket + epoll）

### 1.1 设计自定义协议

**目标**：定义客户端与服务端之间传输的消息格式。

**包格式**（大端字节序）：

```
┌──────────────┬──────────┬────────────────┐
│ 4 字节长度    │ 1 字节类型│ N 字节载荷      │
│ (含 type+body)│          │ (JSON 字符串)   │
└──────────────┴──────────┴────────────────┘
```

**消息类型枚举**：

```cpp
enum MessageType : uint8_t {
    MSG_LOGIN        = 1,   // 客户端 -> 服务端：登录
    MSG_LOGIN_ACK    = 2,   // 服务端 -> 客户端：登录结果
    MSG_CHAT         = 3,   // 客户端 -> 服务端：发送消息
    MSG_CHAT_BROAD   = 4,   // 服务端 -> 客户端：广播消息
    MSG_USER_LIST    = 5,   // 服务端 -> 客户端：在线用户列表
    MSG_PRIVATE      = 6,   // 私聊
    MSG_PING         = 7,   // 心跳请求
    MSG_PONG         = 8,   // 心跳响应
    MSG_LOGOUT       = 9,   // 主动登出
    MSG_SYSTEM       = 10,  // 系统消息（如 "XXX 上线"）
};
```

**载荷示例（JSON）**：

```json
// MSG_LOGIN
{"username": "alice"}

// MSG_CHAT
{"room": "lobby", "content": "hello"}

// MSG_CHAT_BROAD
{"from": "alice", "room": "lobby", "content": "hello", "time": 1719120000}

// MSG_USER_LIST
{"users": ["alice", "bob", "charlie"]}

// MSG_PRIVATE
{"to": "bob", "content": "private hi"}
```

**验收标准**：
- 用纸笔画出每个消息的二进制布局
- 写好 `packet.h` 的结构定义和常量

---

### 1.2 实现协议编解码

**目标**：把 `Packet` 对象 ↔ 字节流互转。

**`Packet` 结构**：

```cpp
struct Packet {
    uint8_t  type;
    std::string body;  // JSON 字符串
};
```

**编码函数签名**：

```cpp
// 把 Packet 编码成字节流（4字节长度 + 1字节类型 + body）
std::vector<uint8_t> encode(const Packet& pkt);

// 从字节流解码（输入可能不完整）
// 返回值: -1 表示数据不够；>=0 表示消耗的字节数
std::pair<int, Packet> tryDecode(const uint8_t* data, size_t len);
```

**关键技术点**：
- 字节序：长度字段用 `htonl` / `ntohl`
- 长度字段**包含**类型字段 + body 的总字节数
- 拆包逻辑：先看够不够 4 字节拿长度，再看够不够长度字节拿完整包

**验收标准**：
- 写一个 `encode -> decode` 的单元测试（main 函数里临时测一下）
- 验证粘包场景：把两个包拼成一个 buffer，解码后能正确得到两个 `Packet`

---

### 1.3 实现读/写缓冲区

**目标**：把"半包/粘包"问题封装在一个类里。

**`Buffer` 类接口**：

```cpp
class Buffer {
public:
    void append(const uint8_t* data, size_t len);  // 接收数据
    void consume(size_t len);                      // 处理掉前 N 字节
    const uint8_t* data() const;
    size_t size() const;
    std::vector<uint8_t> readAll();                // 取出所有
};
```

底层用 `std::vector<uint8_t>` + `readPos_` + `writePos_`，类似 Netty 的 `ByteBuf`。

**验收标准**：
- 模拟"分两次收到一个完整包"，能正确解码
- 模拟"一次收到两个完整包"，能解码出两个包

---

### 1.4 epoll Reactor 框架

**目标**：单进程多客户端的事件循环。

**核心结构**：

```cpp
class Reactor {
public:
    void addFd(int fd, uint32_t events, EventCallback onRead, EventCallback onWrite);
    void modifyFd(int fd, uint32_t events);
    void removeFd(int fd);
    void loop();   // 主循环
private:
    int epollFd_;
    std::unordered_map<int, FdContext> contexts_;
};
```

**`FdContext` 包含**：
- `fd`
- 当前关心的事件
- `onRead` / `onWrite` 回调（`std::function`）
- 用户自定义数据指针（用于存放 `Connection*`）

**关键技术点**：
- `epoll_create1(EPOLL_CLOEXEC)`
- `EPOLLIN | EPOLLET`（边缘触发）
- 错误处理：`errno == EAGAIN` 表示读完了，下次再读

**验收标准**：
- 写一个测试：监听 `:9999`，accept 后 echo 数据
- 用 `nc localhost 9999` 验证

---

### 1.5 Connection 类

**目标**：封装单个客户端连接的状态。

```cpp
class Connection {
public:
    enum State { CONNECTING, AUTHENTICATED, CLOSED };
    int fd;
    Buffer readBuf;
    Buffer writeBuf;            // 待发送数据
    std::string username;       // 登录后填
    State state = CONNECTING;
    time_t lastActiveTime;      // 用于心跳超时
};
```

**关键逻辑**：
- 读事件触发 → 从 fd 读数据到 `readBuf` → 循环解析包 → 调用 `MessageHandler`
- 写事件触发 → 把 `writeBuf` 数据写到 fd → 写完移除 `EPOLLOUT`
- 发送数据时，先 append 到 `writeBuf`，然后 `modifyFd(EPOLLIN | EPOLLOUT)`

---

### 1.6 实现登录与广播

**目标**：用户登录后能广播消息。

**`UserManager` 类**：

```cpp
class UserManager {
public:
    bool addUser(const std::string& name, Connection* conn);
    void removeUser(const std::string& name);
    Connection* getConnection(const std::string& name) const;
    std::vector<std::string> getOnlineList() const;
private:
    std::unordered_map<std::string, Connection*> users_;
    std::mutex mutex_;
};
```

**`MessageHandler::onMessage(conn, pkt)` 逻辑**：

```
switch (pkt.type) {
    case MSG_LOGIN:
        if (userMgr.addUser(pkt.body.username, conn)) {
            conn->state = AUTHENTICATED;
            conn->username = pkt.body.username;
            sendLoginAck(conn, true);
            broadcast(MSG_SYSTEM, "XXX 上线");
            broadcastUserList();
        } else {
            sendLoginAck(conn, false, "用户名已存在");
        }
    case MSG_CHAT:
        if (conn->state != AUTHENTICATED) return;
        broadcast(MSG_CHAT_BROAD, {from: conn->username, content: pkt.body.content});
    // ...
}
```

**验收标准**：
- 启动服务端，开 3 个 `nc` 连接
- 用某个用户名登录，其他连接都能收到"XXX 上线"系统消息
- 重复用户名登录会失败

---

### 1.7 心跳与离线检测

**目标**：客户端掉线时自动清理。

**方案**：
- 服务端起一个 1 秒 tick 定时器（用 `epoll_wait` 的超时参数）
- 每 tick 遍历所有连接，`now - lastActiveTime > 30s` 则关闭连接
- 收到任何消息更新 `lastActiveTime`
- 客户端可发 `MSG_PING`，服务端回 `MSG_PONG`（这一步可先用 nc 模拟）

**验收标准**：
- 启动一个连接登录后，kill 进程，对端在 30 秒内收到"XXX 离线"
- 在线用户列表自动更新

---

### 1.8 阶段 1 测试清单

- [ ] 两个 nc 客户端同时连，互相发消息都能收到
- [ ] 重复用户名登录被拒绝
- [ ] 主动断开客户端，对端收到离线通知
- [ ] kill -9 客户端，服务端 30s 内清理
- [ ] 1000 个并发连接无崩溃（用 `wrk` 或自己写个脚本）

---

## 阶段 2：HTTP 服务（手写）

### 2.1 HTTP 协议学习

**请求格式**：

```
GET /index.html HTTP/1.1\r\n
Host: localhost:8080\r\n
User-Agent: curl/7.81.0\r\n
\r\n
[body]
```

**响应格式**：

```
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 1234\r\n
Connection: keep-alive\r\n
\r\n
<body>
```

**关键点**：
- 用 `\r\n` 换行
- Header 与 Body 之间有**一个空行**（`\r\n`）
- `Content-Length` 决定 Body 多长
- `Connection: close` 表示短连接；`keep-alive` 表示长连接
- 第一阶段实现只支持 `Connection: close`，简化逻辑

---

### 2.2 单端口 vs 多端口

**问题**：HTTP（8080）和 TCP（9999）可以共用端口吗？

**答案**：技术上可以（监听同一个端口，根据请求首字节判断），但**建议你用两个端口**：
- `9999`：阶段 1 的 TCP 自定义协议
- `8080`：HTTP

简化复杂度，专注于理解 HTTP 本身。

---

### 2.3 HTTP 请求解析

**目标**：实现 `HttpRequest` 结构体。

```cpp
struct HttpRequest {
    std::string method;     // "GET" / "POST"
    std::string path;       // "/api/login"
    std::string query;      // "a=1&b=2"
    std::string version;    // "HTTP/1.1"
    std::map<std::string, std::string> headers;
    std::string body;
};
```

**解析步骤**：
1. 找到第一个 `\r\n\r\n`，之前是 Header，之后是 Body
2. 第一行按空格分割得到 method / path / version
3. 中间行按 `: ` 分割得到 header key / value
4. 看 `Content-Length` 决定 Body 长度（必须读够）

**关键技术点**：
- 复用阶段 1 的 `Buffer` 类
- 解析失败返回 400
- 注意 query string 解析：`a=1&b=2` → `{"a": "1", "b": "2"}`

---

### 2.4 HTTP 响应封装

```cpp
class HttpResponse {
public:
    HttpResponse(int code = 200, const std::string& reason = "OK");
    void setHeader(const std::string& k, const std::string& v);
    void setBody(const std::string& body);
    std::string toString() const;  // 序列化成字节流
private:
    int statusCode_;
    std::string reason_;
    std::map<std::string, std::string> headers_;
    std::string body_;
};
```

**辅助方法**：

```cpp
HttpResponse makeJsonResponse(int code, const json& body);
HttpResponse makeErrorResponse(int code, const std::string& msg);
HttpResponse makeStaticFileResponse(const std::string& path);
```

**验收标准**：
- `toString()` 输出符合 HTTP/1.1 格式
- 静态文件支持 `.html` / `.css` / `.js` / `.png`，返回正确 MIME

---

### 2.5 静态文件服务

**API**：
- `GET /` → 返回 `frontend/index.html`
- `GET /style.css` → 返回 `frontend/style.css`
- `GET /app.js` → 返回 `frontend/app.js`

**关键技术点**：
- MIME 表：`{".html": "text/html", ".css": "text/css", ".js": "application/javascript"}`
- 路径安全：拒绝包含 `..` 的路径，防目录穿越
- 文件不存在返回 404

**验收标准**：
- 浏览器访问 `http://localhost:8080/` 能看到（哪怕是空的）HTML
- 直接访问 `http://localhost:8080/style.css` 能下载到文件

---

### 2.6 REST API 设计

**接口清单**：

| 方法 | 路径 | 说明 | 入参 | 出参 |
|------|------|------|------|------|
| POST | `/api/login` | 登录 | `{username}` | `{ok: true, token: "xxx"}` |
| GET  | `/api/users` | 在线用户列表 | - | `{users: ["alice", "bob"]}` |
| GET  | `/api/messages?room=lobby&limit=50` | 拉取历史消息 | - | `{messages: [...]}` |
| POST | `/api/messages` | 发消息（HTTP 方式） | `{room, content}` | `{ok: true}` |
| POST | `/api/logout` | 登出 | - | `{ok: true}` |

**Token 设计**：
- 登录成功后生成一个随机字符串作为 token
- 服务端维护 `token -> username` 映射
- 客户端后续请求带 `?token=xxx` 或 Header `Authorization: Bearer xxx`

**关键技术点**：
- 引入第三方 JSON 库：[nlohmann/json](https://github.com/nlohmann/json)，单头文件 `json.hpp`
- 把 JSON 字符串和 `json` 对象互转

**验收标准**（用 curl 测试）：
```bash
curl -X POST http://localhost:8080/api/login -d '{"username":"alice"}'
curl http://localhost:8080/api/users?token=xxx
curl -X POST http://localhost:8080/api/messages?token=xxx -d '{"room":"lobby","content":"hi"}'
curl http://localhost:8080/api/messages?room=lobby
```

---

### 2.7 业务层接入

**目标**：把 HTTP API 和阶段 1 的 `UserManager`、`Room` 关联起来。

```cpp
// HttpHandler::handle(req, conn) -> HttpResponse
if (req.path == "/api/login")      return handleLogin(req);
if (req.path == "/api/users")      return handleUserList(req);
if (req.path == "/api/messages") {
    if (req.method == "GET")       return handleGetMessages(req);
    if (req.method == "POST")      return handleSendMessage(req);
}
return makeErrorResponse(404, "Not Found");
```

**`Room` 类**：

```cpp
class Room {
public:
    void addMessage(const Message& msg);          // 持久化 + 加到 deque
    std::vector<Message> recentMessages(int n);   // 最近 N 条
private:
    std::deque<Message> messages_;                // 最近 1000 条
    std::mutex mutex_;
};
```

阶段 2 暂时把消息存内存，阶段 5 再加持久化。

**验收标准**：
- 阶段 1 的 nc 客户端发的消息，能通过 `GET /api/messages` 看到
- HTTP API 发的消息，nc 客户端也能收到广播（两个端口的连接共享同一个 `Room`）

---

### 2.8 阶段 2 测试清单

- [ ] `GET /` 返回 `index.html`
- [ ] 静态资源路径穿越攻击被拦截（`GET /../etc/passwd`）
- [ ] 登录返回正确 token
- [ ] 错误 token 访问 API 返回 401
- [ ] POST 消息后 GET 能拉到
- [ ] HTTP 发的消息，TCP 客户端也能收到

---

## 阶段 3：WebSocket（手写）

### 3.1 WebSocket 原理

**握手**（基于 HTTP）：

客户端请求：
```
GET /ws HTTP/1.1
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
```

服务端响应：
```
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

**`Sec-WebSocket-Accept` 算法**：
```
accept = base64(sha1(Sec-WebSocket-Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
```

---

### 3.2 WebSocket 帧格式

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
|     Extended payload length continued, if payload len == 127  |
+ - - - - - - - - - - - - - - - +-------------------------------+
|                               |Masking-key, if MASK set to 1  |
+-------------------------------+-------------------------------+
| Masking-key (continued)       |          Payload Data         |
+-------------------------------- - - - - - - - - - - - - - - - +
:                     Payload Data continued ...                :
+ - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
|                     Payload Data continued ...                |
+---------------------------------------------------------------+
```

**关键字段**：
- `FIN`：是否为最后一帧（0=还有续帧，1=最后一帧）
- `opcode`：0x1=文本, 0x2=二进制, 0x8=关闭, 0x9=Ping, 0xA=Pong
- `MASK`：客户端发送必须为 1，服务端发送必须为 0
- `Payload len`：0-125 直接是长度；126 表示后面 2 字节是长度；127 表示后面 8 字节

---

### 3.3 WS 握手实现

**目标**：把 HTTP 连接升级为 WS 连接。

**在 `HttpHandler` 里识别升级请求**：

```cpp
if (req.method == "GET"
    && req.headers["Upgrade"] == "websocket"
    && req.headers["Connection"] == "Upgrade") {
    return handleWebSocketUpgrade(req, conn);
}
```

**`handleWebSocketUpgrade` 步骤**：
1. 校验 `Sec-WebSocket-Version == 13`
2. 计算 `Sec-WebSocket-Accept`
3. 构造 101 响应并立即 flush
4. 把 `Connection::state` 改为 `WS_CONNECTED`
5. 修改 epoll 关注的事件（不变，但后续读到的数据按 WS 帧解析）

---

### 3.4 WS 帧解析

**目标**：实现 `WsFrame` 结构体和解析函数。

```cpp
struct WsFrame {
    bool fin;
    uint8_t opcode;
    uint64_t payloadLen;
    std::vector<uint8_t> payload;  // 解码后的数据
};

std::pair<int, WsFrame> tryDecodeWsFrame(const uint8_t* data, size_t len);
```

**解析步骤**：
1. 第 1 字节：`fin = (b & 0x80) >> 7`，`opcode = b & 0x0F`
2. 第 2 字节：`mask = (b & 0x80) >> 7`，`len7 = b & 0x7F`
3. 如果 `len7 == 126`，再读 2 字节作为 `payloadLen`
4. 如果 `len7 == 127`，再读 8 字节作为 `payloadLen`
5. 如果 `mask == 1`，再读 4 字节 masking key
6. 读 `payloadLen` 字节的 payload，并按 masking key 解码（XOR）

**封装（发送）**：

```cpp
std::vector<uint8_t> encodeWsFrame(const std::string& payload, uint8_t opcode = 0x1);
```

服务端发送不需要 mask：
1. 第 1 字节：`0x80 | opcode`
2. 第 2 字节：`payloadLen`（< 126 直接写；否则扩展）
3. payload

---

### 3.5 心跳（Ping/Pong）

**目标**：维护长连接活性。

**服务端逻辑**：
- 每 30 秒向所有 WS 连接发 `Ping`（opcode 0x9）
- 收到 `Pong`（opcode 0xA）更新 `lastActiveTime`
- 收到 `Ping` 回 `Pong`
- 60 秒没收到任何帧（包括 Pong）→ 关闭连接

**复用**阶段 1 的定时器机制。

---

### 3.6 消息协议设计

**目标**：在 WS payload 里也用 JSON，字段和阶段 1 的 `MSG_CHAT` 对齐。

```json
{"type": "chat", "room": "lobby", "content": "hi"}
{"type": "system", "content": "bob 上线"}
{"type": "user_list", "users": ["alice", "bob"]}
```

这样前端只需解析一种 JSON 格式，不管走 HTTP 还是 WS。

---

### 3.7 阶段 3 测试清单

- [ ] 用 `wscat -c ws://localhost:8080/ws?token=xxx` 能连上
- [ ] 浏览器 console 写 `new WebSocket(...)` 能连上
- [ ] 客户端发文本消息，服务端能收到
- [ ] 服务端推送文本消息，客户端能收到
- [ ] 发送 Ping 收到 Pong
- [ ] 客户端断网后服务端 60s 清理
- [ ] WS 发出的消息，TCP 客户端（阶段 1）也能收到广播

---

## 阶段 4：前端

### 4.1 文件结构

```
src/frontend/
├── index.html   # 登录页（阶段 2 的 GET / 返回）
├── chat.html    # 聊天室页（GET /chat 返回）
├── style.css
└── app.js       # 所有 JS 逻辑（先全部塞一个文件）
```

---

### 4.2 登录页（`index.html`）

**结构**：

```html
<form id="loginForm">
    <h1>聊天室</h1>
    <input id="username" placeholder="输入用户名" required>
    <button>登录</button>
    <div id="error"></div>
</form>
<script>
document.getElementById('loginForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const username = document.getElementById('username').value.trim();
    const res = await fetch('/api/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username})
    });
    const data = await res.json();
    if (data.ok) {
        localStorage.setItem('token', data.token);
        localStorage.setItem('username', username);
        location.href = '/chat';
    } else {
        document.getElementById('error').textContent = data.error;
    }
});
</script>
```

---

### 4.3 聊天室页（`chat.html`）

**结构**：

```html
<div class="app">
    <header>
        当前用户: <span id="me"></span>
        <button id="logout">退出</button>
    </header>
    <div class="main">
        <aside class="users">
            <h3>在线用户</h3>
            <ul id="userList"></ul>
        </aside>
        <section class="chat">
            <div id="messages" class="messages"></div>
            <form id="sendForm" class="input-bar">
                <input id="msgInput" placeholder="输入消息...">
                <button>发送</button>
            </form>
        </section>
    </div>
</div>
<script src="/app.js"></script>
```

---

### 4.4 CSS（`style.css`）

**布局要点**：
- 用 Flexbox 三栏：左用户列表 / 中聊天区
- 聊天区上半部分是消息列表（`overflow-y: auto`），下半部分固定输入栏
- 消息气泡：自己发的靠右、其他人的靠左，系统消息居中灰色

```css
.app { display: flex; flex-direction: column; height: 100vh; }
.main { display: flex; flex: 1; overflow: hidden; }
.users { width: 200px; border-right: 1px solid #ddd; padding: 1rem; }
.chat { flex: 1; display: flex; flex-direction: column; }
.messages { flex: 1; overflow-y: auto; padding: 1rem; }
.input-bar { display: flex; padding: 1rem; border-top: 1px solid #ddd; }
.input-bar input { flex: 1; }
```

---

### 4.5 JS 逻辑（`app.js`）

**核心对象**：

```js
const TOKEN = localStorage.getItem('token');
const ME = localStorage.getItem('username');

// 1. WebSocket 连接
let ws;
function connectWs() {
    ws = new WebSocket(`ws://${location.host}/ws?token=${TOKEN}`);
    ws.onopen = () => { console.log('ws connected'); };
    ws.onmessage = (e) => handleMessage(JSON.parse(e.data));
    ws.onclose = () => setTimeout(connectWs, 3000);  // 断线重连
    ws.onerror = (e) => console.error(e);
}

// 2. 发送消息
function sendMessage(content) {
    ws.send(JSON.stringify({type: 'chat', room: 'lobby', content}));
}

// 3. 处理收到的消息
function handleMessage(msg) {
    if (msg.type === 'chat') {
        appendMessage(msg.from, msg.content, msg.from === ME);
    } else if (msg.type === 'system') {
        appendSystemMessage(msg.content);
    } else if (msg.type === 'user_list') {
        renderUserList(msg.users);
    }
}

// 4. 初始化：拉历史消息 + 拉用户列表
async function init() {
    const res = await fetch(`/api/messages?room=lobby&limit=50&token=${TOKEN}`);
    const data = await res.json();
    data.messages.forEach(m => handleMessage(m));
    connectWs();
}
init();
```

---

### 4.6 优化体验

- 回车发送（监听 input 的 keydown）
- 收到新消息自动滚到底部
- 时间显示（每条消息前缀时间，5 分钟内合并）
- 输入框聚焦
- 退出按钮：清 localStorage，跳回 `/`

---

### 4.7 阶段 4 测试清单

- [ ] 浏览器打开 `http://localhost:8080`，输入用户名登录成功
- [ ] 跳转到聊天页，能看到自己
- [ ] 在两个浏览器窗口（不同用户名）登录
- [ ] 一个窗口发消息，另一个实时收到
- [ ] 关掉一个浏览器，对面看到"XXX 离线"
- [ ] 关闭服务端再启动，前端断线重连成功

---

## 附录 A：编译与构建

**Makefile 关键点**：

```makefile
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2 -pthread
INCLUDES = -I src -I third_party
LDFLAGS = -pthread

SRC = $(wildcard src/**/*.cpp)
OBJ = $(SRC:.cpp=.o)

TARGET = chatroom

all: $(TARGET)
$(TARGET): $(OBJ)
    $(CXX) -o $@ $^ $(LDFLAGS)

clean:
    rm -f $(OBJ) $(TARGET)
```

运行：

```bash
make
./chatroom 9999 8080   # TCP 端口 HTTP 端口
```

---

## 附录 B：调试技巧

1. **协议调试**：阶段 1 用 `nc` 模拟客户端，`tcpdump` 或 `wireshark` 抓包看二进制
2. **HTTP 调试**：`curl -v` 看完整请求响应
3. **WS 调试**：浏览器 F12 → Network → WS 标签看帧
4. **日志**：每个连接 accept 后打日志，每个错误路径都打日志
5. **gdb**：学习用 `gdb --args ./chatroom 9999 8080` 调试 core dump

---

## 附录 C：推荐参考资源

- **HTTP**：RFC 2616 / RFC 7230
- **WebSocket**：RFC 6455
- **epoll**：`man epoll` / `man epoll_wait`
- **nlohmann/json**：[GitHub README](https://github.com/nlohmann/json)
- **HTTP 调试**：[httpbin.org](https://httpbin.org/) 测试各种请求
- **WS 测试**：`wscat`（`npm install -g wscat`）

---

## 完成度检查表

| 阶段 | 关键能力 | 是否完成 |
|------|---------|---------|
| 1.1 | 协议设计 | ☐ |
| 1.2 | 编解码 | ☐ |
| 1.3 | Buffer | ☐ |
| 1.4 | epoll Reactor | ☐ |
| 1.5 | Connection | ☐ |
| 1.6 | 登录广播 | ☐ |
| 1.7 | 心跳掉线 | ☐ |
| 2.3 | HTTP 解析 | ☐ |
| 2.5 | 静态文件 | ☐ |
| 2.6 | REST API | ☐ |
| 3.1 | WS 握手 | ☐ |
| 3.4 | WS 帧解析 | ☐ |
| 3.5 | WS 心跳 | ☐ |
| 4.2 | 登录页 | ☐ |
| 4.3 | 聊天页 | ☐ |
| 4.5 | 前端逻辑 | ☐ |

完成所有项后，你就拥有了一个完整的手写协议栈，并且对前后端分离、HTTP/WS 协议、epoll 都有扎实的理解。