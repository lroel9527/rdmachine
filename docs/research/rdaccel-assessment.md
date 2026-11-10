# rdaccel 能力评估：作为 rdmachine 底座能做到什么

> 评估对象：[lroel9527/rdaccel](https://github.com/lroel9527/rdaccel) v1.0.0（提交 `89e0ee0`，2026-11-05），约 3.2 万行，176 个文件。
> 方法：按采集/输入、编码、传输/安全、公开 API/工程化四块分别通读代码，结论以代码为准、文档为辅。文中路径都相对 rdaccel 仓库根目录。
> 用途：给“NoMachine 功能清单 × rdaccel 覆盖度”对照表提供 rdaccel 一侧的事实。

## 一句话结论

rdaccel 是一个做得很扎实的**单路、点对点、低带宽画面加输入引擎**：画面分层编码、UDP 拥塞控制、独立光标通道、加密和稳定 C ABI 都是产品级的。但它刻意只做“画面 + 输入 + 一条消息通道”，类 NoMachine 产品需要的**会话管理、身份认证与密钥协商、NAT 穿透/中继、多观看者、音频、文件传输、Windows、Wayland 输入、Linux 硬解**都要 rdmachine 自己补，或者回头改 rdaccel。

## 总览：远程桌面各模块的覆盖度

图例：✅ 能直接用　🟡 部分支持，需要补　❌ 没有，需要 rdmachine 或改库来做

| 模块 | 状态 | 说明 |
| --- | --- | --- |
| 屏幕采集 macOS | ✅ | ScreenCaptureKit，系统脏区，HiDPI 物理像素，默认 60 fps |
| 屏幕采集 Linux X11 | 🟡 | MIT-SHM 定时轮询，**没有 XDamage 脏区**；分辨率变化后采集停止不重建 |
| 屏幕采集 Wayland | 🟡 | portal + PipeWire，带合成器脏区、能跟随分辨率变化；**只能单屏、要用户在弹窗里选** |
| 多显示器 | 🟡 | 能列举、能选一块采；**一个 Host 只采一块**，多屏同时要开多个 Host |
| 虚拟显示器 / 无头 | 🟡 | 只有 X11 下借 Xvfb；没有创建虚拟屏、新建桌面会话的能力（macOS 的虚拟屏由 rdmachine 自己建，见[协议](../protocol.md)的“请求桌面”） |
| 登录界面 / 锁屏 | ❌ | 三个平台都没有相关代码 |
| 分层编码（平移/缓存/无损/视频） | ✅ | 核心卖点，静止几十 kbps、文字无损 |
| 视频编码 macOS | ✅ | VideoToolbox HEVC，低延迟硬件优先，可落到软件 |
| 视频编码 Linux | 🟡 | NVENC 优先，否则 x265（GPL、CPU 重）；VAAPI 实验性 |
| 视频解码 | 🟡 | macOS 硬解；**Linux 只有 FFmpeg 软解** |
| H.264 / AV1 回退 | ❌ | 只支持 HEVC，设计上明确不做 |
| 4:4:4 / 10bit | ❌ | 一律 4:2:0 8bit，多色抗锯齿文字靠精修兜底 |
| 自适应码率 / 弱网 | ✅ | 0.4 Mbps 可用，延迟有上限 |
| 光标 | ✅ | 独立高优先级通道，形状缓存，接收端自己画 |
| 键盘鼠标注入 macOS | ✅ | CGEvent，含拖拽、双击、文字注入、输入法临时切换 |
| 键盘鼠标注入 X11 | ✅ | XTest，文字可借空闲 keycode 输入任意字符 |
| 输入注入 Wayland | ❌ | 没有 libei / portal RemoteDesktop |
| 触控 / 手写笔 / 手柄 / 媒体键 | ❌ | 事件模型里没有 |
| 剪贴板 | 🟡 | 只有文本，库只负责传，读写系统剪贴板归应用 |
| 文件传输 | ❌ | 消息通道单条上限 16 MiB、可靠有序、会队头阻塞，需要另设计 |
| 音频 | ❌ | 不采集不播放，也没有不可靠/实时通道 |
| 打印 / USB / 驱动器映射 | ❌ | 只能自己在消息通道上复用 |
| 传输 UDP / TCP / 自定义 | ✅ | 自带类 BBR 的可靠 UDP，6 个通道互不阻塞；可接 QUIC/WebRTC/中继 |
| NAT 穿透 / 中继 / 信令 | ❌ | 只有 `udp_from_fd` 接收应用打好洞的 socket |
| 断线重连 / 会话恢复 | ❌ | 断开是终态，只能重新建连接并从关键帧开始 |
| 连接迁移（换网络） | ❌ | UDP 收到首包后 `connect()` 死对端地址，没有连接 ID |
| 加密 | 🟡 | XChaCha20-Poly1305，防重放；但密钥是**预共享**，**没有前向保密**和公钥身份 |
| 认证 / 账号 | 🟡 | 只有 `on_auth(token)` 钩子，账号、配对、权限归应用 |
| 多观看者 / 共享协作 | ❌ | 同一时刻一个 Viewer，`keep_serving` 只是一个走了再接下一个 |
| 录制回放 | ✅ | `.rdstream` 码流录制，可跨平台回放 |
| Windows | ❌ | CMake 在非 macOS/Linux 直接报错 |
| 客户端界面 | 🟡 | 只有 Mac 窗口控制端示例 `rdaccel_remote`；Linux 没有窗口化 viewer |

## 1. 采集、光标与输入

### 采集

- **macOS ScreenCaptureKit**（`platform/mac/capture_sck.mm`）：用 SCK 自带脏区并换算到像素（:84-101），空闲帧丢弃；帧率由 `minimumFrameInterval` 控制，默认 60（:174）；按 `pointPixelScale` 取物理像素，也可以指定输出宽高让 SCK 缩放（:168-172）；固定 32BGRA，关闭音频（:177-178），默认排除本进程窗口。多屏列表去掉镜像屏、主屏排第一（`displays_mac.mm:246-263`）。**分辨率变化和热插拔不处理**，输出尺寸在 Start 时定死。
- **X11 MIT-SHM**（`platform/linux/capture_x11.cpp`）：按固定 fps 轮询，**没有脏区**，整帧交给分块比对；XDamage 写着 TODO（`capture_x11.h:5-6`）。只支持 24/32 位深，swscale 缩放；多屏走 RandR 1.5。分辨率变化后 SHM 尺寸不对，连续失败 1 秒就上报停止（:128-131）。可以跑在 Xvfb 上。
- **Wayland portal + PipeWire**（`platform/linux/capture_pipewire.cpp`）：只用 ScreenCast 接口，`multiple=FALSE`，`display_index` 必须是 0（:322-323, :858-859），分享哪块屏由系统弹窗决定；`restore_token` 可持久化以免每次弹窗。脏区来自 `SPA_META_VideoDamage`，支持 8 种 RGB 排列，帧率协商上限 360；只用普通内存缓冲，没有 DMA-BUF。分辨率变化能自动跟上，Host 会重建编码器（`src/host.cpp:413-433`）。
- **静止补帧**：SCK 和 PipeWire 静止时不出帧，Host 的 idle 线程每 100 ms 补交一帧（`src/host.cpp:140-175`）。
- **应用自己喂帧**：`submit_frame` 加脏区，rdmachine 可以用自己的采集（比如将来的 Windows DXGI、虚拟显示器）接进来。

### 光标

Host 按 `cursor_poll_hz`（默认 120）采样，图像随采集缩放（`src/host.cpp:107-134`）。X11 用 XFixes（形状最大 256×256，`cursor_xfixes.cpp:299-341`）；macOS 用 `NSCursor currentSystemCursor`，每 4 次采样比对一次像素，**应用主动隐藏光标检测不到**（`cursor_mac.mm:384`）；Wayland 优先 metadata 模式，否则合成器直接画进画面。

### 输入

- 事件（`core/rdacore/input.h:7-30`）：鼠标移动、5 个鼠标键、滚轮（按格，没有像素级平滑滚动）、按键、单个 Unicode 字符的文字事件、修饰键位。**没有触控、手写笔、手柄。**
- 键码：线上用 USB HID page 0x07，静态表映射到 mac kVK 和 evdev（`src/keycodes.cpp:17-149`），覆盖 F1-F24、小键盘、ISO/JIS、Lang1-5；**没有 Consumer 页**（音量、媒体键）。
- 坐标按 `source_width/height` 映射到目标屏并 clamp；一个注入器绑一块屏。
- X11 XTest（`inject_xtest.cpp`）：文字先在当前布局里找键，找不到就临时改映射一个空闲 keycode（:151-202）；析构时释放按住的键。
- macOS CGEvent（`inject_mac.mm`）：需要辅助功能权限并预检（:326-330）；能产生拖拽、识别双击三击；文字用 `CGEventKeyboardSetUnicodeString`，遇到拼音等输入法会临时切到 ASCII 布局，1 秒后切回（:88-130）。
- **Wayland 下没有注入**，`CreateInputInjector` 只走 XOpenDisplay + XTest（:226-249）。
- Host 只把输入事件回调给应用，注入器要应用自己创建（`src/capi/capi.cpp:390`）。这对 rdmachine 是好事：权限控制、只读观看模式都可以在应用层做。

## 2. 画面编码管线

每帧切 64×64 块，按代价从低到高分四条路：

- **变化检测**：只看脏区碰到的块，FNV-1a + splitmix 哈希（`core/tile/tile_diff.cpp:6-12`）。注释提到的 Metal GPU 版本并不存在，全部是 CPU 单线程。
- **平移**（滚动、拖窗）：变化块不少于 6 个才检测；每块取细节最多的一行做滚动哈希探针，在上一帧里投票出位移，取前 3 个候选逐块精确比对；边缘块按 4 px 竖条再拆（`core/tile/motion.cpp`）。拷贝太碎时整帧放弃平移（`host_session.cpp:429-437`）。
- **块缓存**：接收端 1024 槽，Host 维护 LRU 索引并下指令，两边状态一致。稳定 8 帧以上或同内容出现 2 次以上的块会存（`host_session.h:165-168`）。
- **调色板无损**：不超过 256 色的块（每帧都变的“热”块只许 4 色）编成调色板 + 1/2/4/8 bit 索引，所有块和拷贝/缓存指令共用一条跨帧 deflate 流（level 6，关键帧重置）。预算是令牌桶，画面在动时只给 20% 码率，静止补全时 80%（`host_session.h:218-239`）。
- **HEVC 整屏**：剩下的块进硬件编码器；无损块在编码输入里抹成底色。精修 `refine_frames=8`。
- **视频区域**（`core/tile/video_region.h:28-37`）：30 帧窗口里 20 帧以上忙的块、连通且至少 2×2、连续 3 帧后确认，最多 2 个区域，各开一路 HEVC，QP 下限 27，码率按面积分（`host_session.cpp:992-1022`）。30 帧不变就关闭，暂停画面会再走一次清晰路径。
- **渐进首帧**：关键帧只发视频版，文字块随后按码率补成无损。

### 编解码器

| | 编码 | 解码 |
| --- | --- | --- |
| macOS | VideoToolbox HEVC：低延迟硬件 → 硬件 → 任意（可软件）；RealTime、无重排、Main 8bit、BT.709、不发周期关键帧（`video_vt.mm:54-103`） | VTDecompressionSession 硬解，输出 BGRA |
| Linux | FFmpeg：`hevc_nvenc`（p1/ull）→ `libx265`（ultrafast/zerolatency）；`hevc_vaapi` 实验性需手动指定（`video_ffmpeg.cpp:423-426`）；无 B 帧、按需 IDR、VBV 约 0.5 s | **FFmpeg 软解**，切片线程（`video_ffmpeg.cpp:296-304`） |

色彩一律 YUV420 / NV12、8bit、BT.709 limited。x265 中途改码率要重开编码器（代价一个关键帧），所以变化超过 1.3 倍才重开。编码器创建失败时 Host 停止并报 `RDA_ERR_CODEC`，没有降级到纯无损的模式。

### 码率与帧率

默认 60 fps；`bitrate_bps` 默认 8 Mbps（自适应时是上限），`min_bitrate_bps` 300 kbps，`max_queue_ms` 50，`min_qp` 24。发送队列排空时间超过 50 ms 就跳帧并把变化合并到下一帧；拥塞时目标码率立刻降到估计带宽的 85%，畅通时每 250 ms 最多升 25%；RTT 抬高也当作排队；编码器超发会按比例压。`capture_width/height` 可降采集分辨率。

### 实测数字（摘自 `docs/benchmarks.md`）

| 场景 | 数字 |
| --- | --- |
| Mac 1408×880 空闲 | 338 → **17 kbps**（相对整屏硬编基线） |
| Mac 滚动 | 1592 → 736 kbps，PSNR 39.2 → 46.1 dB |
| Mac 视频窗口 | 2557 → 1240 kbps |
| Mac 采集到显示 | 约 10 ms |
| Linux x265 1080p 视频窗口，开视频区域 | 4652 → 1764 kbps，编码 16.5 → 6.0 ms，CPU 252% → 90% |
| 模拟 20 Mbps | 延迟 p50/p95 = 15/21 ms |
| 模拟 1.5 Mbps | 自适应开 42/65 ms；关掉是 3～6 秒且越积越多 |
| Mac 0.4 Mbps | 中位延迟约 100～140 ms |
| 光标通道 | 码率 544 → 22 kbps，CPU 179% → 13% |
| 1.5 Mbps 渐进首帧 | 首次出画 3.1 → 1.4 s，完全清晰约 4.7 s |

**没有的数据**：4K、高帧率（120+）、多路并发、硬件编码下的 CPU 占用。

## 3. 传输、连接与安全

### 协议与连接层

- 6 个逻辑通道：control、input、surface、video、cursor、message（`core/rdacore/transport.h:24-33`）。消息帧 `[type u8][len varint][payload]`，16 种消息，单条上限 64 MiB。
- 协议版本 4（Host 最低兼容 3，Viewer 最低 4），能力位只有 `kCapMessages`、`kCapTextInput`；未知消息和多余尾字段会跳过，**便于向前扩展**（`core/proto/messages.cpp:378,432`）。
- 握手：Viewer 发 Hello（版本 + `auth_token`）→ Host 校验版本 → 调 `on_auth` → Welcome（`core/session/link.cpp:296-314`）。认证通过前不发任何画面数据。
- 心跳 1 s，10 s 收不到字节判断线；Goodbye 带 closed / incompatible / rejected 三种原因。
- **没有会话恢复**：断开即终态（`link.cpp:199`）。`keep_serving` 是换新传输、从关键帧建全新会话。
- **一个 Host 同时只服务一个 Viewer**（`link.cpp:304` 注释 “one viewer per host”）。

### UDP 可靠传输（`core/net/reliable_transport.cpp`）

- 每包新包号，ACK 最多 32 个区间，重传的是字节区间；丢包判定：后面 3 个包已确认、早于 9/8·RTT、或 PTO 指数退避（:239-256, :588-598）。
- 简化 BBR：Startup / Drain / ProbeBW 8 相增益，带宽取近 10 轮或 1 s 最大值，min_rtt 窗口 10 s，cwnd 约 2·BDP + 4 包，排队时 pacing 降到 0.9 倍。
- 数据报固定 1200 B（`core/net/datagram.h:54`），不依赖 IP 分片。
- 每通道独立有序字节流，发送优先级 control > input > cursor > surface > message > video，丢视频包不会卡输入和光标。
- **不支持连接迁移**：收到首包后 `connect()` 到对端地址（`core/net/udp_socket.cpp:91-97`），换网络或 NAT 重绑定会断。NAT 保活只靠 1 s 心跳顺带。
- **没有背压**：`Send` 直接 append 到发送缓冲（:116）；接收端乱序缓存、加密层握手前缓存也没有上限，存在内存 DoS 面。

### 安全（`core/transport/secure_transport.cpp`）

- 应用提供 32 字节**预共享密钥**；双方明文交换 `"RDAS"|ver|32B 随机数`，用 keyed BLAKE2b-512 派生两个方向的会话密钥（:149-152, :188-200）。
- XChaCha20-Poly1305，nonce = 通道号 + 每通道每方向 64 位计数器，AD 是通道号，能防重放、重排、跨通道挪用，跨连接重放也无效。
- **缺口**：没有 DH 密钥协商，**没有前向保密**（PSK 泄露后历史录包可解）；没有公钥/证书身份、没有换钥；认证只有“持有 PSK”加 `on_auth(token)` 两层；记录长度明文。
- C++ API 的 TCP/UDP 默认明文（`core/rdacore/transport.h:8-9` 注释要求跑在 VPN 或自己的安全传输里）；C API 强制要求密钥或显式 `allow_plaintext`（`src/capi/capi.cpp:641-652`）。

### 应用消息通道

可靠有序，单条最多 16 MiB，内置 type 1 = 剪贴板 UTF-8 文本，自定义类型从 `0x10000` 起（`core/rdacore/connection.h:21-22`），发送时按 256 KiB 分块且整条连续。适合剪贴板和控制信令；**不适合**直接做文件传输（大文件会堵住整条通道、无续传）和音频（只有可靠模式，优先级低于 surface）。要加新通道需要改 `kChannelCount`，会牵动优先级表、加密层和 TCP 帧校验三处。

### 自定义传输

C++ `Transport` 契约（`core/rdacore/transport.h:46-91`）：Send 线程安全、通道内有序、`SetReceiver` 前到的数据要留着、析构等回调返回；可选 GetStats / PeerKnown / PeerPresent / NextPeer。一个通道映射一条 QUIC/WebRTC stream 即可。
**注意**：C ABI 的 `rda_transport_vtable` 只有 send / set_receiver / close / get_stats / destroy 五个函数（`include/rdaccel/rdaccel.h:313-320`），**做不了 NextPeer / PeerKnown**，所以经 C 接口接入的自定义传输用不了 `keep_serving`。TCP 是单连接，有队头阻塞，注释写 “QUIC transport planned for later”（`core/transport/tcp_transport.h:5-7`）。

## 4. 公开 API 与工程化

- **接口**：`rdaccel.h` 是唯一二进制接口，62 个 `RDA_API` 函数，不透明句柄、带 `struct_size` 的配置结构体、13 个 `rda_status` 错误码、线程局部 `rda_last_error()`、进程级日志回调（不含画面、按键、消息内容）。`rdaccel.hpp` 是只有头文件的 C++20 包装，只承诺源码兼容。Swift / Rust / Python 都能直接绑。
- **Host**：约 35 个配置字段（每项优化都能单独关）、回调 on_input / on_error / on_state / on_message / on_auth、库自采或 `submit_frame`、约 32 个统计计数器。
- **Viewer**：on_stream_info / on_frame（BGRA 画布 + 变化矩形）/ on_cursor / on_state / on_message，`send_input`、`feed_recording`。
- **线程模型**：库自己开线程。on_frame 在解码线程，on_cursor 和 on_message 在接收线程，可能并发；destroy 返回后不再回调，不能在回调里 destroy；传输交给 create 后归库管（`docs/integration.md:128-145`）。
- **ABI 纪律**：1.x 只加不改，soname `.so.1`；链接脚本只导出 `rda_*`，CI 用 nm 检查；每个 PR 跑 abidiff（`tools/abi_check.sh`）；枚举强制 4 字节。
- **测试**：21 个测试文件、179 个 gtest 用例；8 个 libFuzzer 目标（messages、lossless、viewer、link、secure、reliable、hevc、recording），CI 每个跑 60 s；Linux 全量 ASan+UBSan；X11 测试跑在 Xvfb，Wayland 测试跑在无头 sway；Linux↔Mac 码流互解任务。macOS 的 VideoToolbox 步骤在 CI 里是 `continue-on-error`，硬件编解码靠真机复测清单。没有 Windows。
- **发版产物**：Linux x86_64 共享/静态包、macOS 通用二进制（arm64 + x86_64）、源码包、SHA256SUMS，带 CMake 包和 pkg-config。
- **许可**：Apache-2.0，Monocypher 是 BSD-2/CC0。**Linux 没有 NVENC 时会自动落到 libx265（GPL）**，NOTICE 写明带 x265 的二进制须按 GPL 分发。rdmachine 打包时要决定：用 GPL 分发，或自带只含 LGPL 组件的 FFmpeg 并把 VAAPI 做成熟。
- **示例**：
  - `rdaccel_remote`（`examples/remote_mac/main.mm`，737 行）是能用的 Mac 窗口控制端：IOSurface 只拷变化区、本机指针跟手、键鼠含 Command 快捷键、标题栏显示延迟。没有剪贴板、全屏、多屏选择等产品功能。
  - `rdaccel_serve --inject` 就是被控端（Linux 和 Mac）。
  - `examples/integration` 是纯 C 的无界面远程协助演示：加密、配对码当 token、依次服务多个接收端、剪贴板、注入、重连，CI 跑 smoke 测试。**这是 rdmachine 被控端服务最好的起点。**
  - Linux 没有窗口化 viewer，`rdaccel_view` 只打印统计和导出 PPM。

## 5. 对 rdmachine 的含义

### 可以直接拿来用

画面采集（mac 全面、Linux 基本可用）、分层编码与自适应码率、UDP 传输、光标通道、mac/X11 输入注入、剪贴板文本传输、PSK 加密、录制回放、稳定 C ABI。rdmachine 的 Host 守护进程和 Mac 客户端可以从 `examples/integration` 和 `rdaccel_remote` 起步。

### rdmachine 应用层必须自己做（库明确不做，`docs/design.md:48-49`）

1. **客户端界面**：连接管理、全屏/窗口/缩放、多屏切换、设置面板；Linux（和将来 Windows）客户端要从零写窗口渲染。
2. **账号、配对与密钥协商**：建议在 rdaccel 外层做一次 Noise（如 `Noise_XX` / `IK`）或 TLS 1.3 握手，产出每次连接的临时 32 字节密钥再交给 rdaccel，这样同时拿到公钥身份和前向保密，不用改库。
3. **信令、NAT 穿透、中继**：打洞后用 `udp_from_fd` 交给库；中继可实现自定义 Transport（注意 C vtable 的 NextPeer 限制）。
4. **会话管理**：服务化（systemd / launchd）、开机自启、登录界面与锁屏可用、按用户起会话、Linux 虚拟会话（Xvfb/Xorg dummy 或 Wayland headless）。
5. **文件传输、音频、打印、设备映射**：可以先在消息通道上做小文件和控制，但正式实现建议走独立连接（独立 QUIC stream 或独立 UDP 流），避免和画面抢同一条有序通道。
6. **权限模型**：只读观看、键鼠控制开关、被控端确认弹窗——输入事件本来就先回调给应用，容易实现。

### 建议回头改 rdaccel 的项（按对产品影响排序）

| 优先级 | 改动 | 原因 |
| --- | --- | --- |
| 高 | Wayland 输入注入（libei / portal RemoteDesktop） | 现在主流 Linux 桌面默认 Wayland，没有注入就只能看不能控 |
| 高 | 断线重连 / 会话恢复，UDP 连接 ID 与地址迁移 | 移动网络、Wi-Fi 切换是远程桌面的常态 |
| 高 | 发送缓冲背压、接收缓存上限 | 面向公网时的稳定性和 DoS 面 |
| 中 | X11 XDamage 脏区，X11/SCK 分辨率变化与热插拔自动重建 | 省 CPU，避免改分辨率后断流 |
| 中 | Linux 硬件解码（VAAPI / NVDEC），VAAPI 编码转正 | Linux 客户端 4K 下软解吃力；规避 x265 GPL |
| 中 | 多观看者（一路编码、多路分发） | NoMachine 有多人同屏/协作 |
| 中 | C vtable 增加 NextPeer / PeerKnown | 让 C 接口的自定义传输（中继）也能 keep_serving |
| 中 | 音频通道（不可靠/低延迟数据报语义） | 音频是远程桌面的基础体验 |
| 低 | H.264 回退、4:4:4 模式 | 老硬件兼容、设计/CAD 场景文字边缘 |
| 低 | 多屏同时编码、4K/高帧率评测、GPU 化分块比对 | 高端场景 |
| 低 | Windows 平台后端 | 用户面最大，但工作量也最大，可放到后期 |

---

*本文由四路并行代码研读汇总而成，引用的行号对应 rdaccel `89e0ee0`。*
