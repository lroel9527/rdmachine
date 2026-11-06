# 同类开源远程桌面方案调研

> 调研日期：2026-11-06。目的：为 rdmachine（开源的类 NoMachine 远程桌面，画面引擎依托 [rdaccel](https://github.com/lroel9527/rdaccel)）找出可借鉴的设计和差异化机会。
> 各项目细节见后面的分章节；未能核实的事实在文中标注“待核实”。

## 1. 总览对比

| 项目 | 许可证 | 协议/传输 | 画面编码 | 会话模型 | NAT 穿透/中继 | 浏览器接入 | 主要短板 |
|---|---|---|---|---|---|---|---|
| RustDesk | AGPL-3.0（服务器 Pro 闭源） | 自有协议，TCP/UDP，hbbs 信令 + 打洞，hbbr 中继，NaCl 端到端加密 | 整帧视频：软件 VP8/VP9/AV1，硬件 H.264/H.265 | 共享物理桌面为主 | 有 | 有限 | 整帧 YUV420 文字发糊；Wayland 弱；采集编码与应用耦合 |
| TigerVNC | GPL-2.0 | RFB，仅 TCP | 脏矩形 + Tight（zlib/JPEG）、CopyRect，ContinuousUpdates 流控 | Xvnc 虚拟会话；也可共享 | 无 | 经 noVNC | 无音频/文件传输；无 macOS 服务端；高分辨率视频场景吃力 |
| xrdp + FreeRDP | Apache-2.0 | RDP，静态/动态虚拟通道，GFX 帧级 ACK | GFX/RemoteFX，0.10.2 起软件 H.264（无 AVC444、无硬编） | xrdp→sesman→sesexec 虚拟会话，断线可续连 | 无 | 经 Guacamole | 不能原生共享物理桌面；编码性能有限 |
| Apache Guacamole | Apache-2.0 | 浏览器网关，Guacamole 文本协议 + 分块流 ACK | 网关解码上游后重编 PNG/JPEG/WebP | 网关代理 RDP/VNC/SSH | 依赖网关部署 | 核心能力 | 网关 CPU 重、延迟高，无视频编码 |
| X2Go（NX 3） | GPLv2+（部分 AGPLv3+） | SSH 隧道，nxagent 压缩/缓存 X11 | X11 协议层差分压缩，不走视频编码 | 虚拟会话，挂起/恢复、换机续用 | 无（靠 SSH） | 无 | 不支持 Wayland；现代合成桌面、视频、浏览器体验差 |
| Sunshine + Moonlight | GPL-3.0（待核实） | GameStream：RTSP + RTP/UDP，逐帧 Reed-Solomon FEC，ENet 控制 | GPU 零拷贝采集 + NVENC/VAAPI/VideoToolbox，H.264/HEVC/AV1，HDR | 共享物理桌面/游戏 | 无 | 无 | 偏游戏：无自适应码率、NAT 穿透、文件传输、多显示器 |

（参考背景：NoMachine 从 4.x 起 NX 协议闭源，转向视频编码 + 自有传输。）

## 2. 各方案最值得借鉴的设计

**会话层（归 rdmachine 应用层）**
- 会话与连接分离：会话常驻、连接可断，支持挂起/恢复和换机续用（X2Go、xrdp sesman）。这是 NoMachine 体验的核心，也是开源方案里少有人做好的。
- 进程分层：前端接入 → 认证（PAM）→ 每会话独立进程（xrdp 的 sesman/sesexec 模型）。
- 虚拟会话与共享物理桌面两种模式都要有（TigerVNC Xvnc / xrdp 只做前者，RustDesk / Sunshine 只做后者）。

**连接与安全（归应用层）**
- “信令 + 打洞 + 中继兜底 + 端到端加密”（RustDesk hbbs/hbbr）。自建服务器要开源完整，避免 RustDesk Pro 收费引发的争议。
- 配对体验参考 Sunshine 的 PIN 确认，但用 SPAKE2 / Noise 等标准方案替代自制的 PIN 派生密钥。

**扩展机制（协议设计）**
- RDP 式通道：静态通道 + 可按名字动态开关的通道，剪贴板、文件、音频、显示布局各走独立通道；FreeRDP “每个通道一个插件”的结构可直接参考。
- 显示布局调整（分辨率、多屏热插拔）走单独的控制通道（RDP DisplayControl）。

**媒体层（rdaccel 与传输）**
- 零拷贝流水线：GPU 采集 → 硬件编码 → 发送（Sunshine）。
- 丢包恢复分层：FEC → NACK 重传 → 参考帧失效（RFI）→ 关键帧兜底（Sunshine 有 FEC + RFI，缺 NACK 和码率自适应）。
- 流控：帧级 ACK（RDP GFX）、ContinuousUpdates（RFB）、sync 帧 ACK 测 RTT（Guacamole），即基于 RTT 和确认的背压。
- 脏区 + CopyRect + 内容自适应（文字区无损、视频区有损），避免 RustDesk 整帧 YUV420 导致的文字发糊。

**浏览器与运维**
- 浏览器端直接解码码流（WebCodecs / WASM），网关只做中继和鉴权；Guacamole 那种网关转码仅作降级方案。
- 分享链接、在协议流层录制会话、可插拔认证、可单独嵌入的 JS SDK（Guacamole）。

## 3. rdmachine 的差异化机会

1. **办公场景的画质**：文字锐利（脏区、无损文字区、YUV444 或分区编码）与视频流畅兼得。RustDesk 和 Sunshine 是整帧视频编码，VNC 和 X2Go 遇到视频就吃力，目前没有开源方案两头都做好。
2. **完整的会话保持**：虚拟显示器 + 挂起/恢复 + 换机续用，同时支持 Wayland（PipeWire / 虚拟输出）和 macOS。X2Go 卡在 X11 上，xrdp 不能共享物理桌面，RustDesk 没有虚拟会话。
3. **全开源的自建服务器**：信令、中继、账号和管理后台都开源，不分社区版和 Pro 版。
4. **现代弱网传输**：在 UDP 上做 FEC + NACK + 码率自适应 + RTT 感知流控。这一块 Sunshine 只做了一半，RustDesk 和 VNC 基本没有。
5. **可复用的引擎**：rdaccel 是独立的 C ABI 库，第三方也能嵌入使用，不像 RustDesk 那样采集编码和应用绑在一起。

**需要注意的缺口**：rdaccel 不做音频、多观看者、Windows、H.264/AV1。音频、文件传输、剪贴板要由 rdmachine 的应用层通道补上。浏览器直接解码依赖 WebCodecs 支持的编码，需要确认 rdaccel 的码流能否在浏览器里解码，否则浏览器接入只能走网关转码。

---

# 分项目详述

## RustDesk

### 定位与许可证
- 定位：开源的 TeamViewer/AnyDesk 替代品，主打"开箱即用 + 可自建服务器"。面向远程支持（运维/客服）和日常远程办公，支持 Windows/macOS/Linux/Android/iOS/Web。
- 许可证：客户端 **AGPL-3.0**（[GitHub](https://github.com/rustdesk/rustdesk)）。自建服务器 `rustdesk-server`（hbbs/hbbr）开源版也是 AGPL-3.0；另有闭源的商业版 **RustDesk Server Pro**，提供 Web 控制台、账号/地址簿、策略、OIDC 等（[自建文档](https://rustdesk.com/docs/en/self-host/)）。这是一种 open-core 模式。
- 技术栈：核心用 Rust，UI 用 Flutter（旧的 Sciter UI 已弃用）。

### 整体架构
仓库结构（[README](https://github.com/rustdesk/rustdesk)）：
- `libs/hbb_common`：视频编解码、配置、TCP/UDP 封装、protobuf 和公共工具，与服务端共享。
- `libs/scrap`：屏幕采集，也承载编码器封装（vpx/aom/hwcodec 等）。
- `libs/enigo`：各平台的键鼠注入。
- `libs/clipboard`：跨平台的文件复制粘贴（Win/Linux/macOS）。
- `src/server`：被控端的各项服务，包括 audio、clipboard、input、video 和网络连接。
- `src/client.rs`：主控端发起连接。
- `src/rendezvous_mediator.rs`：与 rendezvous 服务器通信，等待打洞直连或中继连接。
- `src/platform`：平台相关代码，如 Windows 服务、macOS 权限、Linux 会话切换。
- `flutter/`：桌面与移动端 UI。

进程模型：同一个二进制既能当主控端，也能当被控端，没有独立的"服务端程序"。
- Windows 上以系统服务形式常驻，服务再拉起用户会话里的进程，以便采集登录界面/UAC 和注入输入。
- Linux 上有 `rustdesk --service`（root），负责按会话启动 server 子进程。
- macOS 依赖"屏幕录制"和"辅助功能"授权。
- UI 进程与后台服务之间走本地 IPC。（多进程细节：待核实）

服务端组件（[自建文档](https://rustdesk.com/docs/en/self-host/)）：
- **hbbs**：ID/rendezvous/信令服务器。负责注册在线状态、交换双方地址、协调 NAT 打洞。
- **hbbr**：中继服务器，打洞失败时转发流量。

### 传输协议与连接方式
- 端口：
  - TCP 21115：NAT 类型探测。
  - TCP/UDP 21116：注册、心跳、打洞。
  - TCP 21117：中继。
  - TCP 21118/21119：WebSocket（供 Web 客户端用）。
  - TCP 21114：仅 Pro 版的 HTTP API。

  21115–21117 是最低要求。如果只开放 443，那么只能走 WSS 中继，没有 P2P（[自建文档](https://rustdesk.com/docs/en/self-host/)）。
- 连接流程：
  1. 客户端向 hbbs 发心跳，上报公网地址。
  2. A 请求连接 B 时，hbbs 通知双方并尝试打洞直连。
  3. 打洞失败则双方都连到 hbbr，由它中继。

  官方称多数情况打洞能成功。
- 其他连接方式：
  - 支持 IP 直连（局域网/VPN 内，不经 hbbs，默认端口 21118，待核实）。
  - 支持 IPv6 P2P 和 UDP 打洞（1.4.x 起有报道，[ubuntuhandbook](https://ubuntuhandbook.org/index.php/2025/10/rustdesk-released-1-4-3-with-multi-monitor-for-wayland-virtual-mouse/)）。
  - 早期的数据通道主要是 TCP 打洞，UDP 数据通道的可靠性方案（KCP 之类）：待核实。
- 加密：
  - 基于 libsodium/NaCl（sodiumoxide）实现端到端加密。
  - hbbs 持有 Ed25519 密钥对，客户端配置它的公钥（`id_ed25519.pub`），用来校验对端公钥的签名、防中间人。
  - 会话密钥用 Curve25519 box 交换，数据流用 secretbox 对称加密。

  中继只转发密文。（具体算法细节：待核实，见 `hbb_common` 与 `src/client.rs`）
- 消息格式：protobuf（`message.proto`、`rendezvous.proto`），所有业务消息都复用同一条连接。

### 画面采集
（`libs/scrap`，部分实现细节待核实）
- **Windows**：DXGI Desktop Duplication 为主，GDI 兜底。
- **macOS**：Quartz `CGDisplayStream`。是否已迁移到 ScreenCaptureKit：待核实。
- **Linux X11**：XShm/XCB 截屏。
- **Linux Wayland**：通过 xdg-desktop-portal + PipeWire 采集，需要用户授权弹窗。1.4.3 起支持 Wayland 多显示器（[ubuntuhandbook](https://ubuntuhandbook.org/index.php/2025/10/rustdesk-released-1-4-3-with-multi-monitor-for-wayland-virtual-mouse/)）。Wayland 下无人值守和登录界面的支持仍然有限。
- **Android**：MediaProjection。

### 编码
- 编解码器：
  - 软件编码：VP8、VP9（libvpx）、AV1（libaom）。
  - 硬件编码：H.264、H.265，经 `hwcodec`（基于 FFmpeg：NVENC/AMF/QSV/VAAPI/VideoToolbox/MediaCodec）。
  - Windows 上另有 "vram" 零拷贝路径，即 D3D 纹理直接进硬件编码器（待核实）。

  来源：[ubuntuhandbook](https://ubuntuhandbook.org/index.php/2025/10/rustdesk-released-1-4-3-with-multi-monitor-for-wayland-virtual-mouse/)。
- 编解码器协商：被控端根据双方能力选择，用户也可手动指定或选"自动"。
- 画质与码率：提供"画质优先/均衡/速度优先"和自定义 FPS/画质，带自适应码率（基于延迟反馈调节，待核实）。
- 增量策略：整帧视频编码，依靠视频编码器的帧间预测做增量，**不做 RFB 式的脏矩形分块编码**。画面不变时跳帧，或通过比较采集结果来省带宽。
- 色彩：默认 YUV420。较新版本支持 I444 真彩模式，用于文字清晰度（待核实）。

### 功能
- 键鼠：
  - 支持 map/translate/legacy 三种键盘模式，解决跨布局问题。
  - 支持远程光标和本地光标。
  - 移动端有虚拟鼠标和摇杆（1.4.3）。
- 剪贴板：支持文本和图片；在 Win/Linux/macOS 上支持文件复制粘贴（`libs/clipboard`）。
- 音频：支持被控端声音回传（Windows 用 WASAPI loopback，Linux 用 PulseAudio，编码为 Opus，待核实）。
- 文件传输：有独立的文件管理器会话，支持断点续传/目录传输。
- 多显示器：支持切换、合并显示、每屏独立窗口（1.4.3 起 Wayland 下也支持）。
- 其他：TCP 端口转发/隧道、远程重启、隐私模式（被控端黑屏）、聊天、会话录制、无人值守永久密码 + 一次性密码、2FA、Web 客户端（Pro）、多人同时连接同一被控端。

### 性能与延迟特点
- 局域网/直连时延迟低，体验接近商业产品。
- 走公共中继时受服务器带宽限制，明显卡顿。官方公共服务器负载高，社区普遍建议自建 hbbr。
- 软件 VP9/AV1 在高分辨率下 CPU 占用高。硬件 H.264/H.265 能显著降低占用，但驱动兼容问题多。
- 整帧视频编码对文字/IDE 场景会有糊字和色度损失，所以后来提供了 I444 选项。

### 优点
- 生态完整：P2P + 中继 + 自建服务器 + 全平台客户端，开箱即用，社区庞大（GitHub star 数为 10 万级，待核实精确值）。
- Rust 实现，安全性和跨平台性好；protobuf 协议清晰，可二次开发。
- 编解码器选择丰富，软硬件都有，并且能自动协商。
- 自建服务器部署非常简单，hbbs/hbbr 两个二进制或 Docker 即可。

### 缺点与社区常见痛点
- **Linux Wayland 支持弱**：需要 portal 弹窗授权，登录界面/锁屏无法远控，多数发行版的无人值守体验差（长期 issue）。
- open-core 带来争议：账号、地址簿、集中管理等功能在 Pro 版里，开源版 hbbs 功能有限。
- 公共服务器拥堵；曾因被诈骗分子滥用而限制部分地区，产生过信任争议（待核实具体时间线）。
- 单体代码库耦合重：采集、编码、传输、UI 交织，`libs/scrap` 难以单独复用；Flutter 与 Rust 之间的桥接复杂。
- 硬件编码在部分驱动上花屏或崩溃；macOS 权限引导繁琐。
- 只有"远程控制现有桌面"，没有 NoMachine 那样的独立虚拟会话（Linux 无头场景需要 Xvfb/虚拟显示器等变通）。

### 对 rdmachine 的可借鉴点
1. **连接层**：hbbs（信令/打洞）+ hbbr（中继）的二分架构简单有效。rdmachine 应用层可参照它做"信令 + 打洞 + 中继兜底"，并让中继只转发端到端密文。
2. **信任模型**：用"服务器公钥签名对端身份 + 端到端会话密钥"防中间人，比单纯靠 TLS 证书更适合 P2P 场景。
3. **协议**：protobuf 单连接多路复用（视频/输入/剪贴板/文件），值得参考；但 rdaccel 应把画面通道独立出来，避免文件传输阻塞视频（队头阻塞）。
4. **反面经验**：RustDesk 的采集/编码和应用耦合在一起。rdaccel 以 C ABI 独立成库，正好补上"可复用画面引擎"的空白。
5. **Wayland 是痛点**：rdaccel 如果能在 Linux 上提供 PipeWire 采集 + 虚拟显示/无头会话（类 NoMachine），就是明显的差异化点。
6. **文字清晰度**：RustDesk 整帧 YUV420 视频编码对文字不友好。rdaccel 不做 H.264/AV1，可以突出"脏区 + 无损/近无损文字"路线，这正是 RustDesk 的短板。

---

## TigerVNC

### 定位与许可证
- 定位：高性能、跨平台的 VNC（RFB 协议）实现，源自 TightVNC/RealVNC 4 代码。核心场景是 **Linux 上的虚拟桌面服务器（Xvnc）**，被 RHEL/Fedora 等发行版作为默认 VNC 方案，常用于服务器远程图形桌面、HPC 和瘦客户端。
- 许可证：**GPL-2.0**（[GitHub](https://github.com/TigerVNC/tigervnc)）。
- 语言：C++，构建系统为 CMake。

### 整体架构
组件（[GitHub README](https://github.com/TigerVNC/tigervnc)）：
- **Xvnc**：带虚拟帧缓冲的 X server，同时也是 VNC server，即"无头虚拟会话"。通常用 `vncserver@.service`（systemd）或 `vncsession` 启动，每个用户/显示号一个进程。
- **x0vncserver**：共享已有的物理 X 显示。实现方式是轮询，效率低，官方称它主要用于演示。1.16.1/1.16.2 修复过一个"其他用户可观察/操控屏幕"的安全问题（[Releases](https://github.com/TigerVNC/tigervnc/releases)）。
- **w0vncserver**：1.16（2026-01-27）新增，用于共享 Wayland 桌面（[linuxiac](https://linuxiac.com/tigervnc-1-16-released-with-wayland-desktop-sharing-support/)）。推测基于 xdg-desktop-portal RemoteDesktop/ScreenCast + PipeWire（待核实）。
- **vncviewer**：基于 FLTK 的跨平台原生客户端（Win/macOS/Linux）。另有 Java viewer。
- **winvnc**：Windows 服务端，已无人维护，可能不可用。
- **工具**：vncpasswd、vncconfig。
- 代码结构：
  - `common/rfb`：协议核心，包括编解码器和安全类型。
  - `common/network`、`common/rdr`：IO 流。
  - `unix/xserver`：Xvnc 补丁，挂在 Xorg 源码树上编译。
  - `vncviewer/`、`win/`、`java/`。
- 进程模型：典型的 C/S，服务端监听 TCP 5900+N，客户端直连。Xvnc 本身就是 X server，可以直接拿到损伤区域，不需要截屏。

### 传输协议与连接方式
- 协议：标准 RFB 3.8 及 TigerVNC 扩展。只走 **TCP**，不支持 UDP、P2P/打洞、中继或信令服务器。
- 跨网访问的常见做法：SSH 隧道（viewer 内置 `-via` 选项）、VPN，或反向连接（服务端主动连 `vncviewer -listen`）。
- 加密/认证（Security Types）：
  - `None`、`VncAuth`（DES 质询，弱）。
  - `Plain`（用户名密码，配合 PAM）。
  - `TLSNone/TLSVnc/TLSPlain`（匿名 TLS）、`X509None/X509Vnc/X509Plain`（证书 TLS），基于 GnuTLS。
  - **RA2/RA2ne/RA2_256**：RSA-AES，兼容 RealVNC。
  - 1.14 起原生 viewer 还支持 Apple DH 和 UltraVNC MSLogonII；Java viewer 支持 RSA-AES（[Releases](https://github.com/TigerVNC/tigervnc/releases)）。

### 画面采集
- **Xvnc**：自身是 X server，渲染发生在自己的帧缓冲里，通过 X 的 damage 机制精确获得变化区域，零截屏开销。这是它性能好的根本原因。1.14 起支持经 GBM 的硬件加速 OpenGL/Vulkan（DRI3）（[Releases](https://github.com/TigerVNC/tigervnc/releases)）。
- **x0vncserver**：用 XShm 轮询和比较现有 X 显示。支持 XDamage 时用 damage 提示，否则逐块比较，CPU 占用较高。
- **w0vncserver**：Wayland 采集（PipeWire，待核实）。
- **Windows (winvnc)**：钩子/轮询，已不维护。
- **macOS**：**没有服务端**，只有 viewer。

### 编码
- 编码类型：
  - Raw、CopyRect、RRE、Hextile、ZRLE。
  - **Tight**：zlib + JPEG，使用 libjpeg-turbo 加速，是 TigerVNC 的主力编码。
  - 1.13 前后 viewer 新增 **H.264 解码**（依赖 FFmpeg，[Arch FS#78713](https://bugs.archlinux.org/task/78713)；1.14.1 修复过 H.264 崩溃）。服务端是否提供 H.264 编码：待核实，一般认为 TigerVNC 服务端不编码 H.264，主要用于连接 RealVNC 等服务端。
- 软件编码，无 GPU 编码。
- 增量机制：基于**脏矩形**。服务端只发送变化区域，按区域内容自适应选编码，例如色彩少的区域走无损调色板/zlib，照片类区域走 JPEG。窗口移动用 CopyRect。
- 质量控制：自动选择（AutoSelect）会根据实测带宽在 Tight+JPEG 和无损之间切换；也可手动设置压缩级别和 JPEG 质量。
- 流控：客户端拉取模式（FramebufferUpdateRequest）。TigerVNC 扩展了 ContinuousUpdates 和 Fence 消息，实现"服务端推送 + 基于 RTT 的拥塞控制"，显著改善高延迟链路的体验。
- 多线程编码：服务端把一帧切成多个矩形并行编码。

### 功能
- 键鼠：
  - 支持 QEMU Extended Key Event，按扫描码传输，减少键盘布局问题。
  - 1.15 起支持后退/前进鼠标键，服务端能处理布局中缺失的键。
  - 1.16 用可配置快捷键体系取代 F8 菜单；Windows/macOS 下可捕获几乎所有系统键（[linuxiac](https://linuxiac.com/tigervnc-1-16-released-with-wayland-desktop-sharing-support/)）。
- 剪贴板：
  - 支持 Extended Clipboard 扩展（UTF-8、较大文本）。
  - 只支持**文本**，不支持图片和文件。
  - x0vncserver 从 1.15 起支持剪贴板。
- 音频：**不支持**（RFB 无标准音频，需要配合 PulseAudio 网络转发等外部方案）。
- 文件传输：**不支持**（TightVNC/UltraVNC 有私有扩展，TigerVNC 没有实现）。
- 多显示器：通过 ExtendedDesktopSize 支持服务端多屏布局，客户端可全屏跨多个显示器。支持客户端调整远端分辨率（resize）。
- 共享：多个 viewer 可同时连接同一会话（shared/exclusive 模式）。

### 性能与延迟特点
- 局域网下 Xvnc + Tight/JPEG 非常快，2D 办公和终端场景延迟低，CPU 占用小，是"VNC 中最快的之一"。
- 文字区域走无损编码，所以清晰；但视频、3D、大面积动画场景下 JPEG 块效应明显，带宽暴涨。没有帧间预测，同等画质下带宽远高于 H.264 类方案。
- 高延迟/广域网下靠 ContinuousUpdates + 拥塞控制缓解，但仍不如基于 UDP 的商业方案（NoMachine NX/Parsec）。
- x0vncserver 共享物理桌面的效率明显差于 Xvnc。

### 优点
- 稳定、成熟、标准兼容（RFB），与大量第三方客户端/服务端互通。
- Xvnc 虚拟会话模型天然适合 Linux 多用户无头服务器，是 NoMachine "虚拟桌面"功能的开源对应物。
- 脏矩形 + 内容自适应编码对文字/UI 友好、CPU 占用低。
- 安全类型丰富（TLS/X509/RSA-AES），可通过 PAM 集成系统账户。

### 缺点与社区常见痛点
- 没有 NAT 穿透、中继和 ID 体系，跨网必须自己搭 SSH/VPN，对普通用户门槛高。
- 不支持音频、文件传输、图片剪贴板，功能明显少于 RustDesk/NoMachine。
- 视频类内容带宽大、画质差；没有服务端硬件编码。
- 没有 macOS 服务端；Windows 服务端已废弃；Wayland 服务端（w0vncserver）刚起步。
- Xvnc 的 GPU 加速依赖 GBM/DRI3，NVIDIA 专有驱动、旧驱动下 3D 应用（浏览器、IDE）可能慢或出问题。
- 默认 VncAuth（8 位 DES 口令）不安全，配置 TLS/证书繁琐。

### RFB 生态顺带一提
- **libvncserver/libvncclient**（[LibVNC](https://github.com/LibVNC/libvncserver)，GPL-2.0）：C 语言的 RFB 服务端/客户端库，可嵌入，被 x11vnc、QEMU 周边、wayvnc 的前身等大量项目使用。它的 API 形态（向库提供帧缓冲 + 标记脏区，由库负责编码和传输）可以直接对照 rdaccel 的 C ABI 设计。
- **x11vnc**：共享已有 X 显示的经典服务端，基于 libvncserver，用 XDamage 加轮询，功能丰富但多年少有维护（待核实当前维护状态）。
- **wayvnc / neatvnc**：wlroots 系 Wayland 合成器的 VNC 服务端。neatvnc 支持 H.264（经 VAAPI/V4L2 的 Open H.264 编码，待核实）。
- **noVNC**（MPL-2.0）：纯 HTML5/JS 的 VNC 客户端，配合 websockify（TCP↔WebSocket 代理）实现浏览器访问，是很多云平台（OpenStack、Proxmox）控制台的事实标准。
- 协议标准：RFB 3.8 见 [RFC 6143](https://www.rfc-editor.org/rfc/rfc6143)；社区扩展见 [rfbproto](https://github.com/rfbproto/rfbproto)。

### 对 rdmachine 的可借鉴点
1. **虚拟会话模型**：Xvnc 的"自己就是显示服务器"思路可以零拷贝、精确拿到 damage。rdmachine 在 Linux 上做无头/虚拟会话时，可以考虑 Xvnc 式的虚拟 X server，或 Wayland headless 合成器 + damage，这比截屏轮询高效得多。
2. **脏区 + 内容自适应编码**：TigerVNC 证明了"按矩形选无损/有损编码"对文字/UI 场景的高效。rdaccel 不做 H.264/AV1，可以在这条路线上走得更深，例如更好的无损压缩（zstd/QOI 类）、SIMD 加速、多线程分块、对静止区域做渐进式无损精修。
3. **流控**：ContinuousUpdates + Fence 的"推送 + RTT 感知拥塞控制"是从客户端拉取模式到低延迟推送的成熟改造，rdaccel 的传输层可以借鉴（帧率随带宽和 RTT 自适应，丢弃过期帧而不是排队）。
4. **CopyRect/滚动检测**：窗口移动和滚动用 CopyRect 一类的位移指令代替重编码，收益很大。
5. **兼容性选项**：可以考虑让 rdmachine 应用层或 rdaccel 提供一个可选的 RFB 网关，以便接入 noVNC/浏览器等现有生态（非核心）。
6. **反面经验**：TigerVNC 缺少 NAT 穿透、音频、文件传输，安全默认值偏弱。rdmachine 应用层应默认端到端加密，并内建信令/中继，避免重蹈"只能配 SSH 隧道"的门槛。

## xrdp / FreeRDP（RDP 生态）

### 定位与许可证
- **xrdp**：Linux/BSD 上的开源 RDP **服务端**，主要给每个用户启动**独立的虚拟 X 会话**（不是镜像物理屏幕），客户端可以是 Windows 自带的 mstsc、Microsoft Remote Desktop（macOS/iOS/Android）或 FreeRDP。许可证 Apache-2.0。源码：https://github.com/neutrinolabs/xrdp
- **FreeRDP**：RDP 协议的完整开源实现，同时提供客户端（`xfreerdp`、`wlfreerdp`、`sdl-freerdp`）、服务端库（`libfreerdp-server`）、shadow server（`freerdp-shadow-cli`，镜像现有桌面）和 `freerdp-proxy`。许可证 Apache-2.0。GNOME Remote Desktop、Weston RDP 后端、Guacamole 都以它为协议栈。3.x 是当前主线（3.0 于 2023 年发布）。源码：https://github.com/FreeRDP/FreeRDP
- **GNOME Remote Desktop（g-r-d）**：GNOME 的官方远程桌面服务，许可证 GPL-2.0+。它基于 FreeRDP 服务端库，画面来自 Mutter 的 ScreenCast/RemoteDesktop D-Bus API（PipeWire）。有两种模式：①共享当前登录的桌面；②**headless/远程登录**，即经 GDM 新建会话（GNOME 46 起，待核实具体版本）。https://gitlab.gnome.org/GNOME/gnome-remote-desktop
- **Weston RDP 后端**：Weston 合成器自带的 RDP 输出后端（MIT），基于 FreeRDP。微软的 WSLg 用它加 RAIL（RemoteApp）把 Linux GUI 窗口逐个投到 Windows 上。https://wayland.pages.freedesktop.org/weston/toc/running-weston.html 、https://github.com/microsoft/wslg

### xrdp 架构与会话模型
```
mstsc/FreeRDP ──TLS/NLA──> xrdp (监听 3389, 协议解析/编码, 每连接 fork)
                              │  Unix socket (libipm)
                              ▼
                         xrdp-sesman (认证 PAM, 会话表) ──fork──> xrdp-sesexec (每会话)
                                                                    ├─ Xorg + xorgxrdp 驱动（或 Xvnc）
                                                                    ├─ 窗口管理器/桌面 (startwm.sh)
                                                                    └─ xrdp-chansrv (剪贴板/驱动器/音频通道)
```
- **xrdp**：协议前端，负责 TLS/NLA、能力协商、位图/GFX 编码、登录界面。它通过"模块"连接后端：`libxup`（对接 xorgxrdp）、`libvnc`（对接任意 VNC 服务器，例如共享物理桌面）、`libxrdpneutrinordp`（RDP 代理）。
- **xrdp-sesman / xrdp-sesexec**：会话管理器。v0.10 起只走 Unix 域套接字（V2 协议，libipm）。每个会话由独立的 `xrdp-sesexec` 进程拉起，`--reload` 可以在线重载配置。https://github.com/neutrinolabs/xrdp/releases/tag/v0.10.0
- **xorgxrdp**：Xorg 的 video/input 驱动模块。Xorg 自己运行在内存帧缓冲上，xorgxrdp 收集脏区，经共享内存交给 xrdp 编码（有 SIMD 颜色转换，GPU 加速是可选/实验性的，待核实）。旧的 X11rdp 已在 0.10 移除。https://github.com/neutrinolabs/xorgxrdp
- **会话持久化**：断开连接后 X 会话继续运行。同一用户再次登录时按策略（如 `Policy=Default` 按用户+分辨率+位深匹配）重连原会话。这是 NoMachine 式"虚拟桌面 + 断线续连"的经典做法。
- **共享物理桌面**：xrdp 不原生支持，常见做法是用 x11vnc 加 `libvnc` 模块转接。0.10 增加了实验性的 Ulalaca，用于访问 macOS 的原生屏幕。

### 协议要点（RDP）
- **传输/安全**：基于 TCP 3389，外层 TLS，认证用 NLA/CredSSP（Kerberos/NTLM）。可选 RDP-UDP（MS-RDPEUDP，FEC + 可靠/有损两种模式；FreeRDP 的服务端支持有限，待核实）。RD Gateway 用 HTTP/WebSocket 隧道穿透防火墙。
- **图形演进**：最早是 Bitmap Updates/Orders（GDI 绘制指令、Interleaved/Planar RLE 位图），然后是 **RemoteFX**（MS-RDPRFX：64×64 tile、DWT + RLGR 熵编码，CPU 友好），最后是 **Graphics Pipeline Extension（GFX, MS-RDPEGFX）**。GFX 以 surface 为单位，codec 可选 Uncompressed/Planar/ClearCodec/RemoteFX Progressive/Alpha，以及 **AVC420**（H.264 4:2:0 + 区域元数据）和 **AVC444**（两路 4:2:0 流拼成 4:4:4，文本更清晰）。帧通过 `StartFrame/EndFrame + FrameAcknowledge` 实现**基于 ACK 的流控**。规范：https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/
- **通道机制（RDP 最值得借鉴的设计）**：
  - **静态虚拟通道（SVC）**：连接时协商，最多 31 个，名字 8 字节，例如 `cliprdr`（剪贴板）、`rdpsnd`（音频）、`rdpdr`（设备重定向）、`drdynvc`（承载 DVC）。
  - **动态虚拟通道（DVC，MS-RDPEDYC）**：在 `drdynvc` 上按名字随时创建/关闭，数量不限，支持分片和压缩。GFX、显示器布局（`Microsoft::Windows::RDS::DisplayControl`，MS-RDPEDISP）、触控（rdpei）、麦克风（audin）、USB（urbdrc）、视频重定向等都走 DVC。第三方也可以注册自定义 DVC 做插件。
  - FreeRDP 把每个通道实现为可加载的 client/server channel 插件（源码目录 `channels/<name>/{client,server}`），是插件化架构的现成参考。https://github.com/FreeRDP/FreeRDP/tree/master/channels

### 画面采集与编码
- **xrdp**：xorgxrdp 提供脏区 → xrdp 在 0.9 时代用 RemoteFX/位图。**v0.10.0 引入 GFX**（支持多显示器和热插拔，官方称帧率更高、带宽更低），**v0.10.2 新增 H.264**。编码器只有软件 x264/OpenH264，配置在 `gfx.toml`。AVC444 和硬件编码当时不支持。https://github.com/neutrinolabs/xrdp/wiki/H.264-encoding
- **FreeRDP**：编解码覆盖面最全，包括 Interleaved、Planar、NSCodec、RemoteFX、Progressive、ClearCodec，以及 H.264 AVC420/AVC444。H.264 后端可选 OpenH264、FFmpeg（含 VA-API）、MediaFoundation 等；shadow server 侧也能编码。
- **g-r-d**：从 PipeWire 拿 dmabuf/内存帧和 damage 信息，GFX 支持 RFX Progressive，以及基于 **NVENC（CUDA）** 和 **VA-API** 的 H.264 AVC420/AVC444。VA-API 在较新版本才加入（GNOME 47 左右，待核实）。

### 输入/剪贴板/音频/重定向/多显示器
- **输入**：扫描码 + Unicode 事件，外加鼠标、相对鼠标（MS-RDPBCGR 扩展）、触控笔和触摸（rdpei）。xrdp 有键盘布局映射表，非美式布局是长期痛点。
- **剪贴板**：`cliprdr` 支持文本、图片和文件列表（以 FileContents 流式传输）。xrdp 的 chansrv 把它桥接到 X selection。
- **音频**：`rdpsnd` 负责下行（PCM/AAC/Opus 等），`audin` 负责上行。xrdp 需要单独的 `pulseaudio-module-xrdp`（或 pipewire-module-xrdp）。
- **文件/设备**：`rdpdr` 支持驱动器、打印机、智能卡、串口。xrdp 把客户端驱动器经 FUSE 挂到 `~/thinclient_drives`。USB 走 urbdrc（FreeRDP 客户端支持）。
- **多显示器**：客户端在连接时上报显示器布局，运行中用 DisplayControl DVC 动态改分辨率/布局。xrdp 0.10 起支持动态调整和多屏热插拔。
- **RemoteApp（RAIL）**：单窗口无缝投射，WSLg 的核心机制。

### 性能与延迟
- 在局域网下用 GFX + H.264 观感接近本地。RemoteFX/位图在高动态内容下带宽大。xrdp 只有软件 x264，高分辨率视频场景 CPU 吃紧。
- 流控靠 GFX 的 FrameAcknowledge 和客户端上报的队列深度，没有 WebRTC 式的拥塞控制（GCC）。基于 TCP 的连接在丢包链路上会出现队头阻塞，微软官方栈靠 UDP 传输缓解，开源实现在这方面较弱（待核实）。

### 优点
- 客户端生态极广：Windows/macOS/iOS/Android 都有原生客户端，企业接受度高。
- 通道/扩展机制成熟（SVC/DVC），剪贴板、音频、设备重定向、多显示器、RemoteApp 都有标准化规范。
- xrdp 的"每用户虚拟 X 会话 + 断线续连 + PAM"模型成熟稳定。

### 缺点 / 常见痛点
- 协议庞大复杂（数十份 MS-RDP* 规范），实现和维护成本高，历史 CVE 多。
- xrdp 的虚拟会话与物理桌面割裂：同一用户已在本地登录时，常见"黑屏/会话冲突"（D-Bus/systemd --user 冲突）。Wayland 支持基本依赖 g-r-d。
- 声音和 Wayland 需要额外模块，配置分散（xrdp.ini/sesman.ini/gfx.toml/startwm.sh）。
- xrdp 没有硬件编码，也没有 AVC444（截至 0.10.2）。共享物理桌面体验差。NAT 穿透只能靠 RD Gateway 或 VPN。

### 对 rdmachine 的可借鉴点
1. **会话管理分层**：照搬 xrdp 的"前端协议进程 / 特权会话管理器（PAM 认证）/ 每会话执行器"三层，用 Unix socket + 版本化 IPC。会话匹配策略（用户+分辨率）和断线续连是 NoMachine 体验的核心。rdmachine 的 Linux 虚拟会话可以参考 xorgxrdp（Xorg 驱动拿脏区和共享内存帧）或 headless Wayland 合成器（g-r-d/Weston 方式）。
2. **通道/扩展机制**：在 rdaccel 画面流之外，应用层设计类似 DVC 的"按名字动态打开的多路复用通道"，带版本/能力协商和分片/背压。剪贴板、文件传输、显示布局控制、后续音频都作为通道插件挂载，避免把功能写死在主协议里。
3. **显示控制作为独立通道**：仿照 MS-RDPEDISP，客户端随时上报窗口尺寸/多屏布局/DPI，服务端调整虚拟屏幕后让 rdaccel 重建编码器。
4. **帧级 ACK 流控**：GFX 的 StartFrame/EndFrame/FrameAck 可以直接映射到 rdaccel 的发送窗口设计。
5. **可选兼容层**：长期可以考虑在 rdmachine 服务端旁挂 FreeRDP 服务端库提供 RDP 网关能力，让 mstsc 直接接入。但这和 rdaccel 不做 H.264 的边界冲突，只能用 RemoteFX/Progressive，优先级低。

---

## Apache Guacamole

### 定位与许可证
- Apache 软件基金会顶级项目，许可证 Apache-2.0。定位是**"无客户端"的 HTML5 远程桌面网关**：用户只需浏览器，guacd 在服务端代为连接 RDP/VNC/SSH/Telnet/Kubernetes 并把画面转成 Guacamole 协议推给浏览器。它本身**不是**远程桌面服务端，而是协议转换和接入网关，常用于堡垒机和 VDI 门户。https://guacamole.apache.org/doc/gug/guacamole-architecture.html
- 当前版本是 1.6.0（2025 年发布，具体日期待核实）。主要变化：支持 FreeRDP 3；**重写 guacd 服务端协议优化器**，可实时检测滚动/移动、更好地合并/拆分更新、**并行编码**，不需要 GPU；录屏回放增强。https://guacamole.apache.org/releases/1.6.0/

### 整体架构
```
浏览器 (guacamole-common-js) ──WebSocket/HTTP 隧道──> guacamole-client (Java Web 应用, Tomcat)
                                                   │  认证/授权扩展、连接配置
                                                   ▼ Guacamole 协议 (TCP 4822, 可 TLS)
                                                 guacd (C 守护进程, libguac)
                                                   ├─ fork 每连接进程 + 加载 libguac-client-rdp (FreeRDP)
                                                   ├─ libguac-client-vnc (libvncclient)
                                                   └─ libguac-client-ssh / telnet / kubernetes (终端模拟器)
                                                   ▼
                                           RDP / VNC / SSH 目标主机
```
- **guacd**（guacamole-server，C）：每个连接 fork 一个子进程，子进程按协议名动态加载 `libguac-client-<proto>.so`。插件实现 `guac_client` 回调（join/leave、鼠标/键盘、剪贴板、文件流），通过 libguac 的 `guac_display`/图层 API 输出绘制指令。https://github.com/apache/guacamole-server
- **guacamole-client**（Java）：Web 应用和 REST API。认证用可插拔扩展（JDBC MySQL/PostgreSQL/SQL Server、LDAP、TOTP、Duo、SAML、OpenID、CAS、RADIUS、header auth、JSON auth），管理连接、连接组（含负载均衡）、用户权限、历史记录。https://github.com/apache/guacamole-client
- **guacamole-common / guacamole-common-js**：Java 隧道 API 和 JS 客户端库，可以单独嵌入第三方 Web 应用（很多厂商这样做）。

### Guacamole 协议
- 文本协议，每条指令形如 `长度.操作码,长度.参数,...;`，例如 `4.size,1.0,4.1024,3.768;`。长度按 Unicode 字符计，便于流式解析。https://guacamole.apache.org/doc/gug/guacamole-protocol.html
- **握手**：`select`（协议/连接 ID）→ 服务端回 `args`（含协议版本 `VERSION_1_5_0` 等）→ 客户端发 `size`/`audio`/`video`/`image`（声明支持的 MIME）/`timezone`/`name` → `connect` → `ready`。
- **绘图模型**：类 Canvas 的 2D 图层系统。可见层是正索引，离屏缓冲是负索引；指令包括 `img`（流式 PNG/JPEG/WebP）、`copy`、`rect/cfill`、`transfer`、`cursor`、`move/shade/dispose`（层操作）。`sync` 标记帧边界并携带时间戳，客户端回 `sync` 作为 **ACK**，guacd 据此测往返延迟、调节帧率和画质。
- **流（stream）机制**：`blob`/`end`/`ack` 构成通用分块流，承载图像、音频（`audio`）、剪贴板（`clipboard`）、文件上传/下载（`file`、`put`/`get` 配合 `filesystem` 对象）、管道（`pipe`，即命名的任意数据通道），每块都有 ACK 背压。
- **输入**：`mouse`（坐标 + 按键掩码）、`key`（X11 keysym + 按下/抬起）、`touch`、`size`（窗口大小变化 → 触发 RDP 动态分辨率）。

### 画面采集与编码
- guacd 本身不采集，它在**服务端完整解码**上游协议（FreeRDP 把 RDP 位图/RemoteFX/GFX 解到 GDI 帧缓冲；VNC 用 libvncclient），再对**脏区重新编码**成浏览器能解的图片。编码器按区域特征启发式选择：大面积/图像类内容用 JPEG 或 WebP（有损），文字/UI 用 PNG（无损），纯色用 `cfill`，滚动/移动检测出来的用 `copy`（1.6 的优化器重点改进）。
- 没有视频编解码：浏览器端也没有 H.264/WebCodecs 路径（`video` 指令只用于流式视频重定向场景，基本未用于桌面，待核实）。所以对上游 RDP 也不会透传 H.264，而是"解码 → 再编码成图片"。
- 编码全在 CPU 上做，每个会话都消耗网关 CPU，这是规模化部署的主要瓶颈。1.6 引入了并行编码来缓解。

### 输入、剪贴板、音频、文件、多显示器、会话
- **剪贴板**：仅文本（受浏览器 Clipboard API 限制，旧版需要侧边栏手动粘贴，新浏览器可以自动同步）。
- **音频**：RDP/VNC（PulseAudio）的下行音频在 guacd 转成 PCM 流，浏览器用 Web Audio 播放。RDP 麦克风上行也支持。
- **文件传输**：RDP 驱动器重定向把 guacd 主机上的一个目录映射给远端，浏览器经它上传/下载。SSH 用 SFTP。支持拖拽上传。
- **打印**：RDP 虚拟打印机输出 PDF，供浏览器下载。
- **多显示器**：基本只支持单视口，窗口大小变化时可用 RDP display update 动态调整分辨率。浏览器多窗口多屏不是成熟功能（待核实）。
- **会话共享**：**sharing profile** 生成分享链接，多人可同时加入同一 guacd 连接（只读或可控），由 guacd 的多用户 join 机制实现。
- **会话录制/审计**：录制成 `.guac` 协议流，可用浏览器内播放器或 `guacenc` 转成视频。SSH 支持 typescript 录制。这是堡垒机场景的卖点。
- 是否虚拟会话取决于后端：接 xrdp/Windows RDP 就是虚拟会话，接 VNC 就是共享物理桌面。Guacamole 本身不管会话生命周期。

### 性能与延迟
- 优点是带宽可控、穿透性好（只用 443/WebSocket）。但"解码 + 再编码 PNG/JPEG + 浏览器解码图片"链路长，加上一次中转跳点，交互延迟比原生 RDP 客户端高。全屏视频、3D、高刷场景帧率低，图片编码的带宽也明显高于 H.264。
- 帧率调节依赖 `sync` ACK 的往返时间，弱网下会主动降帧和降低 JPEG 质量。

### 优点
- 零安装浏览器接入，一个网关统一 RDP/VNC/SSH，企业认证集成（LDAP/SAML/OIDC/TOTP）完善，有录屏审计和会话分享。
- 协议简单、文本化、易调试；JS 客户端库可以单独嵌入，二次开发生态大。
- 模块化：协议插件（`libguac-client-*`）和认证扩展（Java SPI）边界清晰。

### 缺点 / 常见痛点
- 网关 CPU 密集，会话多时需要水平扩展 guacd；不支持 GPU/H.264 编码管线。
- 剪贴板只有文本，键盘布局和输入法（IME、中文输入）问题多。多显示器、高 DPI 支持弱。
- 体验上限受上游协议和"再编码"限制，视频/动画卡顿。部署组件多（guacd + Tomcat + 数据库 + 反向代理）。

### 对 rdmachine 的可借鉴点
1. **浏览器接入路径**：rdmachine 的 Web 客户端有两条路。①**网关转码**（Guacamole 式）：兼容性最好但 CPU 重、延迟高。②**浏览器直解**：rdaccel 的码流用 WebCodecs（VP8/VP9 等非 H.264 编码，按 rdaccel 支持的 codec 决定）或 WASM 解码，经 WebSocket/WebTransport/WebRTC DataChannel 直接送到浏览器，网关只做中继和鉴权。推荐②，①仅作为不支持解码时的降级方案。
2. **协议设计**：借鉴 Guacamole 的**通用 stream + blob + ack 背压**抽象，剪贴板、文件、音频、自定义 `pipe` 共用一套分块流机制。`sync` 帧边界 + 客户端回 ACK 测 RTT 的帧率自适应思路，可以和 rdaccel 的发送窗口结合。
3. **会话分享 / 只读观察**：sharing profile（分享链接 + 只读/可控权限）虽然 rdaccel 不做多观看者，但应用层可以在服务端做"一路编码 → 多路转发"的中继，或者至少预留接口。
4. **录制回放**：在协议流层面录制（而不是录视频），存储小、可检索，适合作为后续审计功能。
5. **插件化接入**：guacd 的"每连接 fork + 按协议名加载插件"模型，可以用于 rdmachine 的会话前端/网关进程隔离（单会话崩溃不影响整体），认证也做成可插拔后端（PAM/LDAP/OIDC）。
6. **嵌入式 JS SDK**：把 Web 客户端做成独立 npm 库（参照 guacamole-common-js），方便第三方门户集成。

## X2Go（NX 技术）

### 背景：NoMachine NX 协议的演进
- **NX 1–3（2003–2012 前后）**：NoMachine 把核心库（nxcomp / nxproxy / nxagent）以 GPLv2 开源，商业版另售。设计源自 DXPC（Differential X Protocol Compressor），核心思路是在 X11 协议层做压缩、缓存和**往返（round-trip）消除**，而非传像素视频。来源：[Wikipedia: NX technology](https://en.wikipedia.org/wiki/NX_technology)
- **NX 4（2013 起）**：NoMachine 4 改为闭源，协议转向“帧缓冲采集 + 视频编码”（H.264 为主，后加 VP8、硬件编码等，具体编码清单待核实），不再依赖 X11 代理，以此支持 Windows/macOS 和 Wayland 桌面。来源同上。
- 开源世界里 NX 3 的遗产分成几支：FreeNX（2008 年后基本停滞）、X2Go（最活跃），以及维护共享库的 **ArcticaProject/nx-libs**（X2Go 与 QVD 共同维护，3.6.x 分支，GPL-2.0）。来源：[nx-libs GitHub](https://github.com/ArcticaProject/nx-libs)

### 定位与许可证
- 定位：Linux/Unix 服务器上的远程 X11 桌面/单应用会话（类 Citrix/NoMachine 3 的“终端服务器”模式），客户端支持 Linux/Windows/macOS。
- 许可证：x2goserver / x2goclient 为 GPLv2+，部分组件 AGPLv3+；nx-libs 为 GPLv2。来源：[Wikipedia](https://en.wikipedia.org/wiki/NX_technology)、[AUR x2goserver](https://aur.archlinux.org/packages/x2goserver/)

### 整体架构与会话模型
- **组件**：
  - `x2goclient`（Qt 客户端）/ `pyhoca-gui`、`pyhoca-cli`（Python 客户端，基于 python-x2go）。
  - `x2goserver`：一组 Perl/shell 脚本（`x2gostartagent`、`x2goresume-session`、`x2gosuspend-session`、`x2golistsessions` 等），**没有常驻守护进程**，全部经 SSH 登录后调用；会话状态存数据库（默认 SQLite，可选 PostgreSQL 用于多服务器集群）。
  - `x2goagent`：即 nxagent 的改名包装，是一个**嵌套 X 服务器**（基于 nx-X11），桌面会话里的应用实际连到它。
  - `nxproxy` / `libXcomp`：两端的协议压缩代理，客户端侧把 NX 流还原成本地 X11 请求，交给本地 X server（Windows 上是 VcXsrv，macOS 上是 XQuartz）显示。
- **会话流程**：客户端 SSH 登录 → 服务器脚本启动 x2goagent（分配 display 号和端口，监听在 localhost）→ 客户端通过 **SSH 端口转发**连接该端口 → 启动桌面环境或单个应用。
- **挂起/恢复**：因为 X 应用连的是服务器端的 nxagent 而不是客户端 X server，断网或主动挂起时 nxagent 继续运行（只是不再输出），应用状态全部保留；之后可从**另一台客户端**恢复会话，nxagent 按新客户端的分辨率重新 resize 并重绘。来源：[X2Go FAQ](https://wiki.x2go.org/doku.php/doc:faq:start)
- **会话类型**：完整桌面（KDE/XFCE/MATE/LXDE 等）、单应用“published applications”（无缝窗口，rootless 模式）、**桌面共享/影子会话**（x2godesktopsharing，连接已在本地物理 :0 上运行的 X 会话，经 nxcompshad 轮询差分；性能明显不如虚拟会话，细节待核实）。

### 传输协议
- **全部走 SSH**：认证（密码/密钥/Kerberos-GSSAPI）、加密、端口转发都交给 OpenSSH，X2Go 自身不做加密与 NAT 穿透。优点是在已有 SSH 的服务器上零配置、安全模型清楚；缺点是 **TCP over SSH**，丢包时会出现队头阻塞，在高延迟或丢包链路上卡顿明显。
- NX 流内部：libXcomp 按 X11 消息类型分通道（control/X/音频/打印等的多路复用待核实），对每类消息做差分编码 + 消息缓存（两端维护同步缓存，命中时只发引用）+ zlib/JPEG/PNG 等压缩；提供 link 速度档位（modem/isdn/adsl/wan/lan）调节压缩强度与缓存策略。

### 画面采集与编码
- **不是“采集 + 视频编码”**，而是在 X11 协议层代理：nxagent 本身就是 X server，直接看到绘图请求（字体、像素图、绘制原语），把可压缩的请求以差分/缓存形式发出；图像类数据用 JPEG（有损，默认）或 PNG（无损）。图像会缓存，重复图像不再发送。来源：[X2Go FAQ](https://wiki.x2go.org/doku.php/doc:faq:start)
- nxagent 默认启用“延迟/懒惰更新”（defer），把小的绘图合并后以图像方式推送，以降低往返次数。
- 结果：文本、办公、IDE 一类 2D 界面带宽极低且清晰（无视频压缩伪影）；但视频播放、3D/合成器动画、现代浏览器（大量客户端渲染位图）效果差，不支持 GLX/硬件加速（只有软件 OpenGL，待核实具体版本）。
- **无硬件编码**，没有 H.264/HEVC/AV1，没有 HDR。

### 输入、剪贴板、音频、文件/打印、多显示器
- 输入：X11 键鼠事件转发，存在键盘布局映射问题（客户端与服务器布局不一致时经常出问题，常见痛点）。
- 剪贴板：双向，可配置方向（客户端→服务器、服务器→客户端、双向、禁用），支持 PRIMARY/CLIPBOARD 选区。
- 音频：通过 PulseAudio（客户端启用 TCP 原生协议模块），经 SSH 隧道转发；也支持 ESD/arts（老旧）。来源：[X2Go FAQ](https://wiki.x2go.org/doku.php/doc:faq:start)
- 文件共享：客户端目录通过 **反向 SSHFS** 挂载到服务器（需要 fuse 组权限）。
- 打印：服务器端的 CUPS 虚拟打印机生成 PDF，回传到客户端打印或预览（x2goprint）。
- 多显示器：Xinerama 支持，可选择“所有显示器”或指定显示器；与 KDE 的 KScreen 热插拔逻辑会冲突，FAQ 建议禁用 KScreen。来源同上。

### 性能与延迟特点
- 低带宽（几十到几百 kbit/s 就能办公）、在 LAN 和中等延迟 WAN 上响应好，主要得益于往返消除。
- 帧率和动画能力弱，没有基于帧的码率控制；TCP/SSH 在丢包场景下延迟抖动大。

### 优点
- 真正的**多用户虚拟会话 + 挂起/恢复 + 漫游**（换机器接着用），这一点至今是 X2Go 的核心卖点。
- 完全依托 SSH，部署门槛低，安全面小，无需开额外端口。
- 2D 文本画质无损，带宽极省；单应用无缝窗口模式。
- 开源，有集群/会话代理（x2gobroker）。

### 缺点 / 常见痛点
- **不支持 Wayland/Mir**：NX 3 就是 X11 协议，官方 FAQ 明确说无法支持。来源：[X2Go FAQ](https://wiki.x2go.org/doku.php/doc:faq:start)。随着 GNOME/KDE 默认改为 Wayland，只能退回 X11 会话或使用 XFCE/MATE 等。
- 现代合成桌面（GNOME Shell、KDE 带合成效果）下效果差，甚至无法启动（需要 GLX 合成）；浏览器、视频卡顿。
- 服务器只能是 Linux/Unix；Windows/macOS 客户端依赖 VcXsrv/XQuartz，体验参差。
- nx-libs 基于老版 X.Org，rebase 到新 X server 的工作尚未完成，维护人手有限。来源：[nx-libs GitHub](https://github.com/ArcticaProject/nx-libs)
- 影子会话（物理桌面共享）性能差，不是它的强项。

### 对 rdmachine 的可借鉴点
1. **会话与传输解耦**：X2Go 的挂起/恢复之所以成立，是因为“会话宿主”（nxagent）和“网络连接”是两个独立的生命周期。rdmachine 可以在应用层设计“会话对象”：rdaccel 采集/编码管线在客户端断开后进入**空闲态**（停止编码或降到极低帧率，保留编码器状态或重建），重连时只需**强制关键帧 + 按新客户端分辨率/能力重新协商**。在 Linux 上若要做多用户虚拟桌面，可考虑无头 Wayland compositor（如 headless wlroots/mutter --headless）或 Xvfb 方案作为 rdaccel 的采集源，对标 nxagent 的角色。
2. **会话发现/管理 API**：参考 `x2golistsessions / x2goresume-session / x2gosuspend-session` 的语义，应用层提供 list/suspend/resume/terminate 接口，并把会话状态持久化，方便以后做 broker。
3. **可选的 SSH 引导**：用 SSH 做首次认证和启动服务（“无守护进程”模式）是很好的零配置入口，可作为 rdmachine 的一种连接方式（SSH 登录启动 server + 交换密钥，之后切到 UDP 数据通道），兼顾安全与易部署，并规避 TCP over SSH 的队头阻塞。
4. **文本/静态画面优化**：NX 的经验说明办公场景下“清晰+省带宽”最重要。rdaccel 可以借鉴“缓存+差分”思路：脏区域检测、静止时停止发送、静止后补一帧高质量/无损 refinement，避免视频编码在文字上出现伪影。
5. **附属通道可插拔**：X2Go 把音频、文件、打印交给 PulseAudio/SSHFS/CUPS 等成熟组件。rdaccel 不做音频/文件，rdmachine 应用层同样可复用现成方案（如 PipeWire/Opus、SFTP），不必自研。

---

## Sunshine / Moonlight

### 定位与许可证
- **Sunshine**（LizardByte）：自托管的游戏串流主机端，实现 NVIDIA GameStream 协议（NVIDIA 已在 2023 年停止 GameStream，Sunshine 成为主要的开源替代），带 Web UI 用于配置和配对。支持 Linux、Windows、macOS（14.2+）、FreeBSD。来源：[Sunshine 文档](https://docs.lizardbyte.dev/projects/sunshine/latest/)
- **Moonlight**：客户端，覆盖 PC（moonlight-qt）、Android、iOS/tvOS、Steam Deck、浏览器、嵌入式等，共享核心协议库 `moonlight-common-c`。
- 许可证：Sunshine 与 Moonlight 均为 GPLv3（待核实具体文件，社区普遍认知为 GPL-3.0）。
- 生态：Apollo（Sunshine 分支，加入虚拟显示器、按客户端权限等）、Wolf（Games on Whales，用 Docker 在一台主机上同时提供多个虚拟会话的 GameStream 服务器）、foundation-sunshine 等。

### 整体架构
- **流水线**：采集（capture）→ 颜色转换/缩放（GPU 上完成）→ 硬件编码 → RTP 分包 + FEC → UDP 发送；音频单独采集 → Opus 编码 → RTP。
- **采集后端**：Windows 上 DXGI Desktop Duplication（以及部分支持 Windows.Graphics.Capture）；macOS 上 ScreenCaptureKit；Linux/FreeBSD 上 KMS/DRM、X11、wlroots（wlr-screencopy）、XDG Desktop Portal（PipeWire）、KWin Screencast，以及 NvFBC（仅 X11 + NVIDIA）。来源：[Sunshine 文档](https://docs.lizardbyte.dev/projects/sunshine/latest/)
- **编码器**：NVENC（Linux/Windows）、VAAPI（Linux/FreeBSD，AMD/Intel/NVIDIA）、QuickSync、AMF（Windows）、VideoToolbox（macOS）、Vulkan Video（Linux，AMD 完整，Intel/NVIDIA 部分）、软件编码（x264/x265/SVT-AV1 等，经 FFmpeg）。来源同上。
- **会话模型**：“应用”（App）列表，每个 App 可以是桌面或一个游戏启动命令，带 prep/undo 命令（例如开流前改分辨率、启用虚拟显示器、结束时恢复）。单主机同时通常只服务一个流（Sunshine 本身不做多用户虚拟会话，Wolf 解决这个问题）。断开后应用可保持运行，客户端可“恢复”同一 App，但这是“重连到正在运行的游戏/桌面”，不是 X2Go 那种独立的虚拟会话。

### 传输协议（GameStream）
| 通道 | 端口（默认） | 加密 | 作用 |
|---|---|---|---|
| HTTP | TCP 47989 | 无 | 未配对时交换公开信息，进行配对 |
| HTTPS | TCP 47984 | TLS（双向证书） | 仅已配对客户端；获取应用列表、下发会话密钥、启动/恢复/结束流 |
| RTSP | TCP 48010 | 新版可 AES-GCM | 协商后续流的端口和参数（编码、分辨率、FEC、码率等） |
| Control（ENet） | UDP 47999 | AES-GCM-128 | 键鼠/手柄/触控输入、丢帧报告、IDR/RFI 请求、HDR 元数据、反馈 |
| Video（RTP） | UDP 47998 | 可选 AES-GCM | H.264/HEVC/AV1 视频 |
| Audio（RTP） | UDP 48000 | AES-CBC-128 | Opus 音频 |

来源：[Wolf: Moonlight Protocols](https://games-on-whales.github.io/wolf/stable/protocols/index.html)、[DeepWiki: moonlight-common-c 网络协议](https://deepwiki.com/moonlight-stream/moonlight-common-c/4-network-protocols)

- **FEC**：视频使用 **Reed-Solomon** FEC，每帧切分为数据分片 + 校验分片，FEC 比例可配置（Sunshine 设置项 `fec_percentage`，默认约 20%，待核实）；大帧可拆为最多 4 个 FEC 块（multi-FEC），客户端回报 FEC 状态供服务器调整。来源：[DeepWiki](https://deepwiki.com/moonlight-stream/moonlight-common-c/4-network-protocols)
- **丢包恢复**：FEC 无法恢复时，客户端经控制通道请求 **参考帧失效（RFI）**（编码器支持时只让受损参考帧失效，下一帧以更早的完好帧为参考）或回退为 **IDR 关键帧** 请求；客户端用 RTP 序号检测丢包，并有“推测性 RFI”及冷却期机制避免过度请求。来源同上。
- **拥塞控制**：GameStream 基本是**固定码率**（用户在客户端选择码率），没有类似 GCC/BBR 的自适应码率；只有丢帧时的错误恢复，没有主动降码率（待核实最新版本是否加入动态码率；一些分支有实验性实现）。这是在 Wi-Fi/WAN 上体验不稳定的主要原因。
- **NAT/外网**：协议本身无 NAT 穿透和中继，外网需要端口转发、UPnP，或者叠加 Tailscale/ZeroTier 等 VPN。

### 配对流程（PIN + 证书钉扎）
1. 客户端生成自签名证书；通过 HTTP 发送 salt 和客户端证书；主机（Sunshine Web UI）显示/输入 4 位 PIN。
2. 双方用 `SHA256(salt + PIN)` 派生 AES 密钥，后续挑战都用它加密。
3. 互相执行挑战-应答：服务器把“客户端挑战 + 服务器证书签名 + 随机 server secret”做哈希后加密返回；客户端同样返回自己的哈希；服务器发出用私钥签名的 pairing secret。
4. 服务器校验客户端哈希和签名，成功后**记住客户端证书**。
5. 之后一切经 HTTPS 双向证书认证，会话启动时由服务器经 HTTPS 下发本次流的 AES 密钥（rikey）。
来源：[Wolf: HTTP/S Pairing](https://games-on-whales.github.io/wolf/stable/protocols/http-pairing.html)
- 评价：本质是 **PIN 认证的证书交换（TOFU + 带外确认）**，用户体验好；但 4 位 PIN 短、协议是历史包袱（自定义构造，非 PAKE），安全性一般。

### 画面采集与编码
- 纯视频编码路线：H.264、HEVC、AV1（需要 GPU 支持）；**HDR**：HEVC Main10 / AV1 10-bit + HDR10 元数据（Windows 主机支持最完整，Linux/macOS HDR 支持有限，待核实），4:4:4 色度在新版本中支持（待核实）。
- 低延迟设置：无 B 帧、单参考或少量参考、超低延迟调优（NVENC ULL）、帧内刷新/RFI、按客户端请求的帧率（最高 120/240 fps）。
- 零拷贝：Linux 上 KMS/DRM 采集出 DMA-BUF，直接导入 VAAPI/CUDA 编码，不经过 CPU；Windows 上 D3D11 纹理直接送编码器。这也是端到端延迟可以做到个位数毫秒到十几毫秒（LAN）的关键。
- 缺点：KMS 采集需要 `cap_sys_admin` 权限；Wayland 下若走 portal 会有授权弹窗和额外延迟；无头/虚拟显示器需要额外手段（EDID 假显示器、Apollo 的 SudoVDA、Wolf 的无头 compositor）。

### 输入、剪贴板、音频、文件、多显示器
- 输入：键盘、鼠标（相对/绝对）、触控、触控笔、**手柄**（Xbox/DS4/DualSense/Switch Pro 仿真，含体感、触控板、震动），Linux 上走 uinput / inputtino，macOS 不支持手柄仿真。来源：[Sunshine 文档](https://docs.lizardbyte.dev/projects/sunshine/latest/)
- 剪贴板：GameStream 协议原本没有；Moonlight 有限支持粘贴文本（待核实），Apollo 等分支加入了剪贴板同步。
- 音频：Opus，立体声/5.1/7.1 环绕声，可选在主机静音。
- 文件/打印：**不支持**。
- 多显示器：一次只串流一个显示器，可选择采集哪一个；没有多显示器同时输出。

### 性能与延迟特点
- LAN 下端到端延迟极低（常见报告为 10–20 ms 级别，取决于编码/解码硬件和显示刷新率，待核实具体测试），4K120、HDR 都可用。
- 码率高（常见 20–150 Mbps），对 Wi-Fi 质量敏感；WAN 上没有自适应码率，丢包高时画面频繁出现 IDR/卡顿。

### 优点
- 业界水平的低延迟流水线：GPU 零拷贝采集、硬件编码、UDP+FEC+RFI。
- 跨平台主机和极其丰富的客户端，社区活跃，协议被多个服务器实现（Sunshine、Apollo、Wolf）复用。
- 配对简单，外设（手柄）支持完善，HDR/AV1 等新特性跟进快。

### 缺点 / 常见痛点
- **偏游戏**：没有剪贴板（原生）、文件传输、多用户、多显示器、打印等办公远程桌面功能；默认面向“单用户、单物理显示器”。
- 无头服务器、虚拟显示器、登录界面/锁屏（Windows UAC、Linux greeter）处理麻烦。
- 无 NAT 穿透/中继，外网要自己配端口转发或 VPN。
- 无自适应码率/拥塞控制。
- 协议为逆向 GameStream 的历史包袱（端口多、加密不统一），macOS 主机支持较弱。

### 对 rdmachine 的可借鉴点
1. **低延迟流水线**：rdaccel 在 macOS 上用 ScreenCaptureKit → IOSurface → VideoToolbox（HEVC，低延迟模式 `kVTVideoEncoderSpecification_EnableLowLatencyRateControl`），Linux 上 KMS/PipeWire DMA-BUF → VAAPI 零拷贝，参考 Sunshine 的 `src/platform/*` 与 `src/video.cpp` 组织方式：采集、颜色转换、编码各自用接口抽象，按平台/GPU 逐个探测可用组合。
2. **FEC + RFI 组合**：参照 GameStream 每帧 Reed-Solomon（可调比例，大帧拆块）+ 失败时 RFI/IDR 的分层恢复策略；rdaccel 可以更进一步加入 **NACK 重传（RTT 短时）+ 自适应 FEC 比例**，并根据丢包/RTT 做码率控制，补上 GameStream 最大的短板（可参考 WebRTC GCC 或简化版延迟梯度算法）。
3. **控制通道设计**：输入、关键帧请求、丢帧报告、统计反馈走一条加密的可靠/半可靠 UDP 通道（ENet 风格），与视频通道分离；时间戳和帧号用于客户端统计端到端延迟。
4. **配对流程**：保留“PIN 带外确认 + 证书持久化（TOFU）”的用户体验，但用标准密码学替换（如 SPAKE2/OPAQUE 或 Noise XX + 短认证串 SAS），密钥派生后长期信任设备证书；rdaccel 只需提供“给定会话密钥的加密传输”，配对放在应用层，符合当前分层。
5. **虚拟显示器/无头会话**：Apollo 的虚拟显示、Wolf 的无头 compositor 说明“按客户端分辨率创建虚拟屏”是用户强需求；结合 X2Go 的挂起恢复，rdmachine 可以把“虚拟屏 + 会话保持”做成差异化卖点。
6. **办公场景补齐**：Sunshine 证明了视频编码路线在游戏上可行，但文字清晰度依赖高码率；rdmachine 面向桌面办公，应在视频编码基础上加入静止区域 refinement / 高码率补帧以及 4:4:4（若硬件支持）选项。
