# NoMachine 功能清单调研

> 调研日期：2026-11-06。调研对象是 NoMachine（含 NX 协议）的公开文档：nomachine.com 产品页、kb.nomachine.com 知识库、官方论坛，以及少量第三方资料（Linux Journal 的 NX 架构文章、Debian 的 nxproxy manpage）。
> 用途：作为 rdmachine 的第一步输入。第二步是逐项评估 rdaccel 能否支撑（见 [rdaccel-assessment.md](rdaccel-assessment.md)）。

## 阅读说明

**重要度**

- **P0**：远程桌面核心必备，缺了就不能用。
- **P1**：NoMachine 体验的关键差异点，用户选择它的原因。
- **P2**：锦上添花，或者属于企业版特性。

**可信度**：NoMachine 官方很少公开协议和编码细节。官方没有写明、只能从论坛或推测得出的内容，统一标为"（待核实）"。表中的配置项名称、端口、版本号都出自所引页面。

**版本背景**：按官方 "What's New" 页面，v10 起免费版由收费的 Personal Edition 取代，Enterprise Client 改名为 NoMachine Player。下文的"免费版/个人版"指这一档。

**许可缩写**：ED = Enterprise Desktop；TS 家族 = Workstation / Small Business Terminal Server / Terminal Server / Enterprise Terminal Server（只在 Linux 上）；CS 家族 = Workgroup Cloud Server / Cloud Server / Enterprise Cloud Server（含 Cluster）。

## 目录

1. [总览：P0 清单](#1-总览p0-清单)
2. [显示采集与编码](#2-显示采集与编码)
3. [网络传输与协议](#3-网络传输与协议)
4. [音频](#4-音频)
5. [输入](#5-输入)
6. [剪贴板与文件传输](#6-剪贴板与文件传输)
7. [设备转发](#7-设备转发)
8. [会话模型](#8-会话模型)
9. [认证与安全](#9-认证与安全)
10. [网络穿透与发现](#10-网络穿透与发现)
11. [跨平台支持](#11-跨平台支持)
12. [客户端体验](#12-客户端体验)
13. [管理与部署](#13-管理与部署)
14. [产品线与授权](#14-产品线与授权)
15. [对 rdmachine 的启示](#15-对-rdmachine-的启示)
16. [待核实与矛盾点](#16-待核实与矛盾点)

---

## 1. 总览：P0 清单

把各模块的 P0 抽出来，就是一个"能用的远程桌面"的最小集合：

| 模块 | P0 功能 |
|---|---|
| 显示 | 物理桌面采集（Windows / Linux X11+Wayland / macOS）、光标、H.264 编码（硬件优先、软件兜底）、画质滑块、按网络自适应、缩放到窗口、远端分辨率跟随、多显示器 |
| 传输 | TCP 主通道（单端口）、多路复用加流控、带宽自适应、断开后会话保留与恢复 |
| 音频 | 远程音频回传、服务端接入系统音频服务 |
| 输入 | 键盘鼠标转发、键盘布局正确 |
| 剪贴板/文件 | 双向文本剪贴板、文件上传下载 |
| 会话 | 连接物理桌面、headless 机器自建虚拟显示、主人审批弹窗 |
| 安全 | 系统账户密码登录、全程 TLS 加密 |
| 穿透 | 支持手动端口转发（至少能直连） |
| 平台 | Windows、macOS、Linux 三端服务端和客户端 |
| 客户端 | 连接列表、显示模式切换 |
| 管理 | 配置文件、服务控制 CLI、日志收集 |

---

## 2. 显示采集与编码

### 2.1 画面采集

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| Windows 物理桌面采集 | 服务端开启"Use acceleration for display processing"后用 DirectX 采集，仅 Windows | P0 | https://kb.nomachine.com/DT10O00154 |
| Linux X11/Wayland 物理桌面采集 | v6.1 起支持 Wayland（GNOME 3.20+、KDE 5.4+）；v8 起默认 EGL 采集，可选 PipeWire（仅 GNOME）或 DRM 回退，node.cfg 键 `WaylandModes "egl,compositor,drm"`；多屏、黑屏、自动 resize 依赖 D-Bus 和 PipeWire（v7.7.4 起） | P0 | https://kb.nomachine.com/?p=948 , https://kb.nomachine.com/?p=1053 |
| macOS 物理桌面采集 | v8 起支持硬件 H.264；采集用的 API 没写（待核实） | P0 | https://kb.nomachine.com/?p=10867 |
| Linux 虚拟桌面（nxagent） | 服务端跑一个基于 X.org/Xnest 的显示代理，不需要物理显示器 | P1 | https://www.linuxjournal.com/node/8489 , https://kb.nomachine.com/?p=1007 |
| GNOME Wayland 虚拟桌面 | v10 起 Terminal Server 支持 GNOME 49+ 的 Wayland 虚拟桌面 | P2 | https://nomachine.com/what-s-new-in-nomachine |
| headless 机器 | 检测不到 X server 时自动创建内置虚拟显示；Enterprise Desktop 可以配置自动建显示并指定 owner；Wayland 下必须接真实显示器 | P1 | https://kb.nomachine.com/?p=952 , https://kb.nomachine.com/?p=442 |
| 光标形状与位置 | Wayland 下有远端光标跟踪（v7.7.4），但形状采集"可能不可靠"；是否作为独立通道发送（待核实） | P0 | https://kb.nomachine.com/?p=948 |

### 2.2 编码器与画质

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| 多编码器 | 支持 H.264、VP8、JPEG/MJPEG。客户端能解 H.264 就用 H.264，否则 VP8；MJPEG 只在显式指定时用，适合弱机，带宽更高 | P0 | https://kb.nomachine.com/?p=761 , https://kb.nomachine.com/DT10O00154 |
| 协议与编码器解耦 | NX 协议不绑定编码器，同一个流里可以混用多种编码 | P1 | https://kb.nomachine.com/?p=761 |
| 软件 H.264 兜底 | 6.6.8 起各平台安装包自带软件编解码器，硬件不可用时回退 | P0 | https://kb.nomachine.com/?p=721 |
| 指定编码器和帧率 | 服务端 Performance 页可以指定编码和帧率，配置键 `DisplayServerVideoCodec`；官方建议一般不手动改 | P1 | https://kb.nomachine.com/DT10O00154 , https://kb.nomachine.com/?p=836 |
| 画质滑块（Display quality） | 用户设目标画质，系统按编解码耗时、延迟和带宽实时调整帧率和画质去逼近 | P0 | https://kb.nomachine.com/?p=761 , https://kb.nomachine.com/?p=893 |
| 网络自适应画质（可关） | 关闭后画质固定为滑块值，不随拥塞变化 | P1 | https://kb.nomachine.com/?p=893 |
| 多遍渐进精修（multi-pass） | 先用低质量快速发出，画面静止后逐步精修，也用来修正 YUV 色度子采样造成的偏色；可关闭 | P1 | https://kb.nomachine.com/?p=893 , https://forum.nomachine.com/?p=33831 |
| 有损/无损切换 | 按屏幕活动自动渐进精修；X11 矢量模式下 `pack=16m-png` / `pack=rgb` 为无损 | P1 | https://kb.nomachine.com/?p=761 , https://kb.nomachine.com/?p=1007 |
| X11 矢量图形模式 | Linux 虚拟桌面默认开：不做视频编码，只压缩 X 协议，带宽和 CPU 更低；不适合 GPU 渲染的桌面 | P1 | https://kb.nomachine.com/?p=761 , https://kb.nomachine.com/?p=721 |
| 客户端后处理和解码缓冲开关 | 在弱机或局域网上关掉以降低延迟 | P2 | https://forum.nomachine.com/?p=26694 |
| 视频区域检测 | 只有"按屏幕活动自适应"这一表述（待核实） | P1 | https://kb.nomachine.com/?p=761 |
| 4:4:4 / H.265 / AV1 / HDR | 没找到官方说明（待核实） | P2 | — |

### 2.3 硬件加速

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| NVIDIA NVENC | Windows 和 Linux，要求 Kepler 及以后的 GPU | P1 | https://kb.nomachine.com/?p=721 |
| Intel QSV | Windows 和 Linux；8.12 起用 VPL，旧硬件用 MediaSDK；Connection 面板会显示"HW encoding, H.264, VPL" | P1 | https://kb.nomachine.com/AR09O00938 |
| AMD / macOS 硬件编码 | v8 起支持；macOS 用的 API 推测是 VideoToolbox（待核实） | P1 | https://kb.nomachine.com/?p=721 , https://kb.nomachine.com/?p=10867 |
| Linux VA-API | v10 起支持厂商中立的 VA-API，替代已停用的 AMF | P1 | https://nomachine.com/what-s-new-in-nomachine |
| 客户端硬件解码 | Windows、Mac、Linux（需要 `/dev/dri` 权限）；iOS/Android 不支持 | P1 | https://kb.nomachine.com/?p=721 |
| 多 GPU 负载均衡 | v8 可以把编码分散到多块 GPU，或指定专用 GPU | P2 | https://kb.nomachine.com/?p=10867 |

### 2.4 分辨率、缩放与多显示器

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| 缩放到窗口（Scale to window） | 物理桌面默认模式；v9.1 起保持宽高比 | P0 | https://kb.nomachine.com/?p=838 |
| 1:1 视口（Viewport） | 虚拟桌面默认模式，鼠标到窗口边缘时平移 | P1 | https://kb.nomachine.com/?p=838 |
| 远端分辨率跟随窗口（Resize remote display） | 物理桌面受显卡支持的分辨率限制，虚拟桌面不限 | P0 | https://kb.nomachine.com/?p=838 , https://www.nomachine.com/changing-how-you-view-the-remote-host |
| 多显示器 | 可以给每个显示器单独设分辨率；Wayland 多屏要求 v7.7.4+ | P0 | https://kb.nomachine.com/?p=838 |
| 连接时询问分辨率 | 每次连接询问 resize 方式 | P2 | https://www.nomachine.com/changing-how-you-view-the-remote-host |
| HiDPI/Retina、颜色深度 | 没有系统说明（待核实） | P1 | — |

---

## 3. 网络传输与协议

### 3.1 NX 协议架构

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| 多路复用加流控 | v4 起 NX 是通用隧道，带帧和流控信息，多通道复用，可同时走 TCP 和 UDP；音频用 Opus（回退 Vorbis） | P0 | https://kb.nomachine.com/?p=761 |
| 流量优先级 | 交互类流量优先，大块数据切片后中间插入交互消息 | P1 | https://www.linuxjournal.com/node/8483 |
| nxproxy 双端代理（NX3） | 两端各一个代理，远端对应用伪装成本地 X server，X11 往返留在本地；两端缓存同步 | P1 | https://www.linuxjournal.com/node/8489 |
| X 协议差分压缩与消息缓存（NX3） | 命中缓存只发引用，命中率 60–80%，压缩率比 ZLIB 高约 10 倍 | P1 | https://www.linuxjournal.com/node/8483 |
| 往返抑制（NX3） | 在本地生成部分回复，Mozilla 启动从约 6000 次往返降到十几次 | P1 | https://www.linuxjournal.com/node/8483 |
| 持久缓存（NX3） | `cache=` / `images=` 设缓存大小，跨会话保存 | P2 | https://manpages.debian.org/testing/nxproxy/nxproxy.1 |

### 3.2 传输、带宽与端口

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| TCP 主通道 | nxd 监听 TCP 4000；SSH 模式下 Linux/mac 用系统 sshd（22），Windows 用 nxsshd（4022）；Web 用 4443 | P0 | https://kb.nomachine.com/?p=780 |
| UDP 承载音视频 | 只有音视频走 UDP，其余走 TCP；v8 起 UDP 也用 4000（`NXUDPPort`），协商失败回退 TCP；SSH 模式下禁用 UDP | P1 | https://kb.nomachine.com/?p=11048 , https://kb.nomachine.com/?p=780 |
| UDP 丢包恢复与加密 | 自己做重传和差错纠正；UDP 用 Blowfish 加密，密钥在 TLS 通道协商并定期更换 | P1 | https://kb.nomachine.com/?p=11048 |
| 按数据类型动态选传输 | 按数据类型和网络状况选 TCP/UDP；官方建议局域网可关闭 UDP | P1 | https://kb.nomachine.com/?p=11048 , https://forum.nomachine.com/?p=26694 |
| 带宽自适应 | 按带宽、延迟和编解码耗时调整帧率和画质；拥塞控制算法未公开（待核实） | P0 | https://kb.nomachine.com/?p=761 |
| 链路速度档位（NX3） | `link=modem/isdn/adsl/wan/lan` 决定压缩级别，`limit=` 限码率 | P2 | https://manpages.debian.org/testing/nxproxy/nxproxy.1 |
| 会话断开与恢复 | 断开后会话保留，重连回到原会话；从另一台客户端连接时会话迁移过去 | P0 | https://kb.nomachine.com/?p=711 |
| 自动重连 | v6 起断网或休眠后自动重试（客户端 UI 需开着），v10 有改进 | P1 | https://kb.nomachine.com/?p=711 , https://nomachine.com/what-s-new-in-nomachine |

---

## 4. 音频

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| 远程音频回传 | 默认开启，可在会话菜单 Audio 面板关闭 | P0 | https://kb.nomachine.com/?p=12396 |
| 音频编码 | Opus，回退 Vorbis；采样率和单独的音频质量档位未公开（待核实） | P1 | https://kb.nomachine.com/?p=761 |
| 服务端音频后端 | node.cfg 的 `AudioInterface`：Linux 可选 pulseaudio、alsa、pipewire（8.2.3 起，仅物理桌面），Mac/Windows 用自研 nxaudio；`disabled` 同时关音频和麦克风 | P0 | https://kb.nomachine.com/?p=12386 , https://kb.nomachine.com/?p=11111 |
| 远程主机静音 | 连物理桌面时让远端本地不出声 | P1 | https://kb.nomachine.com/?p=12396 |
| 麦克风转发 | 默认关闭，开启后远端 VoIP 程序可以用客户端麦克风 | P1 | https://kb.nomachine.com/?p=12396 |
| 本地设备选择 | 只用系统默认的输入输出设备，不能在客户端里选（论坛 2020） | P2 | https://forum.nomachine.com/?p=26234 |
| 音视频同步 | 没找到官方说明（待核实） | P1 | — |

---

## 5. 输入

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| 键盘鼠标转发 | 核心能力 | P0 | https://kb.nomachine.com/?p=11111 |
| 键盘布局同步 | 没找到官方说明（待核实） | P0 | — |
| 抓取键盘（Grab keyboard） | Alt/Cmd+Tab 等系统快捷键发往远端；macOS Sonoma 上有已知问题 | P1 | https://kb.nomachine.com/?p=11896 , https://kb.nomachine.com/?p=12396 |
| 抓取鼠标 | 鼠标锁在会话窗口里，用热键或翻页角呼出菜单 | P1 | https://kb.nomachine.com/?p=12345 |
| 移动端触控手势 | 手势转成鼠标事件，不转发真实多点触控 | P1 | https://nomachine.com/getting-started-with-nomachine-for-ios , https://forum.nomachine.com/?p=36573 |
| IME 输入法 | 没找到官方说明（待核实） | P1 | — |
| 鼠标按键模拟 | 左右键同按模拟中键，Mac 上 Ctrl+点击模拟右键 | P2 | https://kb.nomachine.com/?p=12345 |
| 数位板压感 | 默认当普通指针，不支持压感；走 USB 转发后压感仍有问题 | P2 | https://forum.nomachine.com/?p=43801 |
| 手柄 | 没有专门功能，只能走通用 USB 转发（待核实） | P2 | https://kb.nomachine.com/?p=12345 |

---

## 6. 剪贴板与文件传输

### 6.1 剪贴板

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| 双向复制粘贴 | 核心能力 | P0 | https://kb.nomachine.com/?p=11111 |
| 方向控制 | server.cfg 的 `EnableClipboard`：none / server / client / both | P1 | https://kb.nomachine.com/?p=12386 , https://kb.nomachine.com/?p=1617 |
| 大小限制 | `ServerClipboardLimit` / `ClientClipboardLimit`（字节，0 为不限） | P1 | https://kb.nomachine.com/?p=12386 |
| 富文本和图片 | 支持的格式没有官方说明（待核实） | P1 | — |
| 按用户/组设策略 | Terminal Server 上用 `nxserver --ruleadd` 设规则，仅企业版 | P2 | https://kb.nomachine.com/?p=1617 |
| Web 会话虚拟剪贴板 | 浏览器限制下用中转面板 | P2 | https://forum.nomachine.com/?p=48209 |

### 6.2 文件传输

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| 上传和下载 | 会话菜单选"Transfer a file"，用文件浏览器选文件和目录 | P0 | https://www.nomachine.com/sharing-files-between-computers |
| 传输状态窗口 | 托盘图标里"Show transfer status"，两端都能看 | P1 | https://www.nomachine.com/sharing-files-between-computers |
| 拖放 | 可以从共享磁盘拖出文件；客户端和会话间直接拖放的细节（待核实）；Wayland 下不支持 | P1 | https://kb.nomachine.com/?p=12396 , https://kb.nomachine.com/?p=948 |
| 权限策略 | node.cfg 的 `EnableFileTransfer`（none/server/client/both），以被连主机的配置为准；上传下载可分别控制 | P1 | https://kb.nomachine.com/?p=12386 , https://kb.nomachine.com/?p=1532 |

---

## 7. 设备转发

所有设备类转发（磁盘、打印机、USB、网络端口）都用同一套 none / server / client / both 方向控制。

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| 磁盘/文件夹共享 | 本地文件夹导出到远端，或远端文件夹导入本地，可设成每次自动挂载 | P1 | https://kb.nomachine.com/?p=12396 , https://kb.nomachine.com/?p=12345 |
| 打印机共享 | 双向；Linux/Mac 基于 CUPS；服务端先把打印任务转成临时 PDF 再发打印机 | P1 | https://kb.nomachine.com/?p=12396 , https://kb.nomachine.com/?p=11906 |
| 任意 USB 设备转发 | 设备从本地断开、独占给远端；Linux 服务端靠 nxusb 内核模块，macOS 要批准系统扩展 | P1 | https://kb.nomachine.com/?p=12396 , https://kb.nomachine.com/?p=12341 |
| 摄像头 | 没有专门通道，走 USB 转发，macOS 客户端有已知问题 | P2 | https://forum.nomachine.com/?p=39135 |
| 网络端口转发 | 两机之间隧道转发 TCP/UDP 端口，内置 CUPS、HTTP、FTP、SSH 预设 | P2 | https://kb.nomachine.com/?p=12396 |
| 智能卡转发 | 读卡器给远端程序用，需指定 PKCS#11 库；开关 `EnableSmartcardSharing` | P2 | https://kb.nomachine.com/?p=12396 , https://kb.nomachine.com/?p=12386 |

---

## 8. 会话模型

| 功能 | 说明 | 重要度 | 许可 | 来源 |
|---|---|---|---|---|
| 连接物理桌面 | 接入本机真实显示器，多人连接时看到同一个桌面 | P0 | 个人版、ED | https://kb.nomachine.com/?p=732 |
| headless 服务器 | 找不到本地 X server 时用内置 X server 提供桌面 | P0 | 个人版起 | https://kb.nomachine.com/?p=442 |
| 会话挂起与恢复 | 虚拟桌面断开后后台继续运行 | P0 | TS 家族 | https://kb.nomachine.com/?p=732 |
| Linux 多用户虚拟桌面 | 每个用户一个独立 Linux 桌面，Windows/Mac 上没有 | P1 | 仅 TS 家族 | https://kb.nomachine.com/?p=732 |
| 物理桌面并发连接 | 个人版 1 个桌面连接（另有 4 个终端连接），ED 不限 | P1 | — | https://www.nomachine.com/everybody/compare-features |
| 访客桌面共享 | 主人在场时让别人接入，不必给系统密码，主人决定可操作还是只能看 | P1 | 个人版可用 | https://kb.nomachine.com/?p=736 |
| 多人协作观看 | 多人同时共享一个桌面 | P1 | ED/企业版 | https://www.nomachine.com/feature_comparison |
| 虚拟桌面并发上限 | Workstation 4 个、SBTS 10 个、TS 不限 | P2 | TS 家族 | https://kb.nomachine.com/?p=732 |
| 单应用浮动窗口（rootless） | "Custom session" 跑单个 Linux 应用 | P2 | TS 家族 | https://kb.nomachine.com/?p=702 |
| 会话录制 | Ctrl-Alt-R 录制为 .nxr，用 Player 回放；管理员可要求自动录制（适用版本待核实） | P2 | 全部 | https://www.nomachine.com/recording-the-remote-or-local-desktop-with-nomachine |
| 聊天/白板 | 对比页列出，机制没有文档（待核实） | P2 | 待核实 | https://www.nomachine.com/everybody/compare-features |
| 远程终端 | v10 起可以只开 shell，并自带 SSH server | P2 | v10 | https://nomachine.com/what-s-new-in-nomachine |

---

## 9. 认证与安全

| 功能 | 说明 | 重要度 | 许可 | 来源 |
|---|---|---|---|---|
| 系统账户密码登录（PAM） | 用操作系统账户认证 | P0 | 全部 | https://kb.nomachine.com/?p=1554 |
| 传输加密 | OpenSSL TLS 1.2，默认 ECDHE-RSA-AES128-GCM-SHA256；UDP 用 Blowfish；始终加密 | P0 | 全部 | https://kb.nomachine.com/?p=738 |
| 接入审批弹窗 | `PhysicalDesktopAuthorization` / `VirtualDesktopAuthorization`：0 不询问，1 询问主人（默认，受信用户免询问），2 只有受信用户免询问 | P0 | 全部 | https://kb.nomachine.com/?p=690 |
| NX 协议登录 | 端口 4000，支持密码、SSH 私钥、Kerberos 票据 | P0 | 全部 | https://kb.nomachine.com/?p=1554 |
| SSH 协议模式 | 端口 22/4022，可对接系统 sshd 的任意认证方式，支持 agent 转发 | P1 | 仅企业版 | https://kb.nomachine.com/?p=1554 |
| 密钥认证 | NX 和 SSH 两种模式都支持 SSH 私钥 | P1 | 全部 | https://kb.nomachine.com/?p=1554 |
| 双因素认证 | 手机 App 推送审批，有恢复码；文档没提 TOTP | P1 | 全产品线 | https://www.nomachine.com/everybody/two-factor-authentication |
| 只看/可操作、锁屏、设备策略 | 限制只看、会话期间锁本地屏幕、按方向开关各类转发 | P1 | 全部 | https://kb.nomachine.com/?p=1532 |
| 谁能批准接入 | v9 的 `PhysicalDesktopAcceptUsers` / `VirtualDesktopAcceptUsers` | P1 | v9+ | https://kb.nomachine.com/?p=12233 |
| 受信用户与用户管理 | `nxserver --useradd <user> --system --trusted` | P1 | 服务器产品 | https://kb.nomachine.com/?p=690 |
| 服务器策略配置 | server.cfg 管全局、node.cfg 管节点；profile rules 优先级更高 | P1 | — | https://kb.nomachine.com/?p=870 |
| Kerberos / AD / SSO | Kerberos 票据转发；v10 新增 OIDC（Entra ID、Okta、Keycloak、Google） | P2 | 企业版 | https://kb.nomachine.com/?p=1554 , https://nomachine.com/what-s-new-in-nomachine |
| 智能卡登录 | 智能卡里的 SSH 私钥用于 SSH 模式登录 | P2 | 企业版 | https://kb.nomachine.com/?p=1554 |
| 审计与连接历史 | `nxserver --history`；没有 syslog/SIEM 集成的官方答复 | P2 | 服务器产品 | https://forum.nomachine.com/?p=49746 |

---

## 10. 网络穿透与发现

| 功能 | 说明 | 重要度 | 许可 | 来源 |
|---|---|---|---|---|
| 手动端口转发 / DDNS | 自动映射失败时手动配置；双重 NAT 两台路由都要配 | P0 | 全部 | https://kb.nomachine.com/?p=10083 |
| 局域网自动发现 | 服务器广播，客户端 Machines 面板列出；v9 键 `EnableLocalNetworkBroadcast`；协议可能是 mDNS（UDP 5353，待核实） | P1 | 全部 | https://www.nomachine.com/finding-other-nomachine-computers-on-the-same-network , https://kb.nomachine.com/?p=780 |
| UPnP / NAT-PMP 自动映射 | v8 起默认关闭；`EnableUPnP NXTCP,NXUDP,SSH,HTTP`；`nxserver --upnpstatus/--upnpmap` | P1 | 全部 | https://kb.nomachine.com/?p=10083 |
| 按 Machine Id 连接（NoMachine Network） | 固定 Machine Id，无需公网 IP 或端口转发；地址簿在云端；v9 起官方推荐 | P1 | 发布免费，按 Id 连接需订阅（8.50 美元/月起） | https://nomachine.com/network |
| 客户端代理 | HTTP 和 SOCKS，可自动探测系统代理 | P1 | 全部 | https://kb.nomachine.com/?p=10218 |
| Network 的打洞/中继机制 | 官方没说直连还是中继（待核实） | — | — | https://nomachine.com/network |
| Cloud Server 网关 | 统一入口，把用户分派到节点，到节点走 SSL 隧道 | P2 | CS 家族 | https://kb.nomachine.com/?p=12417 |
| 通信中枢 | v9 `EnableCommunicationHub`，让 Cloud Server 中转所有流量 | P2 | CS 家族 | https://kb.nomachine.com/?p=12233 |

---

## 11. 跨平台支持

| 平台 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| Windows | Win 7 到 11、Server 2008 到 2022，32/64 位 | P0 | https://kb.nomachine.com/?p=12342 |
| macOS | Intel 10.9 起，Apple Silicon 11 到 15 | P0 | https://kb.nomachine.com/?p=12342 |
| Linux | RHEL、Fedora、SLES、openSUSE、Debian、Ubuntu 等，rpm/deb/tar.gz | P0 | https://kb.nomachine.com/?p=12342 |
| ARM / 树莓派 | 客户端支持 ARMv7/v8（Pi 2 到 Pi 5）；ARM 服务端矩阵（待核实） | P1 | https://kb.nomachine.com/?p=12373 |
| iOS / Android 客户端 | NoMachine for Mobile；不支持硬件解码 | P1 | https://kb.nomachine.com/?p=12373 |
| Web 客户端 | 服务端（免费版除外）跑 nxhtd（4443），浏览器免安装访问；默认 MJPEG，支持 WebRTC 时用 VP8/H.264；不支持 USB、磁盘/打印机、VoIP、智能卡 | P1 | https://kb.nomachine.com/?p=12345 , https://kb.nomachine.com/?p=12401 |
| Wayland 限制 | 不支持 headless、断开后不能自动锁屏、不能拖放、可能按键重复 | P1 | https://kb.nomachine.com/?p=948 |

---

## 12. 客户端体验

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| Machines 连接列表 | 首屏列出已保存连接、局域网发现的主机和最近连接 | P0 | https://kb.nomachine.com/?p=12373 |
| 显示模式 | Viewport、Scale to window、Resize remote display、全屏、跨所有显示器全屏 | P0 | https://kb.nomachine.com/?p=12401 |
| 连接文件 | .nxs（客户端）和 .nxw（浏览器），方便分发 | P1 | https://kb.nomachine.com/?p=12373 |
| 会话内菜单 | Ctrl+Alt+0 或屏幕角落翻页角打开；面板有 Input、Devices、Display、Audio、Mic in、Recording、Connection | P1 | https://kb.nomachine.com/?p=12401 |
| 画质与分辨率设置 | 质量滑块、分辨率、匹配客户端分辨率、关闭自适应和多遍编码、显示当前编码器 | P1 | https://kb.nomachine.com/?p=12401 |
| 多显示器选择 | Ctrl+Alt+I 标出编号，Ctrl+Alt+A 显示全部，Ctrl+Alt+<编号> 切换 | P1 | https://kb.nomachine.com/?p=12373 |
| 性能统计与诊断 | Connection 面板显示流量统计；"Take logs" 一键打包日志 | P1 | https://kb.nomachine.com/?p=12401 |
| 热键 | Ctrl+Alt+0 菜单、Ctrl+Alt+T 断开，可在 Input→Shortcuts 改或整体关闭 | P1 | https://kb.nomachine.com/?p=12401 |
| Web 版虚拟键盘 | 16 种布局 | P1 | https://kb.nomachine.com/?p=12401 |

---

## 13. 管理与部署

| 功能 | 说明 | 重要度 | 来源 |
|---|---|---|---|
| 配置文件 | /usr/NX/etc/server.cfg（服务）和 node.cfg（节点与会话） | P0 | https://kb.nomachine.com/?p=12360 |
| 服务控制 CLI | `nxserver --status/--restart/--stop/--start/--startmode` | P0 | https://kb.nomachine.com/?p=12360 |
| 日志与诊断 | `nxserver --debug --enable`、`--debug --collect` 打包日志、`--logrotate`；可输出到 syslog/journald | P0 | https://kb.nomachine.com/?p=1198 |
| 会话管理 CLI | `--list`、`--history`、`--disconnect`、`--terminate` | P1 | https://kb.nomachine.com/?p=12360 |
| 用户管理 CLI | `--useradd/--userdel/--userenable/--userlist/--passwd`，可设受信用户和管理员 | P1 | https://kb.nomachine.com/?p=12360 |
| GUI 管理 | Server 设置界面；Cloud Server 有节点面板和拓扑树 | P1 | https://kb.nomachine.com/?p=12345 |
| 静默安装 | Windows `/silent` `/verysilent`，可关 USB/打印驱动；Linux 包管理器或 `nxserver --install` | P1 | https://kb.nomachine.com/?p=712 |
| 更新机制 | 默认每两天检查，可后台下载，安装需确认；`UpdateFrequency 0` 关闭 | P1 | https://kb.nomachine.com/?p=12342 |
| 集群与负载均衡 | Enterprise Terminal Server 多节点分配虚拟桌面；Cluster 版 active-active 故障转移 | P2 | https://kb.nomachine.com/?p=12423 |
| 事件脚本 | 服务器或节点事件发生时执行自定义脚本 | P2 | https://www.nomachine.com/nomachine-features |

---

## 14. 产品线与授权

| 产品 | 定位与关键差异 | 来源 |
|---|---|---|
| Personal Edition（原免费版） | 只访问物理桌面，1 个桌面连接（另有 4 个终端连接），无 SSH、无虚拟桌面，每年 24.5 美元起 | https://www.nomachine.com/nomachine-features , https://kb.nomachine.com/?p=717 |
| NoMachine Player（原 Enterprise Client） | 只有客户端，不用激活；按 IP 连接不限，按 Machine Id 连接需订阅 | https://kb.nomachine.com/?p=13398 |
| Enterprise Desktop | 物理桌面，并发连接不限，每年 44.5 美元起 | https://www.nomachine.com/feature_comparison |
| Workstation | 单台 Linux 最多 4 个虚拟桌面 | https://www.nomachine.com/terminal-server |
| Small Business Terminal Server | 最多 10 个虚拟桌面 | https://www.nomachine.com/terminal-server |
| Terminal Server | 虚拟桌面、用户、连接都不限 | https://www.nomachine.com/terminal-server |
| Enterprise Terminal Server (+Node / Cluster) | 多节点池加负载均衡，Cluster 版加 HA | https://kb.nomachine.com/?p=12423 |
| Workgroup / Cloud Server / Enterprise Cloud Server | 集中网关；WCS 10 个并发用户，CS 页面上 20 和 40 两种说法（待核实），ECS 不限且支持级联 | https://kb.nomachine.com/?p=12417 |

授权要点：企业服务端产品按虚拟桌面数计费，不按连接数；Cloud Server 体系里每个节点要另买授权。**收费分水岭在两处：多用户 Linux 虚拟桌面，和按 ID 跨 NAT 连接。**

---

## 15. 对 rdmachine 的启示

1. **画质闭环是核心。** NoMachine 的流畅感来自"目标画质 + 按编解码耗时、RTT、带宽实时调整帧率和量化 + 静止后多遍精修"。精修还能补 H.264 4:2:0 造成的文字发虚和偏色。应列为 P0。
2. **编码层和协议解耦，硬件优先、软件兜底。** 至少 H.264（NVENC / QSV / VA-API / VideoToolbox，回退 openh264 或 x264），再加一个 JPEG/无损类编码给弱客户端和静态内容。会话面板要显示当前编码器，排障成本低很多。
3. **传输：TCP/TLS 控制通道 + 可选 UDP 媒体通道，单端口复用。** UDP 不要照搬 Blowfish，改用 DTLS 或 QUIC；UDP 不通时无缝回退 TCP；局域网允许关闭 UDP。
4. **MVP 先做"物理桌面 + headless 自建虚拟显示 + 主人审批"。** 审批按 0/1/2 三档加受信用户白名单，配只看/可操作权限。这是 NoMachine 入门档的全部核心体验。
5. **会话持久化是区别于 VNC 的体验点。** 断开不销毁、自动重连、跨客户端迁移。Linux 多用户可挂起虚拟桌面（Xvfb/Xorg-dummy 或 Wayland headless compositor）是 NoMachine 收费的分水岭，开源做到就是明显优势。
6. **穿透分层做，ID 连接做成可自托管。** 局域网 mDNS 发现 → UPnP/NAT-PMP（默认关）→ 自建 ID 注册加中继（参考 rustdesk 的 hbbs/hbbr）。NoMachine 把按 ID 连接做成订阅，这是开源最容易形成的差异点。
7. **统一的方向权限模型。** 剪贴板、文件、磁盘、打印、USB、端口都用 none/server/client/both，加大小限制。命名上要把方向定义清楚，NoMachine 自己的文档在这点上前后不一致。
8. **设备转发按类别做，通用 USB 放后期。** 通用 USB 依赖内核驱动，维护代价高，问题也多。先做：摄像头走视频流、打印走 PDF/IPP、磁盘走 FUSE/SMB、智能卡走 PC/SC，通用 USB/IP 列为 P2。
9. **NoMachine 没做好的地方可以反超。** 不能选音频设备、不支持压感和手柄、IME 无说明、Wayland headless 不可用、审计没有结构化日志。
10. **运维接口一开始就定形。** 一个 `nxserver` 式的单一管理 CLI（status / list / history / restart / debug collect），server.cfg + node.cfg 两层配置，结构化审计日志（JSON 或 syslog）。数据模型预留节点 ID，以后扩展集群不用重构。
11. **NX3 时代的 X 协议差分缓存和往返抑制，现在价值有限。** 现代桌面以 GPU 合成为主，主路径应是"帧采集 + 视频编码"。X11 矢量模式可以作为低带宽场景的后续选项（P2）。

---

## 16. 待核实与矛盾点

- **文件传输大小限制**：一处官方页面说"没有文件大小限制"，另一份管理文档提到默认上限 100MB（https://www.nomachine.com/sharing-files-between-computers 与 https://kb.nomachine.com/?p=1532）。
- **Cloud Server 并发用户**：同一页面表格写 40，正文写 20（https://kb.nomachine.com/?p=12417）。
- **断线自动重连在各版本的可用性**：对比页列出，但没能确认适用范围。
- **NoMachine Network 是 P2P 打洞还是中继**：官方未说明。
- **局域网发现协议**：端口文档列出 mDNS（UDP 5353），广播功能文档未说明协议。
- **macOS 采集与编码 API、光标通道、视频区域检测、HiDPI、颜色深度、IME、键盘布局同步、剪贴板格式、音视频同步**：均无官方说明。
