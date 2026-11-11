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
| `text_mode` | 布尔 | false | 以文字模式开始会话，见下文“文字模式” |
| `desktop` | 对象 | 无 | 客户端要的桌面：`width`、`height`（像素，≥ 1）、`scale`（每点像素数，可省略，默认 1）。服务端按它建虚拟显示器并共享那块屏，这时 `display_index` 不起作用。见下文“请求桌面” |

### session_ready（服务端 → 客户端）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `transport` | 字符串 | 实际使用的 rdaccel 传输：`udp` 或 `tcp` |
| `port` | 整数 1–65535 | rdaccel 监听的端口 |
| `key` | base64，32 字节 | 本次会话的 rdaccel 密钥，每次会话重新随机生成 |
| `auth_token` | base64，32 字节 | 一次性令牌，客户端放进 rdaccel viewer 的 `auth_token`；服务端在 `on_auth` 里用常数时间比较核对，用一次就作废，30 秒内没用也作废 |
| `displays` | 数组 | 可选的显示器：`index`、`name`、`width`、`height`（像素）、`scale`（每点像素数） |
| `desktop` | 对象，可选 | 为这次请求建好的桌面（`width`、`height`、`scale`，尺寸可能被调整过）。没有这个字段：客户端没请求，或者服务端建不了，画面是服务端自己的显示器，客户端按比例缩放显示 |

客户端收到后立刻用 `rda_udp_connect(host, port)`（或 `rda_tcp_connect`）加上 `key`、`auth_token` 创建 rdaccel viewer。`key` 和 `auth_token` 只在内存里保存，用完清零。

### ping / pong（双向）

`{"type":"ping","id":<整数>}`，对方回同样 `id` 的 `pong`。控制连接空闲时每 15 秒一次，45 秒收不到任何消息就认为断开。画面连接的保活由 rdaccel 自己负责。

### bye（双向）

`{"type":"bye","reason":"<原因>"}`，发完关闭。原因：`user_closed`（用户主动断开）、`server_shutdown`、`session_replaced`（同一用户从别处连进来）、`idle_timeout`。

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

## 文字模式

省流量用（按流量计费的链路）：体验和普通模式一样，流量更少。先保证体验，再在看不出来的地方省。静止的、以及正在打字的文字和界面减到每块 16 色（不够像时 32 或 64 色；只在边缘上有误差，平滑的地方不动）、用专门的编码立刻发出，这部分省 3–7 倍。画面在动的时候（滚动、进出全屏、影片）照普通模式整体走视频，停下后把文字无损补上，滚动和拖窗省三到六成，影片基本不省。数字见 rdaccel 的 CHANGELOG 1.9.0。

- **谁决定**：客户端（它知道自己的链路要不要省）。默认关：不在乎流量时普通模式画质更好（照片、视频、动画）。
- **会话开始时**：`session_request.text_mode`（可省略，默认 false）。
- **会话中途**：rdaccel 应用消息，类型 `0x10004`（`RDA_MSG_USER_FIRST + 4`，`kPictureModeMessageType`），客户端到服务端，内容是 JSON `{"text_mode":true}`。服务端调用 `rda_host_set_text_mode`，下一帧生效；没有单独的应答。关掉时服务端整屏刷新一次（换回原色）。不认识的字段忽略，以后的画面模式加在这个对象里。
- **客户端知道当前状态**：会话统计（见下）里的 `text_mode`；Mac 客户端的浮窗打开时多显示一行 “Data saver”。
- **桌面的分辨率**：文字模式下 Mac 客户端默认请求 1 倍的桌面（“请求桌面”的 `scale` 为 1，像素数 = 窗口的点数），即使窗口在 Retina 屏上：要发的像素是 2 倍桌面的四分之一。被控端按 1 倍渲染文字，客户端在窗口正好是画面整数倍大时用最近邻放大（每个像素变成 2 × 2，不做平滑）。切换文字模式时客户端重新请求桌面（开：1 倍；关：回到窗口的缩放系数）。菜单 View → Data Saver at Standard Resolution (1x) 可以关掉，默认开。服务端不需要知道这件事。
- Mac 客户端：菜单 View → Data Saver (Sharp Text)，选择会记住，下次连接直接用。命令行客户端：`rdmachine-connect --text-mode`（开始时），`--set-text-mode=on|off`（收到画面后切换）。
- 控制协议只加了可选字段，版本仍是 0；旧服务端忽略 `text_mode` 和这条消息（画面照常，只是不省），旧客户端不发。

实现：`core/include/rdmachine/picture_mode.h`（消息）；服务端在 `MediaHost`（`StartSession`、`OnMessage`）；Mac 客户端在 `client/main.mm`（`toggleTextMode:`、`sendPictureMode`）。测试：`core/tests/test_picture_mode.cpp`、`test_messages.cpp`、`session/tests/test_media_e2e.cpp`（`TextModeIsAskedForAtTheStartAndChangedWhileRunning`）。

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

## 键盘语言同步

客户端用 Caps Lock、⌃空格或菜单切换输入源（简体拼音 ↔ ABC 等）时，服务端跟着切到对应的输入源。切换不能当按键传：macOS 只对物理键盘的 Caps Lock 切换输入源，注入的 Caps Lock 不起作用。

- 消息：rdaccel 应用消息，类型 `0x10001`（`RDA_MSG_USER_FIRST + 1`，`kInputSourceMessageType`），客户端到服务端，内容是 JSON：`{"id":"com.apple.inputmethod.SCIM.ITABC","name":"简体拼音","language":"zh-Hans","input_method":true,"ascii_capable":false}`。`id` 必填，其余可缺。
- 客户端在会话开始、系统发出“输入源已改变”通知（`kTISNotifySelectedKeyboardInputSourceChanged`）、远程窗口回到前台时发送；和上次发的一样就不重复发。菜单 View → Sync Keyboard Language 可以关掉（默认打开）。
- 服务端按 `ChooseInputSource`（`core/include/rdmachine/input_source_sync.h`）在本机已启用的输入源里挑：同一个 `id` 优先；输入法找同一语言的输入法（先精确匹配 `zh-Hans`，再看主语言 `zh`）；普通键盘布局找能直接打 ASCII 的布局（同语言优先）；都没有就不动。只看会话不跟随。当前已经是它就不重复切换。
- 读取和切换输入源用 rdaccel 1.2 的 `rda_current_input_source`、`rda_list_input_sources`、`rda_injector_select_input_source`（macOS Text Input Sources）。服务端菜单和客户端浮窗（勾选“Input Source”时）显示同步结果。

## 会话统计

客户端远程窗口右上角的调试浮窗用服务端的数据：服务端会话期间大约每秒发一次 rdaccel 应用消息，类型 `0x10002`（`RDA_MSG_USER_FIRST + 2`，`kSessionStatsMessageType`），服务端到客户端，内容是 JSON（`core/include/rdmachine/session_stats.h`）：

```json
{"transport":"udp","width":2880,"height":1800,"encoded_fps":57.5,"captured_fps":60,
 "bitrate_kbps":3200,"target_kbps":8000,"bandwidth_kbps":120000,"rtt_ms":18.2,
 "recent_loss_pct":0.4,"total_loss_pct":1.5,"frames_dropped_congestion":3,
 "encoder":"VideoToolbox HEVC (low-latency)","input_source":"简体拼音","view_only":false,"text_mode":false}
```

速率由 `LiveStatsSampler` 从 rdaccel 的累计计数算出；`rtt_ms`、`bandwidth_kbps` 为 0、丢包为负数表示没有测量（TCP）。丢包是真正丢失的包：rdaccel 判定丢失并重传、之后又没有收到原包确认的（`packets_lost - packets_spurious`，rdaccel 1.3），只是晚到的不算。浮窗显示哪些项、背景透明度由客户端菜单 View → Statistics 决定，服务端总是发全部字段。

## 实现

- 服务端：`ServerSession`（`core/include/rdmachine/server_session.h`）是不做 I/O 的状态机，按上面的规则处理每条消息；平台相关的部分（PAM 校验密码、启动 rdaccel Host）由 `ServerDelegate` 提供。`ControlServer` 在 TLS 上为每个连接开一个线程驱动它，负责 15 秒无流量发 `ping`、45 秒收不到消息发 `bye`（`idle_timeout`）后关闭。
- 限流、配对码、一次性令牌：`core/include/rdmachine/auth_guards.h`。账号密码和配对码的失败都计入同一个按 IP 的限流。
- 客户端：`ConnectAndStartSession`（`core/include/rdmachine/control_client.h`）走完 TLS、指纹确认、`hello`、`auth`、`session_request`，成功后把控制连接留给调用方在会话期间使用。除了上表的错误码，客户端还会报 `connect_failed`、`untrusted_server`、`timeout`、`connection_closed`、`bad_message`。
- 测试：`core/tests/test_server_session.cpp`（状态机）和 `core/tests/test_control.cpp`（本机回环端到端，包括指纹拒绝、限流、配对码、忙、坏帧、空闲超时）；剪贴板见上面的“剪贴板”一节。

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
- 切换显示器、单独的文件传输窗口（大文件、选目录，类似 NoMachine 的 Transfer a file）、断线后恢复同一会话：M1 起逐步加消息类型。
- 请求桌面：Linux 服务端的虚拟屏；多块屏的桌面；会话期间不让被控端的真实屏幕显示画面（隐私屏）。
