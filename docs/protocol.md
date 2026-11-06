# 控制协议 v0

rdmachine 的客户端和服务端之间有两条连接：

- **控制连接**：TCP + TLS 1.3，默认端口 4100。登录、选择显示器、下发会话密钥、会话控制都走这里，本文描述的就是它。
- **画面连接**：rdaccel 自己的 UDP 传输（默认同为 4100 端口），画面、光标、键鼠和剪贴板走这里。UDP 不通时退回 rdaccel 的 TCP 传输，端口是控制端口加 1（默认 4101），因为 TCP 4100 已被控制连接占用。

控制连接先完成登录，再为这次会话生成一把新的 32 字节密钥和一个一次性令牌，经 TLS 交给客户端，客户端拿它们去建 rdaccel 连接。这样 rdaccel 每次都拿到新密钥，密码不会出现在 UDP 上，也不需要改 rdaccel（rdaccel 本身只有预共享密钥，没有前向保密和服务器身份，见 [rdaccel 能力评估](research/rdaccel-assessment.md)）。

> 版本 0 是 M0 期间的不稳定版本，两端必须一致。字段和消息在版本 1 之前都可能变。

## 一次连接的完整流程

```text
客户端                                                    服务端
  │ ── TCP 连接 4100 ─────────────────────────────────────▶ │
  │ ◀═════════════ TLS 1.3 握手（服务端自签证书）══════════▶ │  客户端核对证书指纹（首次连接时让用户确认）
  │ ── hello {protocol, client_name, client_version} ─────▶ │
  │ ◀── server_hello {protocol, server_name, auth_methods} ─ │
  │ ── auth {method:"password", username, password} ──────▶ │  服务端用系统账号校验（PAM）
  │ ◀── auth_result {ok:true} ───────────────────────────── │
  │ ── session_request {display_index, transport} ────────▶ │  服务端生成 key + auth_token，
  │ ◀── session_ready {port, key, auth_token, displays} ─── │  用 key 开 rdaccel Host 等待连接
  │                                                          │
  │ ══ rdaccel UDP：rda_udp_connect(host, port)，key 加密，auth_token 进 on_auth ══▶ │
  │ ◀══════════════════ 画面 / 光标 / 键鼠 / 剪贴板 ══════════════════════════════▶ │
  │                                                          │
  │ ── ping / pong（控制连接保活）────────────────────────── │
  │ ── bye {reason} ──────────────────────────────────────▶ │  双方关闭 rdaccel 会话和控制连接
```

控制连接在整个会话期间保持打开。任何一端关闭控制连接，另一端都要结束对应的 rdaccel 会话。

## TLS 与服务器身份

- 只接受 TLS 1.3。
- 服务端首次启动时生成自签证书（ECDSA P-256），保存在服务端配置目录，之后一直使用同一张证书。
- 客户端不校验证书链，而是核对**证书公钥（SubjectPublicKeyInfo）的 SHA-256 指纹**：
  - 首次连接某台主机：显示指纹，用户确认后保存（和 SSH 首次连接一样）。服务端界面或命令行能显示本机指纹，供用户比对。
  - 之后连接：指纹必须一致，不一致时直接拒绝并明确提示“服务器身份已变化”，不提供“仍然连接”的快捷按钮。
- 换证书（重装系统等）后，用户在客户端里删掉旧指纹再重新确认。
- 实现：`core/include/rdmachine/tls.h`（基于 Mbed TLS 3.6，`GenerateTlsIdentity`、`CertificateFingerprint`、`TlsListener`、`TlsConnect`）。客户端拿到指纹后先交给 `ClientOptions::trust_server` 判断，通过之前不发送任何消息。

## 分帧

控制连接上的每条消息是：

```text
[ 4 字节大端长度 N ][ N 字节 UTF-8 JSON 对象 ]
```

- N 必须在 1 到 65536 之间。收到 0 或超过 65536 的长度时，接收方直接关闭连接（协议错误，不回 `error`）。
- JSON 必须是对象，并带字符串字段 `type`。
- 实现：`core/include/rdmachine/frame.h`（`EncodeFrame`、`FrameDecoder`）。

## 消息

实现：`core/include/rdmachine/messages.h`（`SerializeMessage`、`ParseMessage`）。

通用规则：

- 不认识的字段一律忽略，方便以后加字段。
- 不认识的 `type`、缺字段、字段类型不对：接收方回 `error`（`code` 为 `bad_message`）后关闭连接。
- 二进制字段用标准 base64（RFC 4648，带填充）。
- 状态机之外的消息（比如没登录就发 `session_request`）：回 `error`（`code` 为 `unexpected`）后关闭。

### hello（客户端 → 服务端）

连接建立后客户端发的第一条消息。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `protocol` | 整数 ≥ 0 | 协议版本，现在是 0 |
| `client_name` | 字符串 | 客户端机器名，用于日志和以后的审批弹窗 |
| `client_version` | 字符串 | rdmachine 版本 |

### server_hello（服务端 → 客户端）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `protocol` | 整数 | 服务端的协议版本。和客户端不一致时，服务端发完 `server_hello` 紧接着发 `error`（`incompatible`）并关闭，客户端提示升级 |
| `server_name` | 字符串 | 服务端机器名 |
| `server_version` | 字符串 | rdmachine 版本 |
| `auth_methods` | 字符串数组 | 服务端接受的登录方式，按推荐顺序：`password`、`pairing_code`。客户端忽略不认识的方式 |

### auth（客户端 → 服务端）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `method` | 字符串 | `password` 或 `pairing_code` |
| `username` / `password` | 字符串 | `password` 方式：被控 Mac 上的账号和密码 |
| `code` | 字符串 | `pairing_code` 方式：被控端屏幕上显示的配对码 |

- **账号密码**：服务端用系统认证（macOS 上走 PAM），M0 只允许登录当前已登录图形会话的那个用户（D5：M0 只控制已登录用户的物理桌面）。
- **配对码**：服务端按需生成，在被控 Mac 上显示；8 位数字，5 分钟过期，用一次就作废，输错 3 次作废。只在 TLS 指纹已确认的连接上使用。配对码目前没有用 PAKE，首次连接时如果用户不核对指纹，存在被中间人转发的可能；以后换成 SPAKE2 一类的标准方案。
- 服务端不记录密码和配对码，日志里只写方式、用户名和结果。

### auth_result（服务端 → 客户端）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `ok` | 布尔 | 是否通过 |
| `error` | 字符串 | `ok` 为 false 时必有，见下文错误码 |
| `retry_after_ms` | 整数，可选 | 被限流时，多久后才能再试 |

失败后连接保持打开，客户端可以再发 `auth`。限流：同一来源 IP 1 分钟内失败 5 次后，1 分钟内的所有 `auth` 都直接回 `rate_limited`。

### session_request（客户端 → 服务端）

登录成功后发。所有字段都可省略。

| 字段 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `display_index` | 整数 ≥ 0 | 0 | 采集哪块显示器，0 是主显示器（同 rdaccel 的 `display_index`） |
| `transport` | 字符串 | `udp` | `udp` 或 `tcp`。客户端 UDP 建连超时后，可以在同一条控制连接上用 `tcp` 再请求一次：服务端先结束正在进行的会话，再按新请求开始（新的密钥和令牌） |
| `view_only` | 布尔 | false | 只看不控：服务端丢弃这个会话的键鼠事件 |

### session_ready（服务端 → 客户端）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `transport` | 字符串 | 实际使用的 rdaccel 传输：`udp` 或 `tcp` |
| `port` | 整数 1–65535 | rdaccel 监听的端口 |
| `key` | base64，32 字节 | 本次会话的 rdaccel 密钥，每次会话重新随机生成 |
| `auth_token` | base64，32 字节 | 一次性令牌，客户端放进 rdaccel viewer 的 `auth_token`；服务端在 `on_auth` 里用常数时间比较核对，用一次就作废，30 秒内没用也作废 |
| `displays` | 数组 | 可选的显示器：`index`、`name`、`width`、`height`（像素）、`scale`（每点像素数） |

客户端收到后立刻用 `rda_udp_connect(host, port)`（或 `rda_tcp_connect`）加上 `key`、`auth_token` 创建 rdaccel viewer。`key` 和 `auth_token` 只在内存里保存，用完清零。

### ping / pong（双向）

`{"type":"ping","id":<整数>}`，对方回同样 `id` 的 `pong`。控制连接空闲时每 15 秒一次，45 秒收不到任何消息就认为断开。画面连接的保活由 rdaccel 自己负责。

### bye（双向）

`{"type":"bye","reason":"<原因>"}`，发完关闭。原因：`user_closed`（用户主动断开）、`server_shutdown`、`session_replaced`（同一用户从别处连进来）、`idle_timeout`。

### error（双向）

`{"type":"error","code":"<错误码>","message":"<给人看的说明，可选>"}`，发完关闭连接。

## 实现

- 服务端：`ServerSession`（`core/include/rdmachine/server_session.h`）是不做 I/O 的状态机，按上面的规则处理每条消息；平台相关的部分（PAM 校验密码、启动 rdaccel Host）由 `ServerDelegate` 提供。`ControlServer` 在 TLS 上为每个连接开一个线程驱动它，负责 15 秒无流量发 `ping`、45 秒收不到消息发 `bye`（`idle_timeout`）后关闭。
- 限流、配对码、一次性令牌：`core/include/rdmachine/auth_guards.h`。账号密码和配对码的失败都计入同一个按 IP 的限流。
- 客户端：`ConnectAndStartSession`（`core/include/rdmachine/control_client.h`）走完 TLS、指纹确认、`hello`、`auth`、`session_request`，成功后把控制连接留给调用方在会话期间使用。除了上表的错误码，客户端还会报 `connect_failed`、`untrusted_server`、`timeout`、`connection_closed`、`bad_message`。
- 测试：`core/tests/test_server_session.cpp`（状态机）和 `core/tests/test_control.cpp`（本机回环端到端，包括指纹拒绝、限流、配对码、忙、坏帧、空闲超时）。

## 错误码

| 错误码 | 出现在 | 含义 |
| --- | --- | --- |
| `bad_credentials` | `auth_result` | 账号密码或配对码不对 |
| `not_console_user` | `auth_result` | M0：账号不是当前已登录图形会话的用户 |
| `code_expired` | `auth_result` | 配对码过期或已作废 |
| `rate_limited` | `auth_result` | 失败太多，看 `retry_after_ms` |
| `method_unsupported` | `auth_result` | 服务端没开这种登录方式 |
| `bad_message` | `error` | 消息格式不对 |
| `unexpected` | `error` | 消息不符合当前状态 |
| `incompatible` | `error` | 协议版本不一致 |
| `no_display` | `error` | 请求的显示器不存在 |
| `capture_denied` | `error` | 服务端没有“屏幕录制”权限，无法开始会话 |
| `busy` | `error` | 已有会话在进行（M0 一次只服务一个客户端） |
| `internal` | `error` | 服务端内部错误，详情看服务端日志 |

## 以后会加的

- 公钥登录（类似 SSH 的 authorized_keys）和被控端审批弹窗（NoMachine 的 0/1/2 三档）：M1。
- 登录窗口和锁屏时的连接：M1（服务端拆成 LaunchDaemon + 用户 / 登录窗口会话里的 agent，协议本身不变）。
- 切换显示器、文件传输协商、断线后恢复同一会话：M1 起逐步加消息类型。
