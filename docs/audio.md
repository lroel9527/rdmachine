# 远程声音

把被控 Mac 的系统声音传到客户端播放。2026-11-16 lroel 提出；取代[决定](decisions.md)里“音频：第一阶段不做声音”。本文是实现依据：实现按它做，审查按它对。原来标了“待验证”的地方，第 0 阶段能在不出声的前提下验的都验了，结果写在原处（和设想不同的地方相应改了设计，也写在原处）；要有声音才能验的留给 lroel，列在第 13 节。原来要 lroel 定的取舍都定了，汇总在最后一节。

版本：rdaccel 1.13.0，rdmachine 4.4.0，控制协议修订 3，最低系统 macOS 14.2。第 0–5 阶段已实现（2026-11-16），第 6 阶段（TCP 上的声音）按 D9 不做；实现时和本文不同的地方、实测的数字见第 17 节，要 lroel 在能出声时验的见第 13 节。

## 0. 硬性规则：任何测试都不许让这台 Mac 出声

开发用的这台 Mac 就是跑 RDMachine 服务端的 Mac mini，lroel 经常正通过 RDMachine 连着它。下面的规则对实现过程中的一切试验、对仓库里的一切自动测试都成立：

| 编号 | 规则 | 怎么保证 |
| --- | --- | --- |
| S1 | 不打开任何真实的输出设备 | 播放器有“空设备”模式（不建 AudioUnit，定时器按 48 kHz 取数后丢弃）；所有测试只用它。环境变量 `RDACCEL_AUDIO_NULL_OUTPUT=1` 时播放器强制用空设备，CTest 给每个测试都设这个变量（`set_tests_properties(... ENVIRONMENT)`），测试开头再断言一次变量在 |
| S2 | 不用 `afplay`、`say`、`NSSound`、`NSBeep`、`AudioServicesPlay*`、`AVAudioPlayer`、接到硬件的 `AVAudioEngine`、输出方向的 AudioQueue | 测试信号全部是程序算出来的 PCM（正弦、扫频、噪声突发、静音），直接喂给编码端；完成标准里有一条 `grep` 检查 |
| S3 | 不改任何设备的音量和静音 | 代码里不对 `kAudioDevicePropertyMute`、`kAudioDevicePropertyVolumeScalar`、`kAudioHardwareServiceDeviceProperty_VirtualMainVolume` 调 `AudioObjectSetPropertyData`（客户端只读）；`grep` 检查 |
| S4 | 自动测试里的 Process Tap 只用 `CATapUnmuted` | 让本机静音的两种行为（`CATapMuted`、`CATapMutedWhenTapped`）不进任何自动测试。tap 测试默认跳过，要 `RDACCEL_TEST_AUDIO_TAP=1` 才跑（它可能弹出权限询问），读到的样本只数个数、看格式和时间戳，不保存、不转发 |
| S5 | 不建虚拟显示器、不注入输入 | 声音的测试都不需要；端到端测试沿用 `test_media_e2e.cpp` 现有的无显示方式 |
| S6 | “真的有声音时对不对”只能由 lroel 自己验 | 见第 13 节的人工验收单。实现者不做，也不找替代办法（比如“先静音再放”：一旦 tap 没建成就会出声） |

S4 里“只输出数字静音的辅助进程”也不做：打开输出设备本身就可能让功放上电咔一声，而测试并不需要它（见 12.3）。

## 1. 总览

```text
被控 Mac（用户 agent）                                         客户端
Process Tap（全局、立体声、排除自己）
  → 私有聚合设备的 IOProc（实时线程，只拷贝）
  → 声音线程：转 48 kHz → 切帧 → 静音门 → Opus 编码 → 打包
  → rdaccel 传输的“数据报”（不重传、排在画面前、算进拥塞控制、加密）
                                                              → 抖动缓冲 → Opus 解码 / 丢包补偿
                                                              → 微调重采样（消除时钟漂移）
                                                              → 播放器（默认输出设备）
```

- **不采 ScreenCaptureKit 的声音**：屏幕采集照旧 `capturesAudio = NO`，画面部分一行不改。声音走 Core Audio Process Tap（macOS 14.2）：只在被我们读的时候让本机不出声（`CATapMutedWhenTapped`），进程一死 tap 就没了，本机声音自己回来，不动用户的音量和静音设置。苹果屏幕共享是把默认输出设备静音、结束后恢复（screensharingd），崩溃时有留下静音的可能，我们不这样做。
- **一个会话一路声音**，挂在主画面那条 rdaccel 连接上；扩展桌面的其余连接不带声音。rdmachine 同一时间只服务一个客户端（3.1.0：新连接接管旧会话），所以不存在多个听众。
- **登录窗口没有声音**：登录窗口 agent 不采集。锁屏时用户 agent 照常采集（音乐锁屏后还在放，就照常传）。

### 边界

| 放在哪 | 内容 |
| --- | --- |
| rdaccel（通用） | 数据报传输和它的加密；Opus 编解码；包格式；静音门；抖动缓冲、漂移修正；macOS 的 Process Tap 采集；可选的播放器模块（和 `rda_injector` 一样是可选件）；统计计数 |
| rdmachine | 发不发声音、本机静不静音这两个设置；协议里的开关和状态；权限的提示和菜单；客户端菜单；浮窗怎么显示；省流量模式和声音档位的对应 |

## 2. 权限（系统声音录制）

Process Tap 读到声音需要 TCC 的“系统声音录制”（`kTCCServiceAudioCapture`），在 系统设置 → 隐私与安全性 → 屏幕与系统音频录制 的“仅系统音频录制”一栏。它和“屏幕录制”是两项：在这台 Mac（macOS 27.2）上实测，同一个进程 `TCCAccessPreflight` 对屏幕录制返回 0（已允许）、对声音录制返回 2（没问过）。

- `packaging/macos/server-Info.plist.in` 加 `NSAudioCaptureUsageDescription`：`RDMachine 把这台 Mac 的声音传给连接进来的客户端。`没有这个键系统不给授权。
- 采集的进程是用户会话里的 agent，它就是 `RDMachine Server.app` 里的 `rdmachine-server`，TCC 按应用标识和签名要求记（和屏幕录制、辅助功能一样，自签证书固定，升级后不丢）。服务端没开 hardened runtime，不需要 entitlement。
- 公开接口里没有“查询/申请”这项权限的函数。用 TCC.framework 的私有函数（`dlopen` + `dlsym`，找不到就当“不知道”）：`int TCCAccessPreflight(CFStringRef service, CFDictionaryRef options)`，0 允许、1 拒绝、2 没问过；`void TCCAccessRequest(CFStringRef, CFDictionaryRef, void (^)(Boolean))` 弹出询问。这台 Mac 上两个符号都在。项目里已经有同类做法（`CGVirtualDisplay`、`__CGPreLoginApp`）。（D5，lroel 定：用。）
- **第 0 阶段实测：`TCCAccessPreflight` 在一个进程里按缓存回答。** 试验程序开始读声音、系统弹出询问，lroel 3.6 秒后点了允许（tccd 日志里 `authValue=2, authReason=2`），可这个进程之后 10 分钟里每秒查一次都还是 2，tccd 根本没再收到查询；新开的进程才查到 0。所以“每 2 秒再查一次”在同一个进程里看不到运行中给的授权，也看不到运行中撤销的。设计相应改成：读到非零样本本身就算证明有权限（不再限于“查不了”的时候）；没确认有权限期间每 5 秒重建一次 tap（新的 `AudioDeviceStart` 才会重新检查授权，见下）。菜单里的权限一行同样可能要等服务端重启才变。
- 服务端启动时（`mac_app.mm` 里和屏幕录制、辅助功能一起）：设置里开着“传声音”且 preflight 是 2 → 调 `TCCAccessRequest`。用户通常正远程连着，在画面里点“允许”即可。
- 菜单的权限区加一行：`系统声音录制：已允许` / `系统声音录制：未允许（点这里打开设置）`。打开 `x-apple.systempreferences:com.apple.preference.security?Privacy_AudioCapture`（第 0 阶段：这台 Mac 的 `SecurityPrivacyExtension.appex` 里有 `Privacy_AudioCapture` 这个锚点，和 `Privacy_ScreenCapture` 并列；没有打开设置页去试，免得在 lroel 的屏幕上弹窗口）。
- 没有权限时会话照常，只是没有声音：客户端浮窗显示 `Audio: off (server needs the System Audio Recording permission)`，服务端日志和菜单各一行。不为此弹窗、不拒绝会话。

**没有权限时不静音。** 没授权时 tap 只给全零而不报错（第 0 阶段实测，见下）；这时如果已经用了 `CATapMutedWhenTapped`，结果是两边都没声音。规则：

1. preflight 不是 0（或查不了）→ tap 用 `CATapUnmuted` 建；读它会触发系统询问（如果还没问过）。
2. 每 2 秒再查一次；变成 0，或者读到了非零样本（等于证明有权限）→ 换成设置要求的静音行为：拆了重建（不去改现有 tap 的 `kAudioTapPropertyDescription`：那条路径要用让本机静音的 tap 才能验证，这台 Mac 上不能试）。还没确认期间每 5 秒重建一次 tap，让运行中给的授权生效（同一进程里 preflight 不会变，见上）。
3. preflight 是 1（明确拒绝）→ 不建 tap，状态 `no_permission`，每 5 秒再查。

**第 0 阶段的结果**（2026-11-16，这台 Mac mini，macOS 27.2.1；临时目录里的试验程序，只用 `CATapUnmuted`，读几百毫秒到 3 秒，样本只数个数、看最大值和时间戳，不保存；lroel 事先同意弹一次询问）：

1. **建 tap 不问，开始读才问。** `AudioHardwareCreateProcessTap` 时 coreaudiod 只对屏幕录制和麦克风做了 preflight，不弹询问；第一次 `AudioDeviceStart`（注册 IO 上下文）时它对 `kTCCServiceAudioCapture` 发正式请求，这时才弹询问（tccd 日志 `AUTHREQ_PROMPTING`）。`AudioDeviceStart` 不等用户回答，立即返回 0。
2. **没授权时 `AudioDeviceStart` 返回 0，IOProc 照常按 48 kHz 回调，样本全是 0**（coreaudiod 日志 `Client is not granted access to the tap`）。询问还没回答时也一样。
3. **屏幕录制权限不够。** 从 Claude Code 里跑的进程，负责进程有屏幕录制权限，声音录制照样被拒（`authValue=0, authReason=8`：负责进程的 Info.plist 没有 `NSAudioCaptureUsageDescription`，系统直接拒绝、不弹询问）。带 `NSAudioCaptureUsageDescription` 的试验 app 自己没有屏幕录制权限，声音录制在 lroel 点了允许后就有了。两项完全独立；没有那个 Info.plist 键就永远拿不到。
4. 授权后新开的进程 preflight 是 0，tap 照常回调（3 秒 281 次）。授权在同一进程里看不到（见上）。

这次试验给试验 app（`org.rdaccel.phase0probe`，在临时目录里）留下了一条“允许”记录，lroel 可以在 系统设置 → 隐私与安全性 → 屏幕与系统音频录制 里删掉。

## 3. 采集（rdaccel，`platform/mac/audio_tap.{h,mm}`）

接口（`core/audio/audio_source.h`）：

```cpp
namespace rdacore::audio {
inline constexpr int kSampleRate = 48000;
inline constexpr int kChannels = 2;
struct SourceConfig {
  bool mute_local = false;                    // CATapMutedWhenTapped，仅在有权限时
  std::vector<std::string> exclude_bundle_ids;  // 连同本进程一起排除
};
enum class SourceState { kStarting, kRunning, kNoPermission, kFailed };
class Source {
 public:
  // 48 kHz、立体声、交错 float；time_us 是第一个样本的采集时刻（MonotonicMicros 的钟）。
  // 在声音线程上调用，不在实时线程上。
  using PcmFn = std::function<void(const float* interleaved, size_t frames, uint64_t time_us)>;
  using StateFn = std::function<void(SourceState, const std::string& message)>;
  virtual ~Source() = default;
  virtual bool Start(const SourceConfig&, PcmFn, StateFn) = 0;
  virtual void Stop() = 0;  // 返回后不再回调
};
std::unique_ptr<Source> CreateSystemSource();  // macOS: Process Tap；Linux: nullptr
std::unique_ptr<Source> CreateTestSource();    // 见 12.1，算出来的信号，不碰任何设备
}
```

建立步骤（都在一个串行的“重建线程”上做，不在回调里做）：

1. 排除列表：`kAudioHardwarePropertyTranslatePIDToProcessObject(getpid())`（没有声音 IO 的进程也有对象，实测）；再遍历 `kAudioHardwarePropertyProcessObjectList`，`kAudioProcessPropertyBundleID` 在 `exclude_bundle_ids` 里的也排除（rdmachine 传 `org.rdmachine.client`：客户端和服务端在同一台 Mac 上时不至于把自己播的再采回去）。
2. `CATapDescription* d = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:excluded]`；`d.name = @"rdaccel"`；`d.UUID = [NSUUID UUID]`；`d.privateTap = YES`；`d.muteBehavior` 按第 2 节的规则取 `CATapUnmuted` 或 `CATapMutedWhenTapped`。`AudioHardwareCreateProcessTap(d, &tap)`。
3. 读 `kAudioTapPropertyFormat`。只接受 32 位浮点 PCM、1 到 8 声道、8–192 kHz；交错与否都处理。别的格式 → `kFailed`（“unexpected tap format …”）。
4. 私有聚合设备：`AudioHardwareCreateAggregateDevice`，字典 `{name: "rdaccel audio tap", uid: "org.rdaccel.tap.<uuid>", private: 1, stacked: 0, tapautostart: 0, taps: [{uid: <tap 的 UUID 字符串>, drift: 1}]}`，**不放硬件子设备**（我们自己不打开输出设备）。第 0 阶段实测：只有 tap 的聚合设备能 `AudioDeviceStart`，名义采样率 48000、IO 缓冲 512 帧，IOProc 每 10.67 ms 回调一次（0.6 秒 56–57 次、3 秒 281 次，相邻两次最多隔 10.67 ms）。tap 的格式是 48 kHz、2 声道、32 位浮点交错（flags 0x9，每帧 8 字节）。不需要加硬件子设备。
5. `AudioDeviceCreateIOProcID`（C 函数，不用 block 版本）+ `AudioDeviceStart`。会话期间一直开着，不因为安静就停：停了再开会丢掉开头，也让“被读时才静音”的行为来回切换。
6. IOProc（实时线程）只做：把输入缓冲和 `inInputTime->mHostTime` 写进无锁的单写单读环（容量 1 秒），`semaphore_signal` 叫醒声音线程。不加锁、不分配、不打日志。
7. 声音线程：tap 的采样率不是 48000 时用 `AudioConverter`（`kAudioConverterSampleRateConverterQuality = kAudioConverterQuality_High`）转；多于 2 声道取前两个，单声道复制成两个；交给 `PcmFn`。时间戳：`mHostTime × numer / denom / 1000`，和 `capture::MonotonicMicros()` 是同一个钟（这台 Mac 上实测两者相等：libc++ 的 `steady_clock` 读的是 `CLOCK_MONOTONIC_RAW`，这台没睡过眠的 Mac 上它和 `mach_absolute_time`、`mach_continuous_time` 读数一样）。睡过眠的 Mac 上两者可能差出睡眠的时长，所以换算时顺手加上“此刻 `steady_clock` − 此刻 `mach_absolute_time`”的差。第 0 阶段：回调里 `steady_clock` 此刻减输入缓冲的 `mHostTime` 约 10.7 ms，正好一个缓冲（`mHostTime` 是缓冲第一个样本的时刻）。单元测试里断言换算结果和 `MonotonicMicros()` 相差小于 1 ms。
8. 两次回调的时间戳相差超过两个缓冲的长度：当成这段时间是静音，样本时间线按经过的时间往前推（见 5.2），不算故障。第 0 阶段实测：没人出声时 tap 照常每 10.67 ms 回调（全零）；这条留着，防的是调度卡顿和别的系统上不一样的行为。

什么时候拆了重建（300 ms 内的多次合并成一次；失败后每 2 秒再试，状态 `kStarting`，和截屏的 `CaptureLoop` 一个思路）：

- `kAudioHardwarePropertyDefaultOutputDevice` 变了（插拔耳机、显示器的声音设备出现或消失）；
- `kAudioTapPropertyFormat` 变了；
- `kAudioHardwarePropertyServiceRestarted`（coreaudiod 重启）；
- 聚合设备的 `kAudioDevicePropertyDeviceIsAlive` 变 0，或 `kAudioDevicePropertyDeviceIsRunning` 在我们没停的时候变 0；
- 进程列表里出现了要排除的应用（`kAudioHardwarePropertyProcessObjectList` 的监听）；
- 权限状态变了（第 2 节）。

拆的顺序：`AudioDeviceStop` → `AudioDeviceDestroyIOProcID` → `AudioHardwareDestroyAggregateDevice` → `AudioHardwareDestroyProcessTap`。重建期间的几十毫秒当静音处理。

**服务端崩溃不会留下静音**：tap 和私有聚合设备属于创建它的进程，进程没了 coreaudiod 就回收，本机声音恢复；我们没有改过任何持久的设置。这一点只能在有声音时确认，列在人工验收单里。

**未验证，要有声音，留给 lroel（第 13 节第 4 项）**：tap 拿到的是设备音量之前的信号（本机音量调小、按了静音键都不影响传过去的音量）。如果不是，文档和设置的说明要改。

## 4. 编码

libopus，`OPUS_APPLICATION_RESTRICTED_LOWDELAY`（只用 CELT，算法延迟 2.5 ms；码率都在 CELT 擅长的范围），48 kHz。不用 AudioToolbox 自带的 Opus 编码器：控制不了帧长、VBR、丢包补偿这些，Linux 也没有。

固定的设置：`OPUS_SET_VBR(1)`、`OPUS_SET_VBR_CONSTRAINT(0)`、`OPUS_SET_COMPLEXITY(10)`（两种模式都是 10：多花的只是 CPU，一路 48 kHz 立体声不到一个核的百分之几）、`OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)`、`OPUS_SET_LSB_DEPTH(24)`、`OPUS_SET_DTX(0)`、`OPUS_SET_INBAND_FEC(0)`、`OPUS_SET_PACKET_LOSS_PERC(最近 5 秒的丢包率，取整，0–20)`。每帧最多 500 字节。

两件事和最初的设想不同，写明原因：

- **不用 Opus 的带内 FEC**：它只在 SILK / 混合模式里有，音乐走的 CELT 没有；为了它强制用 SILK 会让音乐变差。丢包靠 Opus 的丢包补偿，丢包多时用我们自己的冗余（5.4）。
- **不用 Opus 的 DTX**：它在“认为没有活动”时让解码端自己造舒适噪声，安静但真实的内容（环境声、渐弱的尾音）会被换掉，这是听得出来的省法。我们只在真正的静音时不发（5.2）。

### 两个档位

| | 普通模式（`RDA_AUDIO_PROFILE_FULL`） | 省流量模式（`RDA_AUDIO_PROFILE_SAVER`） |
| --- | --- | --- |
| 帧长 | 10 ms | 20 ms |
| 每秒包数（有声音时） | 100 | 50 |
| 立体声目标码率 | 160 kbps | 96 kbps（D1，lroel 定） |
| 左右完全相同时（5.3）按单声道编 | 96 kbps | 56 kbps |
| 复杂度、采样率、频带、静音门、丢包冗余、抖动缓冲 | 相同 | 相同 |

档位可以在会话中途切换（`rda_host_set_audio_profile`），下一帧生效：Opus 每个包自带帧长和声道数，解码端不用知道档位。

### 依赖

放在 rdaccel 的 `CMakeLists.txt`，和 rdmachine 取 Mbed TLS、zstd 的方式一样（FetchContent、固定版本和哈希、静态编进去）：

```cmake
option(RDACCEL_AUDIO "Sound (Opus)" ON)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)   # 见下
FetchContent_Declare(opus
  URL https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
  URL_HASH SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(opus)   # EXCLUDE_FROM_ALL
target_link_libraries(rdaccel PRIVATE $<BUILD_INTERFACE:opus>)
```

- 第 0 阶段核对：`downloads.xiph.org/releases/opus/` 上最新的稳定版是 1.6.1（2026-01-15），`SHA256SUMS.txt` 里它的哈希是上面那个，下载下来算的一样；原先写的 1.5.2 的哈希也和 `SHA256SUMS.txt` 一致，但按“有更新的就用更新的”改用 1.6.1。1.6.1 的 CMake 选项和上面用的一样（`OPUS_DISABLE_INTRINSICS` 关掉 x86 的 SSE 和 ARM 的 NEON 检测）。
- `OPUS_DISABLE_INTRINSICS`：Opus 的 CMake 在配置时按一种 CPU 决定用哪些 SSE / NEON 文件，一次配置出 arm64 + x86_64 的 universal 构建（`build-uni`）会把一种架构的内联指令编进另一种。关掉后两种架构用同一份 C 代码。性能要求见 12.2（编码不低于实时的 50 倍）；不够时再改成按架构分别配置。
- rdaccel 静态安装时 `rdaccel.pc` 的 `Libs.private` 和导出的 CMake 包要带上 opus；动态库把它包在里面，导出表（`cmake/rdaccel.exp` / `.map`）不变，Opus 的符号不外露。Opus 是 BSD 许可，加进 rdaccel 的 `NOTICE`。
- `RDACCEL_AUDIO=OFF` 时声音相关的 C 函数都返回 `RDA_ERR_UNSUPPORTED`，不声明 `kCapAudio`。Linux 上编解码、传输、抖动缓冲照常编译和测试，只是没有系统采集（`CreateSystemSource()` 返回空）和播放器。
- 这是 rdaccel 的库本身第一次在配置时要联网（以前只有测试用的 GoogleTest）。（D8，lroel 定：下载；`FETCHCONTENT_SOURCE_DIR_OPUS` 可以指向本地源码离线构建。）

## 5. 发送端（rdaccel，`core/audio/`）

### 5.1 包格式

一个包放进传输的一个数据报（第 6 节）。小端。

```text
[0]     kind    u8   1 = 声音，版本 1。别的值整个包丢弃
[1]     flags   u8   bit0 CLOCK   后面跟 8 字节采集时刻
                     bit1 RED     后面跟上一帧的副本
                     bit2 END     这是静音前的最后一个包
                     bit3–7       必须是 0，否则整个包丢弃
[2]     spurt   u8   “声音段”编号：每次从静音恢复加 1
[3..4]  seq     u16  每发一个包加 1
[5..8]  ts      u32  主帧第一个样本的位置，单位是 48 kHz 的样本，静音期间照常往前走
[CLOCK] time    u64  主帧第一个样本的采集时刻，微秒，被控端的钟（和画面帧的 capture_time_us 同一个）
[RED]   len     u16  后面 len 字节是紧挨在主帧之前那一帧的 Opus 包（它的 ts = ts − 它自己的样本数）
        主帧          剩下的全部：一个 Opus 包（10 或 20 ms）
```

头 9 字节；整个包不超过 `kMaxDatagramPayload`（1100）。`ts` 约 24.8 小时回绕，接收端按模运算展开成 64 位。解析在 `core/audio/audio_packet.{h,cpp}`，配 fuzz（`fuzz/fuzz_audio.cpp`）。

CLOCK 在每个声音段的前 5 个包上带，之后每 250 个包带一次：接收端靠它把样本位置对到被控端的钟上，用来和画面对时间（7.4）。

### 5.2 静音门：没有声音就不发

桌面绝大部分时间是安静的，这是最大的一笔节省，而且一点都听不出来，所以**两种模式都做**。

- **什么算静音**：一帧里所有样本的绝对值都小于 2⁻¹⁵（约 −90.3 dBFS）。这是 16 位音频最小的一级：比它还小的东西 16 位的播放链路本来就放不出来，在任何正常音量下都低于房间的底噪。真正的“很小声”（渐弱的尾音、安静的环境声，−80 dBFS 以上）都在门限之上，照常发。不做“听起来像不像静音”的判断，只看这一个确定的门限。（D7，lroel 定：−90 dBFS、200 ms。更保守的选择是只认精确的 0：暂停着的播放器如果还在输出抖动噪声，就会一直按几十 kbps 发。）
- **进入静音**：连续 200 ms 都是静音帧（期间照常编码照常发，免得声音在门限上下来回时状态反复）。最后一个包带 END。然后**完全不发**，也不编码；连接的心跳本来就有，不需要声音自己的保活。
- **静音期间**：每来一帧只留着最近的一帧（“前一帧”），`ts` 照常前进。
- **恢复**：某一帧有一个样本超过门限 → `spurt` 加 1，编码器 `OPUS_RESET_STATE`，先编留着的前一帧，再编这一帧；发一个包：主帧是这一帧，RED 是前一帧，带 CLOCK。之后 4 个包也带 RED 和 CLOCK（声音段的开头最怕丢）。
- **为什么开头不会被削**：判断用的就是这一帧自己的内容，含有可闻样本的那一帧整帧都发，不需要“预读”；前一帧作为铺垫一起到，解码端在 `spurt` 变化时同样 `OPUS_RESET_STATE`，两边状态完全一致，CELT 帧间 2.5 ms 的重叠窗有前一帧可用，起音完整。恢复不比平时多任何延迟：这一帧编完就发，和连续播放时一样。
- 接收端：收到 END，或者 100 ms 没有包 → 回到空闲，播数字静音（最后的 200 ms 本来就在门限以下，不会有咔哒声）。新的 `spurt` 到了先攒到目标延迟再开始播（7.2），所以开头也不会因为抖动被吃掉。

### 5.3 左右完全相同时按单声道编

单声道的来源（系统提示音、多数通话、很多网页视频）经过 tap 的立体声混音后左右两路逐样本相同。连续 500 ms 每个样本都 L = R → `OPUS_SET_FORCE_CHANNELS(1)`，码率换成单声道那一档；出现一个 L ≠ R 的样本 → 立刻回到立体声（这一帧就按立体声编）。解码端始终按立体声解，单声道包解出来就是 L = R，和原来逐样本一致的东西相比没有任何声像上的损失，所以两种模式都做。Opus 的包自己带声道数，切换不需要通知对方。

不做“内容是音乐还是语音”之类的分析去选码率或下混：判断错一次就是听得出来的损失。VBR 本身已经按内容分配（简单的声音用得少），这是编码器内部的事。

### 5.4 丢包时的冗余

声音包不重传。平时不带冗余（好链路上是纯浪费）。传输层统计的真实丢包率（`packets_lost − packets_spurious`，最近 5 秒）≥ 1% → 每个包带上一帧的副本（RED），单个丢包由下一个包补上；连续 30 秒低于 0.3% → 关掉。两种模式门限相同：丢包的咔哒声是听得见的，体验优先，省流量模式也照样花这份流量。冗余的字节单独计数（`audio_red_bytes`）。

### 5.5 链路真的很慢时

和文字模式“链路真的慢时才退”一个意思，两种模式都适用，只在带宽估计可信（传输层已经度过起步阶段）时起作用：每秒看一次，声音的线上速率（含包头）不超过估计带宽的四分之一；超过就把目标码率降到够用为止，最低 32 kbps；估计回升后每 5 秒升一档（32 → 48 → 64 → 96 → 档位上限）。Tailscale 局域网上正常不会触发。TCP 上没有估计，按档位码率发。

### 5.6 客户端听不到时不发

客户端的默认输出设备静了音或音量是 0（只读地查 `kAudioHardwareServiceDeviceProperty_VirtualMainVolume` / `kAudioDevicePropertyMute`，加监听），或者用户在菜单里关了声音：客户端发 `paused`（第 9 节）。被控端照常采集（本机该静音还静音），只是不编码不发。恢复时当成一个新的声音段。两种模式都做：没人听得到，流量是零。（D6，lroel 定：做。）

### 5.7 发送端的结构

```cpp
namespace rdacore::audio {
struct SenderStats {  // 累计
  uint64_t packets = 0, bytes = 0;     // 发出的包；bytes 是包的全部字节（9 字节头起）
  uint64_t frames = 0, frames_gated = 0;  // 编了的帧；因为静音没发的帧
  uint64_t red_bytes = 0, dropped_stale = 0;
};
class Sender {
 public:
  using SendFn = std::function<bool(std::span<const uint8_t>)>;  // 一个数据报
  Sender(SendFn send, int profile);
  void Push(const float* interleaved, size_t frames, uint64_t time_us);  // 声音线程
  void SetProfile(int profile);
  void SetPaused(bool paused);
  void SetPath(double loss_fraction, int64_t bandwidth_bps);  // 每秒一次，来自传输统计
  SenderStats stats() const;
  // 给状态显示：当前目标码率、帧长、声道数、是否在静音段
};
}
```

`Push` 把样本攒成整帧（480 或 960 个），依次过：静音门 → 单声道判断 → 编码 → 打包 → `send`。全部在声音线程上，除了统计（原子量）没有共享状态。

## 6. 传输（rdaccel）

现有的传输只有“每个通道一条可靠有序的字节流”，声音需要不重传的包。

### 6.1 数据报

`core/rdacore/transport.h` 的 `Transport` 加（默认实现都是“不支持”）：

```cpp
inline constexpr size_t kMaxDatagramPayload = 1100;
virtual bool CarriesDatagrams() const { return false; }
// 不可靠、不保序、不重传；超过 kMaxDatagramPayload 或连接已关时返回 false。
virtual bool SendDatagram(std::span<const uint8_t> bytes) { (void)bytes; return false; }
using DatagramFn = std::function<void(std::span<const uint8_t>)>;
virtual void SetDatagramReceiver(DatagramFn fn) { (void)fn; }
```

`ReliableTransport`（UDP 和模拟网络）：

- 新的包类型 `kDatagram = 5`：`[5][包号 varint][内容]`。包号和数据包共用一个序列，对端照常确认，所以它**算进拥塞控制**：计入在途字节、投递速率、RTT 采样，`next_send_time_` 按它的大小往后推（画面因此让出这部分速率）。
- 发送队列：最多 64 个，各带入队时间；发送线程每轮先发它们，**不等拥塞窗口也不等发包间隔**（声音不到总流量的百分之几，排队就是抖动），排在所有通道前面。入队超过 100 ms 还没发的丢弃并计数。
- `SentPacket.ch = 0xFE`（`kDatagramChannel`）。`MarkLost`：计入 `packets_lost_` 和 `lost_recent_`（晚到的确认照旧算虚假丢失），**不重发**。`HandleAck`、`ReleaseAcked` 里按通道下标访问的地方都已经有 `ch < kChannels` 的判断，逐处确认。`app_limited = true`（和校验包一样：不拉低带宽估计）。
- 不进传输层的 FEC 分组（那是为可靠流省一次重传等待的；声音的冗余在 5.4）。
- 接收：`MarkReceived` 后在锁外交给 `DatagramFn`；没设接收者就丢。重复的包由加密层的重放窗口挡掉。
- QoS：`TrafficClass::kInteractive`，和画面、输入一样。
- 旧版本对端收到类型 5 会忽略、也不确认，所以只在握手确认对方支持后才发（6.3）。

`LocalPipe`：直接交给对端的接收者。`TcpTransport`、录制、应用自己的传输（`rda_transport_vtable`）：不支持，这次不加公开接口；TCP 见 6.4。

码率控制：`src/host.cpp` 在调 `RateController::Update` 之前，从 `st.bandwidth_bps` 里减去最近 1 秒声音的线上速率（不低于 `min_bitrate_bps`），画面的目标码率就不会把声音那份也算给自己。

### 6.2 加密

和现有通道同一套：XChaCha20-Poly1305，同一对按方向派生的连接密钥。每个数据报单独封：

```text
[计数器 u64 小端][密文][Poly1305 标签 16 字节]
nonce：第 0 字节 0xFE（通道号用 0–7，不会撞），第 16–23 字节是计数器；关联数据：一个字节 0xFE
```

- 每个方向一个计数器，从 0 起。密钥还没协商完时 `SendDatagram` 返回 false（丢掉）。
- 接收端：128 个位置的滑动窗口，计数器重复或比窗口还旧的丢弃（防重放）；验不过的**丢弃并计数**，第一次写一条警告日志，不断开连接（可靠通道验不过是流被破坏了，只能停；数据报丢一个不影响别的）。
- 加密的开销是每包 24 字节。

### 6.3 能力协商

`core/proto/messages.h`：`kCapAudio = 64`，不进 `kCapsAll`。Viewer 在 `rda_viewer_config.audio` 为 1 且传输 `CarriesDatagrams()` 时放进 `ViewerHello`；Host 在同样条件下放进 `Welcome`。Host 只在对方有这一位时才采集、才发；对方没有（旧版本、没开、TCP）时状态是 `waiting_peer`，tap 根本不建，本机也不静音。线上协议版本号（4）不变。

### 6.4 TCP 退回时

第 6 阶段，**先不做**（D9，lroel 定：等真的在 TCP 上用到再说；现在 TCP 上就没有声音，状态是 `waiting_peer`）。以后要做时的做法：新的可靠通道 `Channel::kAudio = 7`（`kChannelCount` 变 8；`SecureTransport` 的各个数组跟着变），内容是 `[长度 u16][5.1 的包]` 连着放；`TcpTransport`、`LocalPipe` 报 `CarriesAudioChannel()`，这时也声明 `kCapAudio`。TCP 丢包时会一顿一顿地到，接收端超出上限的旧声音直接跳过（7.2）。旧的 TCP 对端遇到不认识的通道号会断开，所以同样只在协商后才用。

## 7. 接收端（rdaccel，`core/audio/`）

### 7.1 线程

- 传输的接收线程：`Receiver::OnPacket` 只做长度检查，把包拷进无锁的单写单读环（64 个位置，每个 1200 字节；满了丢最旧的并计数）。
- 播放线程（播放器的渲染回调，实时线程）：`Receiver::Read` 独占抖动缓冲、解码器、重采样器。不加锁、不分配内存（都在构造时分配好）、不打日志；统计用原子量。

### 7.2 抖动缓冲

- 按 `ts` 排好的帧；重复的、已经播过位置的丢弃（计“晚到”）。带 RED 的包：副本那一帧如果还没有，就补进去。
- **目标延迟**：每个包算相对延迟 `d = (到达时刻 − 第一个包的到达时刻) − (ts − 第一个包的 ts) / 48000`；`抖动 = 最近 5 秒里 d 的 99 分位 − 最近 10 秒里 d 的最小值`；`目标 = clamp(抖动 + 5 ms, 20 ms, 60 ms)`，起始 30 ms。5 秒内出现过欠载（没东西可播）时上限放宽到 200 ms，连续 30 秒没有欠载再收回 60 ms。跨声音段保留。
- **一个声音段的开头**：攒到目标延迟再开始播。
- **缺帧**：到了该播的时候帧没到 → Opus 丢包补偿（`opus_decode_float` 传空指针），连续最多 5 帧，之后淡出到静音、计一次欠载；之后来的包重新对齐。
- **积得太多**（一阵网络停顿后一起到）：缓冲超过 `max(3 × 目标, 目标 + 100 ms)` 持续 500 ms → 丢到目标为止，接缝处 5 ms 交叉淡化。平时不靠丢帧、插帧调整。
- 解码后 `opus_pcm_soft_clip`。

### 7.3 时钟漂移：轻微变速，不丢不插

两台 Mac 的声卡时钟差几十 ppm，不处理的话缓冲会慢慢涨满或见底。播放线程每 100 ms 算一次：`L` = 缓冲水位的 1 秒滑动平均（ms），`e = L − 目标`；`ppm = clamp(150 × e + I, −5000, +5000)`，`I += 5 × e`（限制在 ±500）；重采样比例 = `1 + ppm / 10⁶`。最大 0.5% 的变速相当于不到 9 音分，而且是缓慢变化，听不出来；正常工作点在 ±100 ppm 以内。

重采样器（`core/audio/resampler.{h,cpp}`）：比例在 1 附近可变的多相 sinc 插值（Kaiser 窗，不少于 32 抽头、256 相位、相位间线性插值），质量要求见 12.2。

### 7.4 和画面对时间

低延迟优先，不为了对口型去拖慢任何一边，只防止声音明显跑到画面前面。

- 声音：`Read` 的调用者给出这批样本到达扬声器的本地时刻；结合 CLOCK 得到“声音的延迟” `da = 播放时刻 − 采集时刻`。
- 画面：Viewer 在每次 `on_frame` 返回时记 `dv = 本地时刻 − capture_time_us`（1 秒滑动中位数）。
- 两台机器的钟不同步，`da`、`dv` 各自没有意义，但差值 `skew = da − dv` 里钟差抵消了。`skew > 0` 是声音比画面晚。
- 正常情况声音比画面晚 20–60 ms，在人察觉不到的范围里（广播的经验值：声音早 45 ms、晚 125 ms 以内察觉不到），什么都不做。
- `skew < −20 ms`（声音早于画面）持续 2 秒：把目标延迟加上差额，最多加 120 ms；`skew` 回到 0 以上后每秒收回 10 ms。最近 1 秒没有画面帧（屏幕静止）时不做这项。（D10，lroel 定：做。）
- `skew` 作为 `av_offset_ms` 放进统计。

### 7.5 播放器（rdaccel，可选模块，`platform/mac/audio_player.{h,mm}`）

和 `rda_injector` 一样：应用可以用它，也可以自己调 `rda_viewer_read_audio`。（D4，lroel 定：放在 rdaccel。）

- `kAudioUnitSubType_DefaultOutput` 的输出 AudioUnit：默认输出设备换了它自己跟着换。流格式 48 kHz 立体声 float，设备采样率不同由 AudioUnit 转。IO 缓冲请求 256 帧（`kAudioDevicePropertyBufferFrameSize`，这是本客户端的请求，不是持久设置）。
- 渲染回调：`rda_viewer_read_audio(viewer, buf, frames, 输出时间戳换算成的本地时刻)`。
- 空设备模式（`null_device = 1` 或环境变量 `RDACCEL_AUDIO_NULL_OUTPUT=1`）：不建 AudioUnit，一个线程每 5 ms 取 240 帧后丢弃。渲染逻辑和真设备走同一个函数，只有建 AudioUnit 的几十行在测试里跑不到。
- 不提供调音量的功能（跟系统音量走）；只读地报告默认输出是否静音，给 5.6 用。

## 8. rdaccel 的 C 接口（1.13.0，只加不改）

```c
/* ---- audio (1.13) -------------------------------------------------------- */
#define RDA_AUDIO_SAMPLE_RATE 48000
#define RDA_AUDIO_CHANNELS 2
#define RDA_AUDIO_PROFILE_FULL 0
#define RDA_AUDIO_PROFILE_SAVER 1
#define RDA_AUDIO_SOURCE_APP 0     /* rda_host_submit_audio() */
#define RDA_AUDIO_SOURCE_SYSTEM 1  /* macOS 14.2: everything this computer plays */
#define RDA_AUDIO_SOURCE_TEST 2    /* a computed test signal; touches no device */
#define RDA_AUDIO_STATE_OFF 0
#define RDA_AUDIO_STATE_WAITING_PEER 1   /* no viewer, or one that takes no sound */
#define RDA_AUDIO_STATE_STARTING 2
#define RDA_AUDIO_STATE_RUNNING 3
#define RDA_AUDIO_STATE_PAUSED 4
#define RDA_AUDIO_STATE_NO_PERMISSION 5
#define RDA_AUDIO_STATE_FAILED 6         /* retried every 2 s; see message */

typedef struct rda_audio_config {
  uint32_t struct_size;
  int32_t source;      /* RDA_AUDIO_SOURCE_SYSTEM */
  int32_t profile;     /* RDA_AUDIO_PROFILE_FULL */
  int32_t mute_local;  /* 0; 1: this computer stays silent while its sound is captured */
  const char* const* exclude_bundle_ids; /* NULL; applications not to capture (copied) */
  size_t exclude_bundle_id_count;
} rda_audio_config;

typedef struct rda_audio_status {
  uint32_t struct_size;
  int32_t state;        /* RDA_AUDIO_STATE_* */
  int32_t profile;
  int32_t silent;       /* running, and nothing is being sent: the computer is silent */
  int32_t bitrate_bps;  /* the encoder's target now */
  int32_t frame_ms;     /* 10 or 20 */
  int32_t channels;     /* coded: 1 or 2 */
  int32_t muted_local;  /* the capture mutes this computer right now */
  char message[160];    /* why, for FAILED and NO_PERMISSION */
} rda_audio_status;

RDA_API void rda_audio_config_default(rda_audio_config* c);
/* Sound for the host's viewer. Capture starts when a viewer that takes sound
 * is connected and stops when it leaves. RDA_ERR_UNSUPPORTED where the source
 * does not exist (SYSTEM on Linux) or the library was built without sound. */
RDA_API rda_status rda_host_start_audio(rda_host* h, const rda_audio_config* config);
RDA_API void rda_host_stop_audio(rda_host* h);
/* RDA_AUDIO_SOURCE_APP: 48 kHz, stereo, interleaved float. Calls must not overlap. */
RDA_API rda_status rda_host_submit_audio(rda_host* h, const float* interleaved, size_t frames,
                                         uint64_t capture_time_us);
RDA_API void rda_host_set_audio_profile(rda_host* h, int profile);
RDA_API void rda_host_set_audio_paused(rda_host* h, int paused);
RDA_API rda_status rda_host_get_audio_status(const rda_host* h, rda_audio_status* out);

/* Viewer: fills `out` with exactly `frames` frames (48 kHz, stereo, interleaved
 * float), silence where there is none; returns how many were sound. For an
 * audio render thread: never blocks, allocates or logs. play_time_us: when the
 * first frame reaches the speaker (rda_now_us clock), 0 = now. One caller. */
RDA_API size_t rda_viewer_read_audio(rda_viewer* v, float* out, size_t frames, uint64_t play_time_us);

typedef struct rda_audio_player rda_audio_player;
typedef struct rda_audio_player_config {
  uint32_t struct_size;
  int32_t null_device; /* 0; 1: open no device, read and discard (tests) */
} rda_audio_player_config;
RDA_API void rda_audio_player_config_default(rda_audio_player_config* c);
/* Plays the viewer's sound on the default output device and follows it when
 * it changes. Destroy it before the viewer. */
RDA_API rda_status rda_audio_player_create(rda_viewer* v, const rda_audio_player_config* config,
                                           rda_audio_player** out);
RDA_API void rda_audio_player_destroy(rda_audio_player* p);
/* 1: the default output is muted or at volume 0; 0: audible; -1: not known. */
RDA_API int rda_audio_player_output_muted(const rda_audio_player* p);
```

结构体末尾追加：

- `rda_viewer_config`：现在以 `int32_t allow_plaintext` 结尾，后面有 4 字节的对齐空位，旧程序的 `struct_size` 把它算在内。照 `rda_host_config.reserved0` 的先例：先加 `int32_t reserved0;`（从不读），再加 `int32_t audio; /* 0; 1: take the host's sound */`。用 `offsetof` 的静态断言钉住。
- `rda_host_stats`（末尾是 8 字节字段，没有空位）：`uint64_t audio_packets, audio_bytes, audio_frames, audio_frames_gated, audio_red_bytes, audio_dropped_stale;`。**`bytes_sent` 包含 `audio_bytes`**。
- `rda_viewer_stats`：`uint64_t audio_packets, audio_bytes, audio_lost`（用丢包补偿顶上的帧）`, audio_late, audio_underruns; int32_t audio_buffer_ms, audio_target_ms, audio_drift_ppm, av_offset_ms;`。`bytes_received` 包含 `audio_bytes`。
- `rda_transport_stats` 不变（数据报的计数已经在 `packets_sent` / `packets_lost` 里）。

`rdaccel.hpp`、`cmake/rdaccel.exp`、`cmake/rdaccel.map`、`tools/abi_check.sh` 的基线跟着更新。回调线程的说明写进 `docs/integration.md`。

## 9. rdmachine：协议（修订 3）

- `session_request.audio`（布尔，可省略，默认 false）：客户端想要声音。
- `session_ready.audio`（布尔，可省略，默认 false）：这次会话会有声音。false 时可带 `audio_reason`：`disabled`（服务端设置关了）、`login_window`、`unsupported`（系统或构建不支持）。没有权限**不**在这里说（会话中途可能授权），在统计里说。
- 只在协商出的修订 ≥ 3 时用：`ServerSession` 在修订不够时把 `request.audio` 当 false。旧服务端忽略这个字段、不回 `audio`，客户端就不开声音；旧客户端不发，服务端不采集。两个方向都照常工作，只是没有声音。
- 守护进程和 agent 之间不加消息：`start_session.request` 原样带着 `audio`，`grant` 带回 `audio` / `audio_reason`（`SessionGrant` 加这两个字段）。
- 客户端只在 `session_ready.audio` 为真时给主画面的 viewer 设 `audio = 1` 并建播放器；扩展桌面的其余 viewer 一律 0。
- 会话中途：rdaccel 应用消息 `0x10013`（`kAudioControlMessageType`，`core/include/rdmachine/audio_control.h`），客户端到服务端，JSON `{"paused":false}`。`paused` 为真：菜单里关了声音，或者客户端的输出静了音。不认识的字段忽略。
- 档位不单独传：服务端在会话开始和每次收到文字模式消息时，`text_mode` 为真就设 `RDA_AUDIO_PROFILE_SAVER`，否则 `FULL`。
- 会话统计（`0x10002`）加：`"audio"`：`off` / `starting` / `on` / `silent` / `paused` / `no_permission` / `failed`；`"audio_kbps"`：最近一秒声音的码率（`audio_bytes` 的差，和 `bitrate_kbps` 同一个口径）。旧客户端忽略。

## 10. rdmachine：设置、界面、生命周期

### 设置

服务端（`ServerSettings`，`server-settings.json`，设置窗口两个勾选框）：

| 键 | 默认 | 设置窗口上的文字 | 命令行 |
| --- | --- | --- | --- |
| `send_audio` | true | 把这台 Mac 的声音传给客户端 | `--audio=on\|off` |
| `mute_local_audio` | true | 传声音时这台 Mac 自己不出声 | `--mute-local=on\|off` |

和现有设置一样，改了以后对下一个会话生效。隐藏的 `--audio-source=test` 给自动测试用（`RDA_AUDIO_SOURCE_TEST`）。

**本机什么时候静音**：只在声音真的在往一个客户端传的那个会话里（设置开着、有权限、客户端要了声音）。客户端暂停（5.6）期间照旧静音：把笔记本静音不应该让家里的 Mac mini 突然响起来。登录窗口、旧客户端、客户端关了声音再连进来的会话：不采集，本机照常出声。（D3，lroel 定：如上。）

客户端：菜单 View → Play Sound from Remote Computer（`PlayRemoteSound`，默认开，记住）。关掉：当前会话发 `paused:true`，以后的会话 `session_request.audio = false`。命令行客户端 `rdmachine-connect --audio`：要声音，**永远用空设备**，退出时打印声音统计。

### 浮窗和菜单

- 客户端浮窗新的一项 `{@"audio", @"声音", false}`：`Audio: 96 kbps · buffer 32 ms · lost 0.1%`、`Audio: silent`、`Audio: paused`、`Audio: off (server setting)`、`Audio: off (server needs the System Audio Recording permission)`、`Audio: off (login window)`、`Audio: off (older server)`。码率来自服务端的 `audio_kbps`，缓冲和丢失来自本机的 `rda_viewer_stats`。
- 现有的“码率”一项不用改：`bitrate_kbps` 来自 `bytes_sent`，已经包含声音。
- 服务端菜单：权限区的一行（第 2 节）；会话状态里一行 `声音：正在传 / 安静 / 未传（原因）`。`LiveStats` 加 `audio_kbps`，`MediaHostStatus` 加 `rda_audio_status audio`。

### 生命周期

| 事件 | 行为 |
| --- | --- |
| 会话开始（`MediaHost::StartSession`） | 主画面的 host 建好后，`request.audio && options_.send_audio && !options_.login_window` → `rda_host_start_audio`（`mute_local` 来自设置，`exclude_bundle_ids = {"org.rdmachine.client"}`，档位来自 `text_mode`）。失败只写日志，会话照常 |
| viewer 连上 | rdaccel 看到对方有 `kCapAudio` 才建 tap（6.3） |
| 会话结束、被接管、`bye` | `EndSession` 里先 `rda_host_stop_audio` 再销毁 host；tap 拆掉，本机声音恢复 |
| 退出登录 → 登录窗口 | 用户 agent 的会话结束（tap 拆掉），登录窗口 agent 的会话没有声音，新的 `session_ready.audio = false`（`login_window`） |
| 登录窗口 → 登录进桌面 | 迁移到用户 agent，新的 `session_ready.audio = true`；客户端 `on_moved` 里换 viewer 时重建播放器，把 `paused` 状态重新发一次 |
| 换了别的用户（快速用户切换） | 会话以 `console_changed` 结束，照旧 |
| 锁屏 | 用户 agent 照常采集、照常传 |
| 被控端睡眠 | 连接断，会话结束；醒来后客户端重连是新会话 |
| 被控端输出设备变了 | rdaccel 自己重建 tap（第 3 节），几十毫秒的空当 |
| 客户端输出设备变了 | 播放器自己跟随 |
| tap 建不起来 | 状态 `failed` + 原因，每 2 秒再试；统计里 `audio:"failed"`，日志只在状态变化时写一行 |
| 没有权限 | 第 2 节；授权后 2 秒内自动开始，不用重连 |
| 服务端进程崩溃或被杀 | tap 随进程消失，本机声音恢复；launchd 拉起新 agent，会话迁移过去后重新开始 |
| 客户端全屏进出（重连一次） | 新会话，声音跟着重新开始 |

### 要改的文件

| 文件 | 改什么 |
| --- | --- |
| `CMakeLists.txt`（两个仓库） | 最低系统 14.0 → 14.2；版本号 |
| `core/include/rdmachine/protocol.h` | `kProtocolRevision = 3` |
| `core/…/messages.{h,cpp}`、`server_session.{h,cpp}`、`agent_link.cpp` | `SessionRequest::audio`、`SessionReady::audio` / `audio_reason`、`SessionGrant` 同名字段；修订不够时清掉 |
| `core/…/audio_control.{h,cpp}`（新） | `0x10013` 的编解码，照 `picture_mode` 的样子 |
| `core/…/session_stats.{h,cpp}` | `audio`、`audio_kbps` |
| `session/…/server_settings.{h,cpp}` | 两个设置 |
| `session/…/media_host.{h,cpp}` | `MediaHostOptions::send_audio / mute_local_audio / audio_source`；开始、结束、`OnMessage`、文字模式切换时换档位、`StatsLoop`、状态 |
| `session/…/live_stats.{h,cpp}` | `audio_kbps` |
| `session/…/remote_session.{h,cpp}` | `Options::audio`、`audio_null_device`；viewer 的 `audio`；播放器随 viewer 建和拆（`FollowMove` 里也是）；`SetAudioPaused` |
| `server/main.cpp`、`server_runtime.*`、`mac_app.mm` | 命令行、设置窗口、权限申请和菜单 |
| `client/main.mm`、`connect_main.cpp` | 菜单、浮窗、输出静音的监听、`--audio` |
| `packaging/macos/server-Info.plist.in` | `NSAudioCaptureUsageDescription` |
| `packaging/macos/pkg/…` | 安装检查脚本和 `welcome.html` 里的 14 → 14.2 |

## 11. 省流量模式下的声音

原则不变（[决定](decisions.md)“文字模式”）：**先保证体验，再在听不出来的地方省**。省流量模式是为了少花流量，不是因为链路慢；以前每一处看得出来的节省最后都成了抱怨，声音按同样的标准来。

### 11.1 每一项省在哪、凭什么说听不出来

| 做法 | 省多少 | 听得出来吗 | 哪种模式 |
| --- | --- | --- | --- |
| 静音时完全不发（5.2） | 安静时 100%。桌面大部分时间是安静的，这是最大的一项 | 听不出来：门限是 16 位音频的最小一级，门限以上的小声照发；开头有前一帧铺垫，不削 | 两种都做 |
| 客户端听不到时不发（5.6） | 那段时间 100% | 本来就听不到 | 两种都做 |
| 左右相同按单声道编（5.3） | 这类内容约四成 | 逐样本相同的两路，合成一路没有信息损失 | 两种都做 |
| 丢包冗余只在有丢包时开（5.4） | 好链路上省掉一倍 | 没有丢包时冗余没有任何作用 | 两种都做 |
| VBR：简单的内容少用码率 | 提示音、语音比目标码率低不少 | 编码器按同一个质量目标分配 | 两种都做 |
| 20 ms 一帧（普通模式 10 ms） | 包数减半：每秒少 50 个包头，约 26 kbps（经 Tailscale 约 50 kbps）；同样质量下编码本身也省一成多 | 代价是声音晚 10 ms。单看延迟和对口型都远在察觉范围之内，但它是延迟上的差别，不是零 | 只省流量模式（D2，lroel 定：20 ms 一帧一包） |
| 立体声 96 kbps（普通模式 160） | 音乐、视频的声音约四成 | 公开的盲听测试里 Opus 在 96 kbps 接近透明（2014 年 HydrogenAudio 的 96 kbps 测试，满分 5 分得 4.65），绝大多数人在绝大多数内容上分不出来；受过训练的人在个别刁钻的片段上可能分得出。**这是唯一一项靠“感知上听不出”而不是“确定没有损失”的节省** | 只省流量模式（D1，lroel 定：96 kbps） |

不做的（都听得出来）：下混成单声道、降采样率或砍频带、Opus 的 DTX 和舒适噪声、按“音乐 / 语音 / 提示音”猜内容选更低的码率、降低编码复杂度（省的是 CPU，不是流量）、为了少发包把抖动缓冲调大。

### 11.2 包头的账

有声音时每个包除了 Opus 的数据还有：

| | 字节 |
| --- | --- |
| 声音包头（5.1） | 9（开头几个包多 8 字节 CLOCK） |
| rdaccel：类型 + 包号 | 约 4 |
| 加密：计数器 + 标签 | 24 |
| UDP + IPv4 | 28 |
| 合计（不含 Opus 数据） | 约 65 |
| 经 Tailscale 再加（WireGuard + 外层 UDP/IP） | 60 |

每秒 100 个包（10 ms）是 52 kbps 的包头，50 个（20 ms）是 26 kbps，经 Tailscale 分别是 100 和 50 kbps。所以省流量模式里帧长比码率的档位还要紧。

**一个包里放两帧（40 ms 一个包）**：包头再减半（再省约 13 kbps，经 Tailscale 约 25 kbps），代价是第一帧要等第二帧，声音再晚 20 ms，丢一个包丢 40 ms（补偿的效果明显变差）。**不做**（D2，lroel 定）。不增加延迟的合包只有一种，已经在用：声音段开头把铺垫的前一帧和第一帧放在同一个包里（前一帧本来就已经在手上）。

把加密的计数器从 8 字节缩到 4 字节每包能省 4 字节（约 1.6 kbps），不值得为它多一套回绕处理，不做。

### 11.3 估算

“统计口径”是浮窗里显示的数字（rdaccel 交给传输的字节：Opus 数据 + 9 字节包头，和画面的码率同一个口径，都不含 UDP/IP）。“线上”加上每包 56 字节（rdaccel、加密、UDP/IP），不含 Tailscale 的 60 字节。**都是估算**：VBR 的实际码率取决于内容，第 2 阶段用测试信号、第 5 阶段用 lroel 的真实使用量出实测值后回填本表。

| 场景 | 普通模式：统计 / 线上 | 省流量模式：统计 / 线上 | 说明 |
| --- | --- | --- | --- |
| 安静的桌面 | 0 / 0 | 0 / 0 | 一个包都不发 |
| 偶尔的提示音（每分钟 6 次，每次连尾巴约 0.5 秒） | 约 9 / 12 kbps | 约 5 / 7 kbps | 平均值；响的那半秒里和“音乐”一行相同 |
| 音乐、YouTube（立体声，连续） | 约 167 / 212 kbps | 约 100 / 122 kbps | 线上每小时约 95 MB / 55 MB；经 Tailscale 约 117 MB / 66 MB |
| 视频通话（语音，左右相同） | 约 80–103 / 125–148 kbps | 约 45–60 / 66–82 kbps | 说话的间隙低于门限时不发，实际更低 |
| 视频通话（左右不完全相同） | 约 110–167 / 155–212 kbps | 约 65–100 / 86–122 kbps | |
| 客户端静音 / 关了声音 | 0 / 0 | 0 / 0 | |

和画面比：省流量模式下静止的文字每秒只有几 kbps，所以**放着音乐、屏幕不动时，声音就是全部流量**（每小时约 55 MB，经 Tailscale 约 66 MB）。这一点会直接体现在 lroel 的流量上，所以 D1 是真正要选的：

| 省流量模式的立体声码率 | 连续音乐每小时（线上 / 经 Tailscale） | 风险 |
| --- | --- | --- |
| 128 kbps | 约 69 MB / 80 MB | 没有 |
| **96 kbps（推荐）** | 约 55 MB / 66 MB | 极个别内容上受过训练的人可能分得出 |
| 64 kbps | 约 40 MB / 51 MB | 一部分音乐上普通人仔细听分得出（高频的空气感、混响尾巴） |

### 11.4 流量怎么计

- `rda_host_stats.bytes_sent` 包含声音，浮窗的“码率”因此包含声音；另有单独的“声音”一项。
- 和画面一样，这些数字不含 UDP/IP 和 Tailscale 的包头。声音的包小，包头占的比例比画面大得多（省流量模式下约两成，经 Tailscale 约四成），估算流量时按 11.3 的“线上”一列，不要按浮窗的数字。浮窗的口径这次不改（改了画面的数字也要跟着变，是另一件事）。

## 12. 测试（全部遵守第 0 节）

### 12.1 测试信号

`CreateTestSource()` 和测试里的生成函数（`core/audio/test_signal.{h,cpp}`）都是算出来的，循环 4 秒：0–0.3 秒 1 kHz 正弦 −20 dBFS（左右相同）；0.3–1.5 秒静音（精确的 0）；1.5–2.5 秒左 440 Hz、右 660 Hz；2.5–2.6 秒 −95 dBFS 的噪声（门限以下）；2.6–3.0 秒 −70 dBFS 的 1 kHz（门限以上的小声）；3.0–4.0 秒静音。另有：20 Hz–20 kHz 对数扫频、多音（12 个不成倍数的频率）、白噪声突发。

### 12.2 rdaccel 单元测试（GoogleTest，macOS 和 Linux 都跑）

| 文件 | 检查什么 |
| --- | --- |
| `tests/test_audio_packet.cpp` | 编出来再解回去相同；各种截断、保留位、超长都拒绝；`ts` 回绕展开正确 |
| `tests/test_audio_gate.cpp` | 精确的 0 和 −95 dBFS 不发；−70 dBFS 照发；200 ms 的尾巴；恢复时第一个包带前一帧、`spurt` 加 1；门限上下来回不抖动；左右相同 500 ms 后转单声道，出现不同立刻转回 |
| `tests/test_audio_codec.cpp` | 每个档位：多音信号编解码后各频带（20 个）的能量和原来相差平均不超过 1 dB，对齐后的信噪比不低于 15 dB（感知编码不追求波形一致，这是防止出大错的下限，不是音质指标）；起音：1 秒静音后的 1 kHz 突发，起点前 5 ms 以外没有超过 −60 dBFS 的东西，起点后 5 ms 内的能量和原来相差不超过 1 dB；实际码率在目标的 ±25% 内；编码速度不低于实时的 50 倍（两种架构各测一次）；10 ↔ 20 ms、单 ↔ 双声道中途切换没有间断（切换点前后 2 ms 的样本差不超过相邻帧的正常水平） |
| `tests/test_audio_resampler.cpp` | 比例 1 ± 0.005：1 kHz 的信噪比不低于 80 dB，18 kHz 以内幅度偏差不超过 0.1 dB；比例连续变化时没有跳变 |
| `tests/test_audio_jitter.cpp` | 用假时钟：0 / 5 / 20 / 40 ms 的抖动下目标延迟收敛到“抖动 + 5 ms”（限制在 20–60），没有欠载；丢 1% 的包只触发补偿，带 RED 时不触发；乱序和重复的包；一次 300 ms 的停顿后追回目标，只有一次交叉淡化；±200 ppm 的时钟差跑模拟的 10 分钟：水位在目标 ±5 ms 内，没有欠载，没有丢帧插帧；声音段开头不丢样本（输出和输入对齐后从第一个可闻样本起相关系数不低于 0.9） |
| `tests/test_reliable_transport.cpp`（加） | 数据报到达且不重传（模拟 5% 丢包：收到的约 95%，没有一个出现两次）；画面占满带宽时数据报的排队延迟 p99 不超过 20 ms；数据报计入在途和带宽估计（纯数据报的流量下估计不为 0）；100 ms 没发出去的被丢弃；对端不支持时不发 |
| `tests/test_secure_transport.cpp`（加） | 数据报能加解密；改一个字节被丢弃且连接不断；重放被丢弃；乱序 100 个以内都收；密钥没协商完时不发 |
| `tests/test_session.cpp` / `test_capi.cpp`（加） | `rda_host_submit_audio` → 模拟网络 → `rda_viewer_read_audio`：听到的和喂进去的对得上（延迟在 30–120 ms，频带能量如上）；对端没有 `kCapAudio` 时状态是 `WAITING_PEER`、一个声音包都没有；旧 `struct_size` 的 `rda_viewer_config` 不读 `audio`；统计各项和实际发的一致，`bytes_sent` 含声音 |
| `fuzz/fuzz_audio.cpp` | 任意字节进 `Receiver::OnPacket` + `Read` 不崩 |
| `examples/netsim.cpp` | 新场景 `--scenario=audio`（画面静止 + 测试信号）和任意场景加 `--audio`：输出声音的 kbps（统计口径和线上）、延迟、欠载次数、补偿的帧数；`tools/netsim_matrix.py` 加一组（0 / 1 / 5% 丢包 × 0 / 20 ms 抖动 × 1 / 10 Mbps） |

### 12.3 macOS 上的采集和播放

- `tests/test_platform_audio_mac.mm`：
  - 时钟：`mHostTime` 的换算和 `MonotonicMicros()` 相差小于 1 ms（不碰设备）。
  - 播放器空设备模式：5 秒里取到的帧数在 48000 × 5 的 ±1% 内；`RDACCEL_AUDIO_NULL_OUTPUT=1` 时即使 `null_device = 0` 也不建 AudioUnit（测试替身计数）。
  - tap（默认跳过，`RDACCEL_TEST_AUDIO_TAP=1` 才跑；只用 `CATapUnmuted`，不放任何声音）：建 tap 和聚合设备成功；格式是浮点；2 秒里回调的时间戳单调；拆掉后 `kAudioHardwarePropertyTapList` 里没有我们的 tap。**不**断言样本全是 0（lroel 那时可能正在放东西），样本看完就丢。没有权限时测试报告“跳过：没有权限”，不算失败。
- 真实的输出设备、让本机静音的行为：不测，见第 13 节。

### 12.4 rdmachine

| 文件 | 检查什么 |
| --- | --- |
| `core/tests/test_messages.cpp`、`test_server_session.cpp` | `audio` 字段的编解码；修订 2 的对端：请求里的 `audio` 被当成 false，回复里没有 `audio` |
| `core/tests/test_audio_control.cpp`（新） | `0x10013` 的编解码，缺字段、多字段 |
| `core/tests/test_session_stats.cpp` | 新字段，旧报文照常解 |
| `session/tests/test_storage.cpp` | 两个设置的读写和默认值 |
| `session/tests/test_media_e2e.cpp` | `SoundReachesTheClient`：`audio_source` 用测试信号、客户端用空设备，5 秒内 `rda_viewer_stats.audio_packets > 0`，统计报文里 `audio:"on"` 或 `"silent"`；`NoSoundForAClientThatDidNotAsk`；`PausedSendsNothing`；`DataSaverSwitchesTheProfile`（切文字模式后 `rda_audio_status.frame_ms` 从 10 变 20）；`NoSoundAtTheLoginWindow`；`AnOlderClientGetsNoSound` |
| `session/tests/test_daemon_e2e.cpp` | `audio` 经守护进程到 agent、`grant` 带回；迁移后的 `session_ready` 带着对的值 |
| `tools/smoke_test.sh` | `rdmachine-connect --audio`（空设备）对 `--headless --audio-source=test` 的服务端：退出时打印的声音包数大于 0 |

## 13. 人工验收（lroel 做，实现者不做）

在愿意出声的时候做，结果记进本文：

1. 被控端放音乐，连上：客户端有声音；被控端自己不出声（设置默认）。断开：被控端声音立刻回来。
2. 关掉“传声音时这台 Mac 自己不出声”再连：两边都有声音。
3. 连着的时候 `kill -9` 用户 agent：被控端声音回来；几秒后会话恢复，客户端声音回来。
4. 被控端把音量调到很小或按静音键：客户端的音量不变（验证“tap 在设备音量之前”）。
5. 放一段视频：声音和口型对得上；浮窗里的 `A/V` 偏差在 0–80 ms。
6. 切到省流量模式听同一段音乐：听不出差别（D1 的验证）；浮窗的声音码率下降。
7. 暂停音乐：浮窗显示 `Audio: silent`，码率到 0；继续播放时开头不缺。
8. 客户端按静音键：浮窗显示 `Audio: paused`；被控端仍然不出声。
9. 被控端插拔耳机或换输出设备：客户端的声音在 1 秒内继续。
10. 第一次安装后的授权：询问出现、点允许后不用重连就有声音；拒绝后浮窗的提示正确。
11. 退出登录到登录窗口再登录回来：登录窗口没有声音，回到桌面后声音恢复。

## 14. 分阶段实现

每个阶段单独提交、单独能过全部测试；rdaccel 的阶段在 rdaccel 仓库里做。

| 阶段 | 内容 | 完成标准 |
| --- | --- | --- |
| 0 | 第 2、3 节标了【待验证】且不需要声音的几项：tap 只建不读会不会弹询问、只有 tap 的聚合设备能不能跑、没授权时读到什么、有屏幕录制权限是否就够、设置页的锚点；核对 Opus 的版本和哈希 | 在临时目录里做（不进仓库的构建目录），只用 `CATapUnmuted`，不保存样本；做之前告诉 lroel 可能弹一次权限询问；结果写回本文对应的位置，去掉【待验证】或改设计 |
| 1 | rdaccel：数据报（6.1）、它的加密（6.2）、`kCapAudio`（6.3）、码率控制里扣掉声音 | 12.2 里传输和加密的新测试通过；现有测试和 `netsim_matrix.py` 的数字没有变化（没有数据报时行为逐字节不变） |
| 2 | rdaccel：Opus 依赖、包格式、静音门、单声道判断、冗余、发送端、抖动缓冲、重采样、接收端、C 接口（`submit_audio` / `read_audio` / 状态 / 统计）、测试信号源、netsim 的声音场景、fuzz | 12.2 全部通过；universal 构建通过；`abi_check.sh` 只报新增；用 netsim 测出 11.3 里测试信号对应的数字并回填 |
| 3 | rdaccel：Process Tap 采集、播放器（真设备 + 空设备）、`RDA_AUDIO_SOURCE_SYSTEM`、权限状态；`examples/serve`、`view` 加 `--audio`（`view` 默认空设备，`--audio-device` 才开真设备） | 12.3 通过；`grep -rnE "afplay|NSSound|NSBeep|AudioServicesPlay|AVAudioPlayer|\bsay\b" tests tools examples` 没有结果；`grep` 不到对音量和静音属性的写操作；rdaccel 1.13.0 的 CHANGELOG 和文档改完 |
| 4 | rdmachine：最低系统 14.2、修订 3、设置、`MediaHost`、`RemoteSession`、客户端菜单和浮窗、服务端菜单和权限、Info.plist、安装包的检查脚本 | 12.4 通过；`tools/smoke_test.sh` 通过；pkg 能装、`rdmachine-service status` 正常；文档改完；交给 lroel 做第 13 节的 1–5、7、9–11 |
| 5 | 客户端听不到时暂停（5.6）、链路很慢时降码率（5.5）、A/V 对齐（7.4）、按 lroel 实际使用的数字回填 11.3 | 对应的单元测试；第 13 节的 6、8；11.3 的表更新为实测 |
| 6 | TCP 上的声音（6.4），如果 lroel 要 | `test_tcp_transport.cpp` 和 e2e 里 TCP 的一例；UDP 的数字不变 |

## 15. 版本和文档

- **rdaccel 1.13.0**（次版本：C 接口只有新增）。`rdaccel.h` 的版本宏、`CMakeLists.txt`、`CHANGELOG.md`、`NOTICE`（Opus）；最低 macOS 14.2（`CMakeLists.txt` 里的 14.0 两处、`docs/integration.md` 的平台表、`docs/building.md`）；`docs/design.md` 范围表里“音频”从“暂不做”移到“库”（采集、编码、传输、抖动缓冲）和“可选模块”（播放器）；`docs/architecture.md` 加“声音”一节、通道表后面加数据报、代码布局加 `core/audio/`；`docs/integration.md` 加声音的用法、线程和权限。
- **rdmachine 4.4.0**。`CMakeLists.txt`（版本、14.2）、子模块指到 rdaccel 1.13.0；`docs/decisions.md`：“音频”一行改成新的决定并指向本文，“系统版本”一行改成 14.2 和原因（Process Tap），“rdaccel 改动”的边界不变；`docs/protocol.md`：标题和开头的修订说明加修订 3、`session_request` / `session_ready` 的表、新的“声音”一节（消息 `0x10013`）、“会话统计”的字段、“以后会加的”；`README.md`；安装包的 `welcome.html`。本文标题里的“设计，未实现”在第 4 阶段完成后去掉，【待验证】和【lroel 定】换成结果。
- 发布照旧：两个仓库都提交、两边都推，只发 pkg。GitHub 是公开的：测试信号都是算出来的，不提交任何录下来的声音。

## 16. lroel 的决定（2026-11-16，都按推荐）

| 编号 | 问题 | 决定 |
| --- | --- | --- |
| D1 | 省流量模式的立体声码率：128 / 96 / 64 kbps（11.3 的表） | 96 kbps：唯一一项“感知上听不出”而非“确定无损”的节省 |
| D2 | 省流量模式的帧和包：20 ms 一帧一包；40 ms 一包；还是和普通模式一样 10 ms | 20 ms 一帧一包 |
| D3 | 本机静音的范围 | 只在声音真的在传的会话里静音，客户端暂停期间也保持；登录窗口、旧客户端、客户端关了声音的会话不静音 |
| D4 | 播放器放在 rdaccel 还是 rdmachine | rdaccel 的可选模块（`rda_audio_player`） |
| D5 | 用 TCC 的私有函数查询和申请权限 | 用（`dlopen` + `dlsym`），找不到符号时退回靠样本判断 |
| D6 | 客户端静音或音量为 0 时自动暂停发送 | 做 |
| D7 | 静音门限 | −90 dBFS（2⁻¹⁵）、尾巴 200 ms |
| D8 | Opus 由 rdaccel 在配置时下载，universal 构建关掉内联指令 | 下载（FetchContent），关掉内联指令 |
| D9 | TCP 上要不要声音（第 6 阶段） | 先不做 |
| D10 | 声音早于画面超过 20 ms 时最多推迟 120 ms 去对齐 | 做 |
| D11 | 普通模式 10 ms 一帧、160 kbps | 如上 |
| D12 | 第 0 阶段在这台 Mac 上弹一次“系统声音录制”的询问；第 13 节由 lroel 在能出声时亲自验 | 同意；询问已弹过一次，lroel 点了允许（见第 2 节） |

## 17. 实现记录（2026-11-16）

第 0–5 阶段按本文实现；第 6 阶段不做（D9）。下面是实现时和上文不同的地方（上文相应的位置已经改了的也列在这里），以及实测的数字。

### 和设计不同的地方

| 哪里 | 设计 | 实现 | 为什么 |
| --- | --- | --- | --- |
| 2、3：权限 | 每 2 秒查 preflight，变成 0 就换静音行为；先试着改现有 tap 的描述 | 读到非零样本或 preflight 为 0 都算有权限；没确认前每 5 秒重建一次 tap；换静音行为一律拆了重建 | 第 0 阶段实测 preflight 在一个进程里按缓存回答，运行中给的授权看不到；改 tap 描述的路径要用让本机静音的 tap 才能验，这台 Mac 上不能试 |
| 3：进程列表变化 | 出现要排除的应用就重建 | 只在排除的进程集合真的变了时重建 | 进程列表随便哪个应用开始放声音都会变；重建时本机会漏出一瞬间自己的声音（静音时），不该为别的应用这样做 |
| 3：时间戳 | `mHostTime` 直接换算，和 `MonotonicMicros()` 同一个钟 | 换算时加上“此刻 `steady_clock` − 此刻 `mach_absolute_time`” | `steady_clock` 读的是 `CLOCK_MONOTONIC_RAW`；这台没睡过眠的 Mac 上两者一样，睡过眠的 Mac 上可能差出睡眠时长 |
| 5.3：单声道 | 连续 500 ms L = R 就按单声道编 | 只数声音段里的帧（静音的帧不算），每段从立体声开始 | 测出来：Opus 强制单声道、10 ms 一帧时，起音的头 2 ms 被压低 30 dB 和 9.5 dB（未经我们重置的编码器也一样），过了 500 ms 静音的第一帧正好是这种情况；20 ms 一帧没有这个问题 |
| 6.1：数据报在画面满载时的延迟 | p99 排队不超过 20 ms | 测试按光标通道的标准：中位数比空闲时多不到 20 ms，95 分位多不到 70 ms；实测中位数多约 15 ms、99 分位多约 30 ms | 数据报绕过了发送端自己的队列，剩下的是画面留在网络瓶颈里的排队，和画面自己的包一样，要动拥塞控制才能再少 |
| 6.1：带宽采样 | 数据报算进投递速率 | 照算；但确认里有数据包时，带宽采样用最新的数据包，不用数据报 | 数据报不经发包间隔、算应用受限，拿它采样时起步阶段一直看不到“带宽不再增长”，起步的排队站了 3 秒多（RTT 120 ms 而不是 50 ms） |
| 7.1：接收环满了 | 丢最旧的 | 丢新来的（并计数） | 单写单读的无锁环里写的一方不能安全地丢读的一方正在读的那个；环满只发生在长时间没人读（播放器停了）时，那时哪一个都已经过时 |
| 7.2：晚到 | 已经播过位置的帧计“晚到” | RED 带来的、已经播过的副本不计 | 每个带 RED 的包都会带来一份已经播过的帧，算进去“晚到”就没有意义 |
| 7.3：漂移控制的水位 | 每 100 ms 取一次水位，1 秒平均 | 每个包到达时取水位（按到达时刻修正），减去半帧，1 秒平均 | 水位每来一帧涨一帧、之间往下走；固定间隔取样把这个锯齿混叠成随漂移慢慢游走的偏差（模拟里 ±200 ppm 时最大偏离 9.7 ms，按到达取样后在测试精度内为 0） |
| 7.5：播放器的静音查询 | 只读地报告默认输出是否静音 | 真设备照做；空设备报 −1（不知道） | 空设备本来就不出声，测试里不应依赖这台 Mac 的音量 |
| 12.2：会话测试 | `test_session.cpp` / `test_capi.cpp` | `test_capi.cpp`（只用公开的 C 接口） | 一处就够 |
| 12.3：tap 拆掉后 | `kAudioHardwarePropertyTapList` 里没有我们的 tap | 轮询到 tap 数回到建之前（最多 1 秒） | 私有 tap 只有建它的进程看得见，拆掉后列表要稍后才更新（实测 300 ms 内） |
| 12.4：smoke test | 加 `--audio` | 另外给服务端加了 `--standalone`、两端都 `--clipboard=off` | 这台 Mac 装了守护进程（`/var/run/rdmachine/agent.sock` 存在）时，不带 `--standalone` 的测试服务端会当成那个守护进程的 agent 连上去；剪贴板不在这个测试里检查，不碰系统剪贴板 |
| 测试的环境变量 | CTest 给 rdaccel 的每个测试设 `RDACCEL_AUDIO_NULL_OUTPUT=1` | rdmachine 的两个测试程序（core、session）也设了 | 同一个理由 |

### 实测（模拟网络，测试信号；都不出声）

- **编码**：多音信号经编解码后 12 个有能量的频带平均差 0.02–0.04 dB，信噪比 23–27 dB，编解码延迟 120 个样本（2.5 ms）；起音（1 秒静音后的 1 kHz）头 5 ms 的能量差 ±0.05 dB，之前没有超过 −200 dBFS 的东西。多音 + 噪声的码率：普通模式 Opus 数据 181 kbps（目标 160），省流量 114 kbps（目标 96），都在 ±25% 内。这台 Apple Silicon 上编码是实时的 260 倍（普通）和 300 倍（省流量），关掉内联指令也够（要求 50 倍；x86_64 上没测）。
- **重采样器**：比例 1 ± 0.005 时 1 kHz 的信噪比 102.7 dB，到 18 kHz 幅度偏差小于 0.001 dB。
- **抖动缓冲**：抖动 0 / 5 / 20 / 40 ms 时目标延迟 20 / 20 / 25 / 44 ms；40 ms 抖动时开头 2 秒（目标还在起始的 30 ms 时）补偿了 32 帧，之后没有；1% 丢包补偿 17 帧，带 RED 时 0 帧；300 ms 停顿后追回一次、欠载一次；±200 ppm 跑 10 分钟控制量平均 ±200 ppm，没有欠载、追赶和补偿。
- **数据报**：画面占满 4 Mbps 时见上表。
- **11.3 里测试信号对应的数字**（`tools/netsim_matrix.py --audio`，测试信号 4 秒里约 2 秒有声音，统计口径 / 线上，线上按每包 56 字节算）：普通模式 108 / 131 kbps，省流量模式 73 / 85 kbps；路径上有 1% 以上的丢包、冗余打开时普通模式 162–182 / 186–205 kbps，省流量约 120 / 132 kbps。1 Mbps 的路径上画面起步时会把瓶颈排满，声音在开头几秒有 1–2 次欠载、十来帧补偿（10 Mbps 上没有）。lroel 真实使用的数字（第 5 阶段要回填 11.3 的）还没有：要在真正连着用的时候从浮窗读（“声音”一项）。
