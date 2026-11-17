# 控制协议（基础版本 0，修订 3）

rdmachine 的客户端和服务端之间有两条连接：

- **控制连接**：TCP + TLS 1.3，默认端口 4100。登录、选择显示器、下发会话密钥、会话控制都走这里，本文描述的就是它。
- **画面连接**：rdaccel 自己的 UDP 传输（默认同为 4100 端口），画面、光标、键鼠和剪贴板走这里。UDP 不通时退回 rdaccel 的 TCP 传输，端口是控制端口加 1（默认 4101），因为 TCP 4100 已被控制连接占用。

控制连接先完成登录，再为这次会话生成一把新的 32 字节密钥和一个一次性令牌，经 TLS 交给客户端，客户端拿它们去建 rdaccel 连接。这样 rdaccel 每次都拿到新密钥，密码不会出现在 UDP 上，也不需要改 rdaccel（rdaccel 本身只有预共享密钥，没有前向保密和服务器身份，见 [rdaccel 能力评估](research/rdaccel-assessment.md)）。

> **版本和兼容**（4.0.0 起）：`protocol` 是基础版本，固定为 0，两端必须一样；只有旧版本跟不上的改动才会换基础版本（没有计划）。在它之上兼容地加东西用**修订**：hello 和 server_hello 各带自己的 `revision`（没有就是 0，3.4 及更早），两边都按较小的那个用；修订 N 的功能只在协商出的修订不低于 N 时用，旧的一方没有这个功能，照常工作。修订加的字段和消息都是可选的，旧的一方忽略它们。所以新旧版本可以互连，不用两边同时升级。修订 0：3.4 及以前的一切（包括后来加的可选字段 `desktop`、`text_mode` 等）；修订 1：修订本身；修订 2：扩展桌面（见“扩展桌面”）；两者都从 4.0.0 起。修订 3：声音（见“声音”），4.4.0 起。实现在 `core/include/rdmachine/protocol.h`（`kProtocolRevision`、`NegotiatedRevision`）。

## 一次连接的完整流程

```text
客户端                                                    服务端
  │ ── TCP 连接 4100 ─────────────────────────────────────▶ │
  │ ◀═════════════ TLS 1.3 握手（服务端自签证书）══════════▶ │  客户端核对证书指纹（首次连接时让用户确认）
  │ ── hello {protocol, client_name, client_version} ─────▶ │
  │ ◀── server_hello {protocol, server_name, auth_methods} ─ │
  │ ── auth {method:"password", username, password} ──────▶ │  服务端用系统账号校验（PAM）
  │ ◀── auth_result {ok:true} ───────────────────────────── │
  │ ── session_request {desktop, transport} ──────────────▶ │  服务端按 desktop 建虚拟显示器，生成 key + auth_token，
  │ ◀── session_ready {port, key, auth_token, desktop} ──── │  用 key 开 rdaccel Host 等待连接
  │                                                          │
  │ ══ rdaccel UDP：rda_udp_connect(host, port)，key 加密，auth_token 进 on_auth ══▶ │
  │ ◀══════════════════ 画面 / 光标 / 键鼠 / 剪贴板 ══════════════════════════════▶ │
  │                                                          │
  │ ── ping / pong（控制连接保活）────────────────────────── │
  │ ── bye {reason} ──────────────────────────────────────▶ │  双方关闭 rdaccel 会话和控制连接
```

控制连接在整个会话期间保持打开。任何一端关闭控制连接，另一端都要结束对应的 rdaccel 会话。

## 多个网络出口

画面连接用控制连接的那一对地址（3.2.1 起）：服务端把画面的 UDP 绑定在控制连接到达的本机地址上（`TlsConnection::local_ip`，经 `SessionOrigin::local_ip` 交给 `MediaHost`，装成服务时由守护进程转给 agent），客户端往控制连接实际连上的服务端地址发（`ClientConnection::peer_ip`），不再重新解析主机名。原来服务端绑在所有地址上，第一个包到了以后 `connect()` 到客户端，回包的源地址由路由表决定；被控端有多个出口（Wi-Fi 和网线、VPN、Tailscale）时，这个地址可能不是客户端发往的那个，客户端（UDP 已 connect 到原地址）收不到画面：登录成功、画面不来。绑定失败（比如带作用域的 IPv6 链路本地地址）时退回绑定所有地址。TCP 画面连接不受影响。

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
| `protocol` | 整数 ≥ 0 | 基础版本，0 |
| `revision` | 整数 ≥ 0，可选 | 客户端的修订（4.0.0 起是 1；没有就是 0） |
| `client_name` | 字符串 | 客户端机器名，用于日志和以后的审批弹窗 |
| `client_version` | 字符串 | rdmachine 版本 |

### server_hello（服务端 → 客户端）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `protocol` | 整数 | 服务端的基础版本。和客户端不一致时，服务端发完 `server_hello` 紧接着发 `error`（`incompatible`）并关闭，客户端提示升级 |
| `revision` | 整数，可选 | 服务端的修订；两边按较小的那个用 |
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
| `text_mode` | 布尔 | false | 以文字模式开始会话，见下文“文字模式” |
| `desktop` | 对象 | 无 | 客户端要的桌面：`width`、`height`（像素，≥ 1）、`scale`（每点像素数，可省略，默认 1）。服务端按它建虚拟显示器并共享那块屏，这时 `display_index` 不起作用。见下文“请求桌面” |
| `extra_desktops` | 数组 | 无 | 扩展桌面的其余显示器（修订 2，最多 3 项，要和 `desktop` 一起）。见下文“扩展桌面” |
| `audio` | 布尔 | false | 客户端要被控 Mac 的声音（修订 3）。见下文“声音” |

### session_ready（服务端 → 客户端）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `transport` | 字符串 | 实际使用的 rdaccel 传输：`udp` 或 `tcp` |
| `port` | 整数 1–65535 | rdaccel 监听的端口 |
| `key` | base64，32 字节 | 本次会话的 rdaccel 密钥，每次会话重新随机生成 |
| `auth_token` | base64，32 字节 | 一次性令牌，客户端放进 rdaccel viewer 的 `auth_token`；服务端在 `on_auth` 里用常数时间比较核对，用一次就作废，30 秒内没用也作废 |
| `displays` | 数组 | 可选的显示器：`index`、`name`、`width`、`height`（像素）、`scale`（每点像素数） |
| `desktop` | 对象，可选 | 为这次请求建好的桌面（`width`、`height`、`scale`，尺寸可能被调整过）。没有这个字段：客户端没请求，或者服务端建不了，画面是服务端自己的显示器，客户端按比例缩放显示 |
| `login_window` | 布尔，可选 | `true`：画面是被控 Mac 的登录窗口（还没有人登录）。用户在那里登录后会话迁移到他的桌面，见下文“会话迁移”。没有这个字段等于 `false` |
| `extra_displays` | 数组，可选 | 为 `extra_desktops` 建好的显示器：`port`、`desktop`、`x`、`y`（修订 2）。密钥和令牌由会话的派生，见“扩展桌面” |
| `audio` | 布尔，可选 | `true`：这次会话有声音（修订 3），客户端给主画面的 viewer 打开声音。没有这个字段等于 `false` |
| `audio_reason` | 字符串，可选 | `audio` 不为 `true` 而客户端要了声音时说明原因：`disabled`（服务端设置关了）、`login_window`（登录窗口没有声音）、`tcp`（画面走 TCP，TCP 上没有声音）、`unsupported`（系统或构建不支持）。没有权限不在这里说（会话中途可能授权），在会话统计里 |

客户端收到后立刻用 `rda_udp_connect(host, port)`（或 `rda_tcp_connect`）加上 `key`、`auth_token` 创建 rdaccel viewer。`key` 和 `auth_token` 只在内存里保存，用完清零。

### ping / pong（双向）

`{"type":"ping","id":<整数>}`，对方回同样 `id` 的 `pong`。心跳（3.1.0 起加快）：客户端每 2 秒发一次 ping，4 秒没收到服务端的任何消息就在画面上显示“连接中断”，12 秒就认为断开并自动重连；服务端 5 秒没收到消息就发 ping，15 秒收不到任何消息就断开。画面连接的保活由 rdaccel 自己负责。

### bye（双向）

`{"type":"bye","reason":"<原因>"}`，发完关闭。原因：`user_closed`（用户主动断开）、`server_shutdown`、`session_replaced`（另一个客户端登录后接管了会话，见下文“接管”）、`idle_timeout`、`console_changed`（被控 Mac 上换了另一个人登录，见“会话迁移”）、`server_disconnected`（被控端菜单里点了“断开当前连接”，客户端不自动重连）。服务端停止时先发 `bye`（`server_shutdown`）再关连接，客户端据此自动重连。

### 接管

会话进行中又有一个客户端登录并发 `session_request`：服务端给旧连接发 `bye`（`session_replaced`）并关闭，等它的会话结束（最多 5 秒），再为新连接开始会话；新连接不会收到 `busy`。自用场景下，这多半是同一个人换了一台电脑连。

### session_moving（服务端 → 客户端）

`{"type":"session_moving","reason":"<原因>"}`，会话进行中才发：画面要换到被控 Mac 的另一个图形会话（有人在登录窗口登录、退出登录、切换用户）。旧的 rdaccel 连接随后断开，控制连接保持；之后同一条连接上会再来一条 `session_ready`（新的端口、密钥、令牌，`login_window` 表示新画面是不是登录窗口），客户端用它建新的画面连接，也可能是 `bye` / `error`（迁移失败）。原因：`user_logged_in`、`user_logged_out`、`user_switched`、`agent_restarted`、`server_stopping`（这个会话的服务端被主动退出或停止：5 秒内回来就接着迁移过去，比如 `rdmachine-service restart`；不回来就发 `bye`（`server_shutdown`））。见下文“会话迁移”。

### error（双向）

`{"type":"error","code":"<错误码>","message":"<给人看的说明，可选>"}`，发完关闭连接。

## 请求桌面

要解决的问题：HiDPI 是被控端显示模式的属性。被控端是 1 倍的屏（没接显示器、普通外接屏、虚拟机）时，画面拉到 Retina 客户端上要么字很小、要么放大后发虚；像素数或宽高比和客户端窗口不一致时还要经过非整数倍缩放、留黑边。客户端怎么缩放都补不回来。

做法：客户端只说自己要什么样的桌面，服务端去建。

- **规格**（`DesktopSpec`）：`width`、`height` 是像素，`scale` 是每点像素数。Retina 屏上 1440 × 900 点的窗口请求 `{"width":2880,"height":1800,"scale":2}`。
- **服务端怎么取整**（`PlanDesktop`）：`scale` 归到 1 或 2（macOS 只有这两种；≥ 1.5 算 2），像素尽量原样保留（一个像素对一个像素比“一个点多大”重要）；宽高取偶数（点数是整数，视频编码也要偶数）；每边至少 320 点，最大 7680 × 4320 像素。尺寸或 `scale` 不是正数的请求当作没有请求。
- **建屏**（macOS，`CGVirtualDisplay`）：一块名为“RDMachine Desktop”的虚拟显示器，只有一个模式，就是请求的点数和像素数；它成为主显示器（rdaccel 的 0 号屏），真实显示器全部镜像它，所以所有窗口都在这块屏上，被控端屏幕上看到的是同一个桌面。没有显示器的 Mac 上它就是唯一的屏。
- **会话开始时**：`session_request.desktop`。建好后 `session_ready.desktop` 回实际规格，画面流的尺寸就是它的像素数。
- **窗口变化时**：客户端发 rdaccel 应用消息，类型 `0x10003`（`RDA_MSG_USER_FIRST + 3`，`kDesktopRequestMessageType`），客户端到服务端，内容是 JSON `{"width":2048,"height":1536,"scale":2}`。服务端停采集、原地改虚拟屏的模式（显示器不变）、重新开始采集，客户端收到新尺寸的画面流（rdaccel 的 `on_stream_info`）；没有单独的应答。连续几条只处理最新的一条；取整后和现在一样的不处理；会话开始时没建桌面的，这条消息忽略。Mac 客户端在拖动结束、进出全屏、窗口换到缩放不同的屏幕之后 0.25 秒发送，拖动过程中不发。
- **结束时**：会话结束（包括服务端进程退出或崩溃：虚拟屏随进程消失）后，真实显示器取消镜像，回到原来的位置和分辨率。
- **建不了时**：系统没有 `CGVirtualDisplay`、创建失败、Linux 服务端（暂时没有实现）：服务端照常开始会话，共享 `display_index` 指的那块真实显示器，`session_ready` 里没有 `desktop`，客户端按比例缩放显示（窗口保持服务端屏幕的宽高比）。不回错误。
- **键鼠**：坐标仍然是画面流里的像素。rdaccel 的注入器每次换算时读显示器当前的模式，桌面改过大小之后不用重建。
- **缩放模式的客户端屏幕**（MacBook Air 等默认“看起来像 1470 × 956”、面板却不是它的 2 倍）：客户端按窗口的后备像素（点数 × 2）请求，画面和窗口的后备存储一个像素对一个像素，再由 macOS 把整屏缩到面板上，和本机任何窗口的显示方式一样。按面板的物理像素请求反而更糊：画面要先放大到后备存储、再被系统缩小，经过两次重采样。窗口模式下应用绕不开系统的这次缩放。
- 控制协议只加了可选字段，版本仍是 0；旧服务端忽略 `desktop`，旧客户端不发。

注意：进程只有通过 AppKit 的事件循环才能得知新显示器的模式，所以 macOS 上服务端的主线程总是跑 `NSApplication`（`--headless` 也是，只是不显示任何界面）。

实现：`core/include/rdmachine/desktop.h`（规格、取整、消息）；`session/include/rdmachine/session/virtual_desktop.h`（`VirtualDesktop`，macOS 实现在 `session/src/virtual_desktop_mac.mm`）；服务端在 `MediaHost`（`StartSession` 里建，`DesktopLoop` 处理中途的请求）；Mac 客户端在 `client/main.mm`（`initialDesktop`、`requestDesktop`）；命令行客户端 `rdmachine-connect --desktop=2880x1800@2 --resize-desktop=2048x1536@2`。测试：`core/tests/test_desktop.cpp`、`session/tests/test_media_e2e.cpp`（用记录调用的假桌面）、`session/tests/test_virtual_desktop_mac.mm` 和 `tools/smoke_test.sh`（真的虚拟屏，会让本机屏幕重排几秒，要 `RDM_TEST_VIRTUAL_DESKTOP=1`）。

## 扩展桌面

修订 2（4.0.0）。客户端全屏、这台 Mac 有不止一块屏幕、“显示 → 全屏时使用所有显示器”开着（默认）时，`session_request` 除了 `desktop` 再带 `extra_desktops`：其余每块屏幕一项（最多 3 项），`{"desktop":{"width":1920,"height":1080,"scale":1},"x":1440,"y":0}`，`x`、`y` 是这块屏幕的左上角相对于主屏幕左上角的位置（点，y 向下），按客户端屏幕的排列。

服务端（macOS）先照常建主桌面（真实显示器镜像它），再为每项建一块虚拟显示器（“RDMachine Desktop 2”起），放在那个位置、不镜像，成为扩展桌面；系统可能把它挪到和别的显示器挨着，实际位置按 rdaccel 的显示器列表认出它是第几块。每块显示器一个 rdaccel host、一条画面连接，端口是画面端口加 2 加序号（4102、4103、4104；UDP 或 TCP 跟主画面一样），各有自己的键鼠注入（坐标是那块显示器自己的）。密钥和令牌不另外传：由会话的密钥派生（HMAC-SHA256，标签 `rdmachine extra display key <序号>` 和 `rdmachine extra display token <序号>`，令牌的输入还带上会话令牌），两边各算各的；每条连接的密钥都不同（同一把密钥用在两条连接上会重用 nonce）。`session_ready` 里 `extra_displays` 列出建好的：`{"port":4102,"desktop":{...},"x":1440,"y":0}`；建不了的不列，旧服务端不认识 `extra_desktops`，什么都不列，客户端照旧一块屏。

客户端在每块对应的屏幕上开一个无边框、盖住菜单栏的窗口，连上那块显示器的画面；鼠标移到哪块屏幕，输入就走哪条连接，服务端的指针跟着进那块显示器；键盘走当时的前台窗口，进的是同一个系统。剪贴板、统计和输入法同步只走主画面。切到别的应用时这些窗口隐藏。显示器在会话开始时建好，中途不变：进出全屏或改了设置，客户端用保存的登录重新连接一次（“正在切换显示器”）；会话迁移时客户端按新的 `session_ready` 重新连上各块。

会话期间服务端不让显示器因空闲睡眠（`PreventUserIdleDisplaySleep`，4.0.0），连接时还会唤醒睡着的显示器：显示器一睡截屏就停，画面停住直到远程那边点一下或按一下键。

## 文字模式

省流量用（按流量计费的链路）：体验和普通模式一样，流量更少。先保证体验，再在看不出来的地方省。静止的、以及正在打字的文字和界面减到每块 16 色（不够像时 32 或 64 色；只在边缘上有误差，平滑的地方不动）、用专门的编码立刻发出，这部分省 3–7 倍。画面在动的时候（滚动、进出全屏、影片）照普通模式整体走视频，停下后把文字无损补上；视频和码率跟普通模式完全一样（没有单独的视频额度）。滚动和拖窗省三到六成，影片和进出全屏基本不省。数字见 rdaccel 的 CHANGELOG 1.9.1。运动后留在画面上的杂色（白底上的浅色线和带、壁纸和图标边上的彩色细线）在 rdaccel 1.9.3（rdmachine 3.2.4）修掉了，原因见它的 CHANGELOG。rdaccel 1.9.4（rdmachine 3.2.6）起，滚动停下后只重发运动时先走了视频的部分，不再把整页文字重发、把照片重新打磨一遍（模拟里慢速滚动的流量少两到三成，快速滚动少百分之几，画质不变）。rdaccel 1.9.6（rdmachine 3.2.8）起，窗口移动时边上剩下的碎边角合成一块发：在真实录屏上拖窗口少 59%、滚动少 38%，此前拖窗口比普通模式还多 69%。

- **谁决定**：客户端（它知道自己的链路要不要省）。默认关：不在乎流量时普通模式画质更好（照片、视频、动画）。
- **会话开始时**：`session_request.text_mode`（可省略，默认 false）。
- **会话中途**：rdaccel 应用消息，类型 `0x10004`（`RDA_MSG_USER_FIRST + 4`，`kPictureModeMessageType`），客户端到服务端，内容是 JSON `{"text_mode":true}`。服务端调用 `rda_host_set_text_mode`，下一帧生效；没有单独的应答。关掉时服务端整屏刷新一次（换回原色）。不认识的字段忽略，以后的画面模式加在这个对象里。
- **客户端知道当前状态**：会话统计（见下）里的 `text_mode`；Mac 客户端的浮窗打开时多显示一行 “Data saver”。
- **桌面的分辨率**：文字模式下 Mac 客户端默认请求 1 倍的桌面（“请求桌面”的 `scale` 为 1，像素数 = 窗口的点数），即使窗口在 Retina 屏上：要发的像素是 2 倍桌面的四分之一。被控端按 1 倍渲染文字，客户端在窗口正好是画面整数倍大时用最近邻放大（每个像素变成 2 × 2，不做平滑）。切换文字模式时客户端重新请求桌面（开：1 倍；关：回到窗口的缩放系数）。菜单 View → Data Saver at Standard Resolution (1x) 可以关掉，默认开。服务端不需要知道这件事。
- Mac 客户端：菜单 View → Data Saver (Sharp Text)，选择会记住，下次连接直接用。命令行客户端：`rdmachine-connect --text-mode`（开始时），`--set-text-mode=on|off`（收到画面后切换）。
- 控制协议只加了可选字段，版本仍是 0；旧服务端忽略 `text_mode` 和这条消息（画面照常，只是不省），旧客户端不发。

实现：`core/include/rdmachine/picture_mode.h`（消息）；服务端在 `MediaHost`（`StartSession`、`OnMessage`）；Mac 客户端在 `client/main.mm`（`toggleTextMode:`、`sendPictureMode`）。测试：`core/tests/test_picture_mode.cpp`、`test_messages.cpp`、`session/tests/test_media_e2e.cpp`（`TextModeIsAskedForAtTheStartAndChangedWhileRunning`）。

## 声音

修订 3（5.0.0），设计和取舍见[声音](audio.md)。被控 Mac 的系统声音（Process Tap）用 Opus 编码，走 rdaccel 的数据报（不重传、排在画面前、加密），只有 UDP 上有；TCP 上没有声音。

- **会话开始时**：客户端在 `session_request` 里带 `audio: true`（Mac 客户端：菜单 显示 → 播放远程电脑的声音，默认开；命令行客户端：`--audio`，永远用空设备）。服务端设置“把这台 Mac 的声音传给客户端”开着、不是登录窗口时开始传，`session_ready.audio` 为 `true`；否则 `false` 加 `audio_reason`。客户端只给主画面的 viewer 打开声音（rdaccel 的 `rda_viewer_config.audio`），扩展桌面的其余画面没有。rdaccel 在 viewer 连上、握手里说了要声音时才开始采集（本机静音也从那时起），viewer 走了就停。
- **只在修订 ≥ 3 时用**：服务端在协商出的修订不够时把 `session_request.audio` 当成 false，回复里也不带这两个字段；旧服务端忽略这个字段、不回 `audio`，客户端就不开声音；旧客户端不发，服务端不采集。两个方向都照常工作，只是没有声音。
- **会话中途**：rdaccel 应用消息，类型 `0x10013`（`RDA_MSG_USER_FIRST + 0x13`，`kAudioControlMessageType`），客户端到服务端，JSON `{"paused":false}`。`paused` 为真：用户在菜单里关了声音，或者客户端的默认输出静了音、音量为 0（没人听得到）。服务端调用 `rda_host_set_audio_paused`，不编码也不发；本机照旧静音。恢复时当成新的一段声音。不认识的字段忽略。会话迁移后客户端把暂停状态再发一次。
- **档位**不单独传：服务端在会话开始时和每次收到文字模式消息时，`text_mode` 为真就用省流量的档位（20 ms 一帧、立体声 96 kbps），否则普通档位（10 ms、160 kbps）。
- **本机静音**：服务端设置“传声音时这台 Mac 自己不出声”（默认开），只在声音真的在往客户端传的会话里，且确认有“系统声音录制”权限之后；客户端暂停期间照旧静音。
- **守护进程**：`start_session.request` 原样带着 `audio`，`grant` 带回 `audio` / `audio_reason`；守护进程和 agent 之间没有新消息。

实现：`core/include/rdmachine/audio_control.h`（消息）；服务端在 `MediaHost`（`StartSession`、`OnMessage`、`StatsLoop`、`EndSession`）；客户端在 `RemoteSession`（`Options::audio`、`SetAudioPaused`，播放器跟着 viewer 建和拆）和 `client/main.mm`（`togglePlaySound:`、浮窗的“声音”一项）。测试：`core/tests/test_audio_control.cpp`、`test_messages.cpp`、`test_server_session.cpp`（`SoundOnlyFromRevision3`）、`session/tests/test_media_e2e.cpp`（`MediaE2EAudio.*`）、`test_daemon_e2e.cpp`（`SoundGoesThroughTheDaemonAndFollowsTheMove`）、`tools/smoke_test.sh`。

## 守护进程和 agent

装成服务（`rdmachine-service install`）以后，被控 Mac 上的服务端分成三份同一个程序（`RDMachine Server.app` 里的 `rdmachine-server`）：

- **守护进程**（`--daemon`，LaunchDaemon，root，开机就运行）：监听 TCP 4100，持有服务端身份（`/Library/Application Support/rdmachine`，安装时从用户的配置目录复制过来，客户端看到的指纹不变），负责登录（PAM）和配对码，按“谁占着控制台”把会话交给对应的 agent。
- **登录窗口 agent**（`--login-window`，登录窗口会话里的 LaunchAgent，root）：没人登录时的画面。
- **用户 agent**（菜单栏应用，用户会话里的 LaunchAgent）：那个用户的桌面；守护进程的套接字存在时自动成为 agent，否则像 3.0 之前那样自己监听（独立模式）。

agent 负责画面：rdaccel 的采集、编码、注入和虚拟屏，画面的 UDP 4100 / TCP 4101 由正在服务的那个 agent 自己监听（同一时刻只有一个）。守护进程和 agent 之间是本机的 Unix 套接字 `/var/run/rdmachine/agent.sock`，帧格式同控制协议（4 字节长度 + JSON），不加密；守护进程只接受代码签名和自己相同的程序（macOS 上用对方的 audit token 核对指定要求），登录窗口 agent 必须是 root，用户 agent 是那个用户。消息（`core/include/rdmachine/agent_link.h`）：

| 方向 | 消息 | 内容 |
| --- | --- | --- |
| agent → 守护进程 | `hello` | `kind`（`aqua` / `loginwindow`）、`version`（1） |
| 守护进程 → agent | `welcome` | 服务端名、指纹、端口（给菜单栏显示） |
| 守护进程 → agent | `start_session` | `id`、`request`（同 `session_request`）、`secrets`（密钥和令牌）、`origin`、可选 `login`（账号密码，见下文） |
| agent → 守护进程 | `session_started` / `session_failed` | `reply_to`；`grant`（同 `session_ready`，不含密钥）或 `error` |
| 守护进程 → agent | `end_session` | `id`；agent 停掉画面、放开端口后回 `session_ended` |
| agent → 守护进程 | `issue_pairing_code` / `pairing_code_status` | 菜单栏要配对码；守护进程回 `pairing_code`（`code`、`remaining_ms`）。只接受当前控制台用户的 agent |
| agent → 守护进程 | `disconnect` / `settings` | 菜单里的“断开当前连接”；端口和是否允许配对码改了（守护进程换端口重新监听）。同上，只接受控制台用户的 agent |

**登录规则**：有人登录时只接受那个用户的密码（`not_console_user`）；在登录窗口时接受这台 Mac 上任何账号的密码。配对码只由当前控制台用户的菜单栏发出，登录窗口没有。守护进程找不到该接手的 agent 时（登录窗口或用户会话的 agent 还没起来）`session_request` 回 `error`（`unavailable`）。

**替用户填写密码**：用账号密码登录后的第一个会话，如果在登录窗口或者用户会话正锁着屏，agent 等画面连上后轻按 Shift 唤醒显示器，1.5 秒后把密码填进去并回车（登录窗口设成“姓名和密码”时先填账号和 Tab）。ASCII 按美式键盘的物理键发，其他字符按字符发。密码只在守护进程和 agent 的内存里留到这次会话开始，填完清零，不写盘、不进日志；配对码登录不填。会话迁移到登录窗口（用户退出登录）时不填，免得刚退出又被登录回去。

## 会话迁移

控制台换了主人时，守护进程在控制连接上把会话搬过去，客户端不用重新登录：

1. 守护进程每 250 毫秒看一次控制台（`/dev/console` 的主人）和正在服务的 agent。主人变了，或者那个 agent 没了（崩溃后 launchd 重启它）：
   - 新主人是登录窗口，或者就是这次登录的账号（密码登录是用户名；配对码登录是用码时的控制台用户）：发 `session_moving`，下一轮结束旧 agent 的画面（等它放开端口），然后每秒尝试在新 agent 上开始，最多 60 秒；成功就发新的 `session_ready`，失败发 `error`。
   - 新主人是别人：发 `bye`（`console_changed`），结束会话。
2. 客户端收到 `session_moving` 后不把画面断开当成失败，等新的 `session_ready`（最多 75 秒），建新的 viewer 换上，再把窗口大小（请求桌面）、键盘语言、文字模式重新告诉服务端。新的 UDP 不通时在同一条连接上改要 TCP。
3. 画面断开和原因（`bye`、`session_moving`）可能前后脚到：客户端在画面断开后等 1.5 秒，没有收到原因才算 `media_lost`。

例子：从登录窗口连进来、输密码登录，服务端填好密码，用户会话起来后画面自动换成桌面；在远程退出登录，画面回到登录窗口（不会自动再登录）。

## 剪贴板

在一边复制的文本、富文本（RTF、HTML）、图片（PNG），可以在另一边粘贴。**内容只有在对方真正粘贴时才离开本机**：复制时只告诉对方有哪些格式，对方的应用粘贴时才来取，而且只取它要的那一种格式。在访达里复制的文件只作为文件名（文本）共享，文件本身不经过剪贴板：访达粘贴要本机真实存在的文件路径，而只要有程序读了这些路径（访达更新“粘贴”菜单时就会读），就得马上把文件传过去，做不到粘贴时才传（试过访达的粘贴不接受文件承诺）。文件以后用单独的传输窗口，明确地传。

剪贴板不走控制连接，而是走画面连接上 rdaccel 的应用消息：可靠、有序、加密，和画面分开走。控制协议因此没有新消息，版本仍是 0。

### 消息（剪贴板版本 3）

| 类型 | 方向 | 内容 |
| --- | --- | --- |
| `0x10005` hello（`RDA_MSG_USER_FIRST + 5`） | 双向，最先发 | JSON `{"version":3}` |
| `0x1000A` announce | 复制的一方 | JSON `{"id":3,"formats":["text/plain","image/png"]}`：复制了哪些格式，没有内容 |
| `0x1000B` request | 粘贴的一方 | JSON `{"announce":3,"id":1,"format":"text/plain"}`：有应用在粘贴，要这一种格式 |
| `0x10006` offer | 复制的一方 | JSON，回答一个 request：`{"id":4,"request":1,"format":"text/plain","size":12}`；给不了时 `{"id":4,"request":1,"skipped":"gone"}`（`gone`：对方已经又复制了别的；`too_large`：超过上限） |
| `0x10007` data | 复制的一方 | 二进制：`id`（8 字节）、`offset`（8），小端，后面是数据。每条最多 128 KiB |
| `0x10008` ack | 粘贴的一方 | 二进制：`id`（8）、已收到的字节数（8） |
| `0x10009` cancel | 双向 | JSON `{"id":7,"from_sender":true,"reason":"superseded"}`；`from_sender` 说明 `id` 是哪一方的编号 |
| `1` 纯文本（`RDA_MSG_CLIPBOARD_TEXT`） | 双向 | UTF-8 文本，不带结尾的 NUL。只和没说过 hello 3 的旧版本对端用 |

格式用 MIME 名：`text/plain`（UTF-8）、`text/rtf`、`text/html`（UTF-8）、`image/png`。不认识的格式不宣布；收到不认识的忽略。一种格式最大 64 MiB，两端都查。

### 规则（两端相同）

- **复制时只宣布**：各自轮询本机剪贴板（macOS 的 `NSPasteboard.changeCount`，每 250 毫秒一次），变了就发 announce。连接之前就在剪贴板里的内容不宣布，只宣布连接之后复制的。
- **粘贴时才取**：收到 announce 的一方在本机剪贴板上放一个“承诺”（macOS：`NSPasteboardItem` 的延迟提供数据，有这些格式但没有数据）。有应用粘贴时，系统向我们要数据，我们发 request，等对方的 offer 和数据到齐后交给系统。等待期间主线程的事件循环照常转（远程窗口照常刷新），没有进展 30 秒就放弃。同一份取过的内容再粘贴时不再取。只读取 TIFF 的应用从 PNG 转。
- **只收要的**：只接受回答自己 request 的 offer，不请自来的一律 cancel（`refused`）。粘贴纯文本不会把同时复制的图片也传过来。
- **不回传**：承诺被填上数据不算新的复制（changeCount 不变）；自己的承诺从不读、不宣布，所以两端不会来回弹。
- **新的覆盖旧的**：对方又复制了别的，旧承诺作废（正在等的粘贴得到空），正在传的 cancel 掉。粘贴的一方本机复制了别的，对方的承诺同样作废。会话结束时承诺都作废：断开后粘贴得到空。
- **不宣布**：空内容、密码管理器标记为 `org.nspasteboard.ConcealedType` 或 `org.nspasteboard.TransientType` 的内容。复制的是文件时只有文件名（访达还会放图标，不要）。
- **方向**：每端可以设 `both`（默认）、`send`（只从本机发给对方）、`receive`（只收对方的）或 `off`，和 NoMachine 的四档对应。不收的一方不放承诺、不取；不发的一方不宣布，request 一律回 `gone`。服务端：`rdmachine-server --clipboard=…`；命令行客户端：`rdmachine-connect --clipboard=…`；Mac 客户端暂时固定 `both`，设置项等偏好设置窗口一起做。
- **只看不控**：`view_only` 的会话（客户端请求或服务端 `--view-only`）里，服务端不接收客户端的剪贴板，只把自己的发出去。不想让对方拿到被控端剪贴板时用 `--clipboard=receive` 或 `off`。
- **旧版本**：各自连通后先发 hello。对端 1.5 秒内没说 hello 3（不说，或者说更早的版本，包括 2.0.0 和 2.1.0）就当它是旧版本：它没法在粘贴时来取，所以**服务端**一复制就把纯文本发给它（类型 1，和以前一样），**客户端**什么都不发给旧服务端。收到对端的纯文本照收。
- **命令行客户端**没有事件循环来回答系统的请求，所以收到 announce 就把所有格式取过来直接写进剪贴板（`fetch_on_announce`）。

### 节奏

rdaccel 发送时应用消息排在画面前面：一张大图一次全塞给它，画面就要等它发完，它的码率控制也会看到排队而压低画质。所以发送方自己控制速度（`core/include/rdmachine/transfer_pacer.h`），像后台流量一样：从 8 Mbps 开始，每 100 毫秒没有问题就快 25%，有问题就慢 30%（最低 256 kbps）。“有问题”是：

- 接收方的 ack 比见过的最快的慢了 25 毫秒以上（路上在排队）；
- 服务端发送时，rdaccel 因为拥塞跳过了画面帧（`rda_host_stats.frames_dropped_congestion` 变大）。

另外，没有 ack 的数据不超过“当前速度 × (最短 ack 时间 + 200 毫秒)”，至少 256 KiB。局域网上几秒内能到几百 Mbps；慢的链路上停在链路速度附近，画面优先。

实现：`core/include/rdmachine/clipboard_sync.h`（`ClipboardSync`：轮询、宣布、承诺、按请求发送和接收，平台无关，Linux 上用内存剪贴板测试；`ClipboardPromise` 是放在剪贴板上的承诺）、`clipboard_wire.h`（消息）、`transfer_pacer.h`（节奏）；macOS 剪贴板（包括延迟提供数据的 `RDMPromiseProvider`）在 `session/src/system_clipboard_mac.mm`；服务端由 `MediaHost`、客户端由 `RemoteSession` 接上 rdaccel。测试：`core/tests/test_clipboard_*.cpp`、`test_transfer_pacer.cpp`，`session/tests/test_system_clipboard_mac.mm`（用私有的 pasteboard，不碰通用剪贴板；`MacPromiseTest` 用 AppKit 读承诺，和别的应用一样），`session/tests/test_media_e2e.cpp`（两端真实连接）。Linux 暂时没有剪贴板后端（第一阶段只做 Mac）。

### 文件（剪贴板版本 4、5）

复制的是文件或文件夹时，announce 多一个 `"files"`：每一项的名字、类型（`"k"`：`f` 文件、`d` 文件夹、`l` 符号链接）、大小、权限、修改时间和扩展属性（`"x"`，名字到 base64 值，访达标签之类，每项最多 16 KB，系统自己的记账属性不带），没有内容。对方是版本 3 时不带，它只收到文件名（文本）。

版本 5（4.0.0）：announce 的 `id` 是随机的（小于 2^53，不再每个会话从 1 数），复制的一方记住最近 32 份复制，粘贴的一方按 id 来取的都答。所以正在粘贴的时候任一边又复制了别的，传输照常进行；断线重连后新会话也认得旧的 id。对方是版本 4 时，粘贴的一方把它的 id 和本会话的一个随机数混一下再用，免得和别的会话撞上（这种复制断线后不接着传）。粘贴的一方要的时候才问（`core/include/rdmachine/file_transfer.h`）：

| 类型 | 方向 | 内容 |
| --- | --- | --- |
| `0x1000C` list request | 粘贴的一方 | JSON `{"announce":3,"id":1,"item":0}`：第 0 项（文件夹）里有什么 |
| `0x1000D` list | 复制的一方 | JSON `{"request":1,"entries":[...]}`，整棵树，第一项是它自己（路径 `""`）；或 `{"request":1,"error":"gone"}` |
| `0x1000E` fetch | 粘贴的一方 | 同 list request 的字段：把第 0 项整个发过来；可以带 `"from":{"p":"sub/b.bin","o":4096,"s":9000,"t":1700000000}` 从那里接着发（见下） |
| `0x1000F` data | 复制的一方 | 二进制，同剪贴板的 data，`id` 是 fetch 的 |
| `0x10010` ack | 粘贴的一方 | 同剪贴板的 ack |
| `0x10011` cancel | 双向 | 同剪贴板的 cancel |
| `0x10012` zdata | 复制的一方 | 4.1.0：同 data，内容是一个 Zstandard 帧（帧头里记着原始大小，最多 1 MB）；只在 fetch 带 `"z":1` 时发 |

压缩（4.1.0）：粘贴的一方在 fetch 里带 `"z":1`，表示它能解 zdata。复制的一方这时把流的每一块（128 KB）用 Zstandard 1 级单独压一下，省下至少 1/32 才发 zdata，否则照常发 data；连续两块压不动，接下来 32 块（4 MB）不试，再试一块（照片、视频、压缩包几乎不花 CPU）。每块单独成帧，没有跨块的状态。头里的 offset、ack 的字节数都按**未压缩**的流算，所以接着传、进度都不受影响；节奏控制和批量通道的待发量按线上的字节算。老的复制方不认 `"z"`，照常发 data；老的粘贴方不带 `"z"`，就收不到 zdata。

data 走 rdaccel 1.10 的批量通道（`rda_*_send_bulk_message`）：排在画面后面发，用路径空出来的带宽，速度由 rdaccel 的拥塞控制决定；发送方让批量通道里保持约 1 MB 待发（`rda_*_bulk_queued_bytes`），排空了再补，未确认的最多 64 MB。拿不到这个数时（走 TCP，或对端的 rdaccel 早于 1.10、批量消息退回消息通道）用剪贴板那套节奏。ack、list、fetch、cancel 仍走消息通道；两个通道之间不保证顺序，cancel 之后晚到的 data 按未知的 id 丢掉。

一项的内容是一个流，像 tar：每条记录是 4 字节小端长度、JSON 头（`{"p":"sub/a.txt","k":"f","s":12,"m":420,"t":1700000000}`，链接带 `"l"`，扩展属性带 `"x"`），普通文件的头后面跟 `s` 字节内容（带 `"o"` 时只跟从第 `o` 字节起的 `s - o` 字节）；最后一条是 `{"end":true}`。

接着传：粘贴的一方断线时记下正在写的那一项（路径、写了多少、宣布时的大小和修改时间），暂存的文件留着；新会话再取这一项时带上 `from`。复制的一方看那个文件没变（大小和修改时间一样），就把排在它前面的普通文件都发成 `"o"` 等于大小的记录（没有内容，粘贴的一方已经有了，核对一下大小），它自己从 `o` 接着发，后面的照常；变了就从头发，不带 `o` 的记录覆盖已有的。旧的复制方不认识 `from`，从头发，结果一样对。路径相对于这一项，接收方拒绝绝对路径、`.`、`..` 和空的部分。粘贴的一方一个文件开始收就告诉扩展（`OnFileStarted`），按收到的多少回报进度，扩展据此把已到的部分先交给系统（见[文件复制粘贴](file-paste.md)的“部分内容”）。

粘贴的一方先写到 `~/Library/Caches/<应用标识>/files/<announce>/<项>/` 下，再交给 File Provider 扩展。暂存的跟着那份复制走：复制不在剪贴板上、也没有在取的，一分钟后连同扩展里的占位项一起删掉（`RemoveStagedCopy`）；会话结束时正在取的留着，等下一个会话（90 秒内）接着取。macOS 上的做法见[文件复制粘贴](file-paste.md)。

## 键盘语言同步

客户端用 Caps Lock、⌃空格或菜单切换输入源（简体拼音 ↔ ABC 等）时，服务端跟着切到对应的输入源。切换不能当按键传：macOS 只对物理键盘的 Caps Lock 切换输入源，注入的 Caps Lock 不起作用。

- 消息：rdaccel 应用消息，类型 `0x10001`（`RDA_MSG_USER_FIRST + 1`，`kInputSourceMessageType`），客户端到服务端，内容是 JSON：`{"id":"com.apple.inputmethod.SCIM.ITABC","name":"简体拼音","language":"zh-Hans","input_method":true,"ascii_capable":false}`。`id` 必填，其余可缺。
- 客户端在会话开始、系统发出“输入源已改变”通知（`kTISNotifySelectedKeyboardInputSourceChanged`）、远程窗口回到前台时发送；和上次发的一样就不重复发。菜单 View → Sync Keyboard Language 可以关掉（默认打开）。
- 服务端按 `ChooseInputSource`（`core/include/rdmachine/input_source_sync.h`）在本机已启用的输入源里挑：同一个 `id` 优先；输入法找同一语言的输入法（先精确匹配 `zh-Hans`，再看主语言 `zh`）；普通键盘布局找能直接打 ASCII 的布局（同语言优先）；都没有就不动。只看会话不跟随。当前已经是它就不重复切换。
- 登录窗口只选键盘布局，不选输入法（3.2.3 起）：登录窗口的用户列表和密码框只接收键盘布局打出的键，选了拼音之类的输入法时打的字全被输入法吞掉。客户端在用输入法时，登录窗口用 ASCII 布局（先 ABC，再 U.S.，再任何 ASCII 布局）；客户端用普通布局时照常匹配。
- 读取和切换输入源用 rdaccel 1.2 的 `rda_current_input_source`、`rda_list_input_sources`、`rda_injector_select_input_source`（macOS Text Input Sources）。服务端菜单和客户端浮窗（勾选“Input Source”时）显示同步结果。

## 会话统计

客户端远程窗口右上角的调试浮窗用服务端的数据：服务端会话期间大约每秒发一次 rdaccel 应用消息，类型 `0x10002`（`RDA_MSG_USER_FIRST + 2`，`kSessionStatsMessageType`），服务端到客户端，内容是 JSON（`core/include/rdmachine/session_stats.h`）：

```json
{"transport":"udp","width":2880,"height":1800,"encoded_fps":57.5,"captured_fps":60,
 "bitrate_kbps":3200,"target_kbps":8000,"bandwidth_kbps":120000,"rtt_ms":18.2,
 "recent_loss_pct":0.4,"total_loss_pct":1.5,"frames_dropped_congestion":3,
 "encoder":"VideoToolbox HEVC (low-latency)","input_source":"简体拼音","view_only":false,"text_mode":false,
 "audio":"on","audio_kbps":162.5}
```

速率由 `LiveStatsSampler` 从 rdaccel 的累计计数算出；`rtt_ms`、`bandwidth_kbps` 为 0、丢包为负数表示没有测量（TCP）。丢包是真正丢失的包：rdaccel 判定丢失并重传、之后又没有收到原包确认的（`packets_lost - packets_spurious`，rdaccel 1.3），只是晚到的不算。浮窗显示哪些项、背景透明度由客户端菜单 View → Statistics 决定，服务端总是发全部字段。

修订 3 加了声音的两项（旧客户端忽略）：`audio` 是声音的状态：`off`（这次会话没有声音）、`starting`、`on`（正在传）、`silent`（在传，但被控 Mac 现在没声音，一个包都不发）、`paused`（客户端暂停了）、`no_permission`（服务端没有“系统声音录制”权限）、`failed`（采集失败，rdaccel 每 2 秒再试）；`audio_kbps` 是最近一秒声音的码率，和 `bitrate_kbps` 同一个口径（也算在 `bitrate_kbps` 里）。Mac 客户端浮窗的“声音”一项（默认不显示）：`Audio: 96 kbps · buffer 32 ms · lost 0.1%`、`Audio: silent`、`Audio: paused`、`Audio: off (server setting)`、`Audio: off (server needs the System Audio Recording permission)`、`Audio: off (login window)`、`Audio: off (over TCP there is no sound)`、`Audio: off (reconnect to hear sound)`（这次连接开始时菜单里关着声音，后来打开了）、`Audio: off (older server)`（只在协商出的修订低于 3 时）；码率来自服务端，缓冲和丢失来自本机的 `rda_viewer_stats`。

## 实现

- 服务端：`ServerSession`（`core/include/rdmachine/server_session.h`）是不做 I/O 的状态机，按上面的规则处理每条消息；平台相关的部分（PAM 校验密码、启动 rdaccel Host）由 `ServerDelegate` 提供。`ControlServer` 在 TLS 上为每个连接开一个线程驱动它，负责 15 秒无流量发 `ping`、45 秒收不到消息发 `bye`（`idle_timeout`）后关闭。
- 限流、配对码、一次性令牌：`core/include/rdmachine/auth_guards.h`。账号密码和配对码的失败都计入同一个按 IP 的限流。
- 客户端：`ConnectAndStartSession`（`core/include/rdmachine/control_client.h`）走完 TLS、指纹确认、`hello`、`auth`、`session_request`，成功后把控制连接留给调用方在会话期间使用。除了上表的错误码，客户端还会报 `connect_failed`、`untrusted_server`、`timeout`、`connection_closed`、`bad_message`。
- 测试：`core/tests/test_server_session.cpp`（状态机）和 `core/tests/test_control.cpp`（本机回环端到端，包括指纹拒绝、限流、配对码、忙、坏帧、空闲超时）；剪贴板见上面的“剪贴板”一节。

## 错误码

| 错误码 | 出现在 | 含义 |
| --- | --- | --- |
| `bad_credentials` | `auth_result` | 账号密码或配对码不对 |
| `not_console_user` | `auth_result` | 账号不是当前已登录图形会话的用户（在登录窗口时任何账号都可以） |
| `code_expired` | `auth_result` | 配对码过期或已作废 |
| `rate_limited` | `auth_result` | 失败太多，看 `retry_after_ms` |
| `method_unsupported` | `auth_result` | 服务端没开这种登录方式 |
| `bad_message` | `error` | 消息格式不对 |
| `unexpected` | `error` | 消息不符合当前状态 |
| `incompatible` | `error` | 协议版本不一致 |
| `no_display` | `error` | 请求的显示器不存在 |
| `capture_denied` | `error` | 服务端没有“屏幕录制”权限，无法开始会话 |
| `busy` | `error` | 已有会话在进行（M0 一次只服务一个客户端） |
| `unavailable` | `error` | 装成服务时：该接手的 agent（登录窗口或用户会话的）还没起来，过一会儿再连 |
| `internal` | `error` | 服务端内部错误，详情看服务端日志 |

## 不做的

以前在这里列为“以后会加”的，现在的状态（见[决定](decisions.md)的“范围收口”“保持登录”“便捷优先”）：

- 已经做了：登录窗口和锁屏时的连接、会话迁移（3.0）；断线后自动重连（3.1）；多块屏的桌面（4.0，“扩展桌面”）。
- 不做：公钥登录（`enroll_key`、`auth` 的 `key` 方式，改成了记住密码）；被控端审批弹窗、双因素认证；在服务端几块真实显示器之间切换（选了扩展桌面）；隐私屏；TCP 上的声音；客户端麦克风传到被控端；换网络不断线的连接 ID（Tailscale 的地址不随网络变）。
- 不在当前范围：Linux 服务端的虚拟屏。
