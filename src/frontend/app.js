// =============================================================
// app.js —— 聊天室前端逻辑
// 后端约定:
//   登录:   POST /api/login           {username}        → {ok, token, username}
//   拉用户: GET  /api/users?token=…   → {users: [...]}
//   拉历史: GET  /api/messages?token=… → {messages: [...]}
//   WS 推:  ws://host/ws?token=…     每帧一个 JSON, 格式:
//           {type:"chat", from, room, content, time}
//   WS 发:  ws.send(JSON.stringify({type:"chat", room:"lobby", content}))
// =============================================================

const TOKEN    = localStorage.getItem('token');
const ME       = localStorage.getItem('username');
const messagesEl = document.getElementById('messages');
const userListEl = document.getElementById('userList');
const msgInputEl = document.getElementById('msgInput');
const sendFormEl = document.getElementById('sendForm');
const logoutBtn  = document.getElementById('logout');
const meEl       = document.getElementById('me');
const wsStatusEl = document.getElementById('wsStatus');

let ws = null;          // 当前 WebSocket
let reconnectTimer = null;

// ---- 守卫:没 token 直接跳回登录页 ----
if (!TOKEN || !ME) {
    location.href = '/';
}
meEl.textContent = ME;

// ============================================================
// 1. WebSocket 连接
// ============================================================
function setStatus(state) {
    wsStatusEl.className = 'status status-' + state;
    wsStatusEl.textContent = {
        connected:    '已连接',
        connecting:   '连接中…',
        disconnected: '已断开',
    }[state] || state;
}

function connectWs() {
    setStatus('connecting');
    // HTTPS 页面必须用 WSS,否则浏览器 Mixed Content 拦截
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(`${proto}//${location.host}/ws?token=${TOKEN}`);

    ws.onopen = () => {
        setStatus('connected');
        console.log('[ws] connected');
    };

    // TODO: 实现 ws.onmessage
    // 收到的每条 e.data 是一段 JSON, 直接 JSON.parse。
    // 根据 msg.type 分发:
    //   - "chat"  → appendMessage(msg.from, msg.content, msg.from === ME, msg.time)
    //   - "system"→ appendSystem(msg.content)        (后端当前不发,可先不实现)
    //   - "user_list" → renderUserList(msg.users)   (后端当前不发,可先不实现)
    // 收完后,别忘了自动滚到底部(scrollToBottom)
    ws.onmessage = (e) => {
        let msg
        try{msg=JSON.parse(e.data);}catch(_){return;}
        
        if(msg.type==="chat"){appendMessage(msg.from,msg.content,msg.from === ME,msg.time);}
    };

    ws.onclose = () => {
        setStatus('disconnected');
        console.log('[ws] closed, reconnect in 3s');
        // 断线 3 秒后重连
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(connectWs, 3000);
    };

    ws.onerror = (e) => {
        console.error('[ws] error', e);
    };
}

// ============================================================
// 2. 发送消息
// ============================================================
sendFormEl.addEventListener('submit', (e) => {
    e.preventDefault();
    const content = msgInputEl.value.trim();
    if (!content) return;
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        alert('未连接到服务器,请稍后再试');
        return;
    }
    // TODO: 发送 JSON 格式 {type:"chat", room:"lobby", content}
    // 发送后:清空输入框 + 重新聚焦
    ws.send(JSON.stringify({
        type:"chat",
        room:"lobby",
        content:content
    }))
    msgInputEl.value="";
    msgInputEl.focus();
});

// 回车直接发送(input 里 keydown Enter 已经在 form submit 里覆盖,这里无需额外)

// ============================================================
// 3. 渲染消息
// ============================================================
function appendMessage(from, content, isOwn, time) {
    const wrap = document.createElement('div');
    wrap.className = 'message' + (isOwn ? ' own' : '');

    const meta = document.createElement('div');
    meta.className = 'meta';
    const t = time ? new Date(time) : new Date();
    const hh = String(t.getHours()).padStart(2, '0');
    const mm = String(t.getMinutes()).padStart(2, '0');
    meta.textContent = `${from || '?'}  ${hh}:${mm}`;

    const bubble = document.createElement('div');
    bubble.className = 'bubble';
    bubble.textContent = content;

    wrap.appendChild(meta);
    wrap.appendChild(bubble);
    messagesEl.appendChild(wrap);
    scrollToBottom();
}

function appendSystem(text) {
    const wrap = document.createElement('div');
    wrap.className = 'message system';
    const bubble = document.createElement('div');
    bubble.className = 'bubble';
    bubble.textContent = text;
    wrap.appendChild(bubble);
    messagesEl.appendChild(wrap);
    scrollToBottom();
}

function scrollToBottom() {
    messagesEl.scrollTop = messagesEl.scrollHeight;
}

// ============================================================
// 4. 用户列表
// ============================================================
async function loadUserList() {
    try {
        const res = await fetch(`/api/users?token=${TOKEN}`);
        const data = await res.json();
        if (data.users) renderUserList(data.users);
    } catch (e) {
        console.error('[users] load failed', e);
    }
}

function renderUserList(users) {
    userListEl.innerHTML = '';
    for (const u of users) {
        const li = document.createElement('li');
        li.textContent = u;
        if (u === ME) li.classList.add('me');
        userListEl.appendChild(li);
    }
}

// ============================================================
// 5. 拉历史消息
// ============================================================
async function loadHistory() {
    try {
        const res = await fetch(`/api/messages?token=${TOKEN}&limit=50`);
        const data = await res.json();
        if (Array.isArray(data.messages)) {
            for (const m of data.messages){
                appendMessage(m.from, m.content, m.from === ME, m.time);
            }
        }
    } catch (e) {
        console.error('[history] load failed', e);
    }
}

// ============================================================
// 6. 退出
// ============================================================
logoutBtn.addEventListener('click', async () => {
    if (ws) ws.close();
    try {
        await fetch('/api/logout', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({token: TOKEN}),
        });
    } catch (_) {}
    localStorage.removeItem('token');
    localStorage.removeItem('username');
    location.href = '/';
});

// ============================================================
// 7. 启动
// ============================================================
loadHistory();        // 先拉历史
loadUserList();       // 再拉用户
connectWs();          // 再开 WS(顺序无所谓,反正 init 都是异步)
msgInputEl.focus();
