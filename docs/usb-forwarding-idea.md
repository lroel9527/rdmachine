# USB 转发（搁置的想法）

2026-11-20 和 lroel 讨论的想法：让客户端和服务端互相使用对方的 USB 设备，像网络加密狗那样。讨论到最后决定**不做**：市场小，两头都有成熟产品，硬件投入不划算。这里记下结论，以后想重拾时不用从头查。

## 目标

- 客户端插的 USB 设备，在服务端像本机插上一样使用，反过来也可以。
- 想转发的典型设备：串口（ESP32、Arduino 一类）、JTAG 调试器、加密狗、U 盘。
- 不在范围：摄像头、声卡这类等时传输（isochronous）设备。

## 两种做法

### 方向一：原样转发 USB（URB 级别，类似 USB/IP、VirtualHere）

插设备的一端**接管**设备，用设备的一端**冒充**设备。

**接管端（Mac）：可行**

- `IOUSBHostDevice` 加上 `IOUSBHostObjectInitOptionsDeviceCapture`，root 权限就够，不需要授权（SDK 的 `IOUSBHostDefinitions.h` 写明：有 root 时不需要 `com.apple.vm.device-access` 授权，也不需要 `IOServiceAuthorize`）。
- 接管会把这个设备上 macOS 自己的驱动全部踢掉。

**冒充端（Mac）：卡在 Apple 授权**

- 官方有用户态接口 `IOUSBHostControllerInterface`（IOUSBHost 框架）。头文件里写的用途就是"provide access to remote USB devices"：进程创建一个虚拟 USB 主机控制器，macOS 会在上面枚举设备，不需要 kext。
- 要求 `com.apple.developer.usb.host-controller-interface` 授权。Apple 开发者技术支持在论坛（thread 802495）说它是受限而且受管的授权，要通过 Feedback Assistant 申请。到 2026-11 为止，论坛里没有人报告获批。而且我们目前没有付费开发者账号（见 decisions.md 的"签名"）。
- 绕过办法：关 SIP，在 Apple Silicon 上很可能还得加启动参数 `amfi_get_out_of_my_way=1`（相当于关掉整台机器的代码签名检查）。没有实测。这样做只适合自用，发给别人时每个用户都要这么做。
- kext 方案（VirtualHere 的做法）：Apple Silicon 上要在恢复模式里降低安全性，驱动有 bug 会让内核崩溃，不考虑。
- 难度：这个接口要在用户态实现一个类似 xHCI 的控制器，控制器、端口、设备、端点各有一套状态机，资料只有头文件注释，没有示例，也没人公开跑通过。

### 方向二：按设备类别转发

不转发原始 USB，在对端用系统允许的方式模拟出同类设备。

| 设备 | 做法 |
| --- | --- |
| U 盘 | 文件传输（RDMachine 已经有） |
| 摄像头 | CoreMediaIO Camera Extension |
| 麦克风 | 反向音频，虚拟一个输入设备 |
| 串口 | 伪终端（pty）最简单，但波特率、DTR/RTS 传不过去，ESP32 和 Arduino 没法自动复位进下载模式；要完整支持得写 DriverKit 虚拟串口（SerialDriverKit），授权申请还没查实 |
| JTAG | 没法按类别模拟。直接用 OpenOCD 或 J-Link Remote Server：服务端通过 Tailscale 连客户端的端口（比如 `target extended-remote 100.x.x.x:3333`），用不着 RDMachine |

## 硬件替身方案

在冒充端插一块能当 USB 设备用的小板，由它冒充远端设备。Mac 看到的是真设备插在真 USB 口上，所以不需要授权，也不用关 SIP。

### 最终形态（讨论稿）

- 每端一个盒子，里面是一颗 hub 芯片，下游挂控制通道加若干设备槽，再留几个普通 USB 口。Mac 上只占一个口。
- 接管统一由 Mac 软件以 root 身份完成，不管设备插在 Mac 自己的口上还是盒子的普通口上，所以盒子里不需要 USB 主机控制器。
- 一个盒子就能单向用（设备在没盒子的那台，用设备在有盒子的那台），两个盒子就能双向。
- 防止转发成环：盒子本身、它的控制通道、槽里冒充出来的设备，都不能出现在"可转发"列表里。
- 控制通道有两种走法：
  - **走 USB**：只插一根线；但每个槽要额外占一个 USB 控制器，数据在线上走两遍，设备实际约有 20 MB/s 可用。
  - **走网络**：槽可以多一个，带宽接近完整的 USB 2.0；代价是盒子要接网线。
- 一个设备控制器只能冒充一个设备，因为普通控制器只认一个总线地址。要一颗芯片演出 hub 加多个设备，得用 FPGA 自己实现，据我所知没有现成实现。
- 设备槽的速度不能低于被冒充的设备：全速槽演不了高速设备。

### 芯片和开发板（2026-11 的调查）

| 选项 | 价格（约） | 情况 |
| --- | --- | --- |
| FRDM-MCXN947 | 22–25 美元 | 1 个高速设备口，有 RJ45 网口，板载 MCU-Link 调试器。"控制走网络、一个槽"方案里性价比最高 |
| Teensy 4.1（RT1062） | 32 美元，网口套件另加约 10 美元 | 只有一个设备口，第二个口是主机口；没有引出 SWD 调试接口 |
| MIMXRT1170-EVKB | 一两百美元 | 两个 OTG 口，适合验证"控制和槽都走 USB"；两个口同时当设备没找到实测报告 |
| FRDM-IMX93 | 约 100 美元，另加 PD 电源 | Linux 加 raw-gadget 开发最快；第二个口是 Type-A 主机口，所以只有一个槽 |
| MCIMX93-EVK | 约 620 美元 | 两个 USB-C 数据口，太贵 |
| i.MX6ULL 第三方板 | 便宜 | 芯片有两个高速 OTG，要看板子是不是两个都按 OTG 引出 |
| NUCLEO-U5A5ZJ-Q | 约 30 美元 | 内置高速 PHY，没有网口（W5500 模块又太慢） |
| NUCLEO-N657X0-Q | 约 55 美元 | 一个高速口加 RJ45；要从外部 Flash 启动 |
| Zynq-7000 | 不等 | PL 要外接 ULPI PHY；Vivado 不能在 Apple Silicon 上跑；hub 加多设备的逻辑得自己写 |

### 价格压到最低：ESP32 加 WiFi

| 芯片 | USB | WiFi | 能不能当设备槽 |
| --- | --- | --- | --- |
| ESP32 原版、C3、C6 | 没有 OTG，或者只有固定功能的 USB 串口/JTAG | 有 | 不能 |
| ESP32-S3（S2 类似） | OTG 全速，12 Mbps，Synopsys DWC2 系 | 有 | 能，但只能冒充全速设备 |
| ESP32-P4 | USB 2.0 高速 OTG，PHY 集成在芯片里 | 芯片本身没有，板子上一般搭一颗 C6 | 能冒充高速设备 |

- **ESP32-S3**：开发板约 5–10 美元，做成成品也差不多这个价（一块模组，加一个 USB 口，再加外壳）。
  - 软件用 TinyUSB：运行时生成描述符，自己处理控制请求，主动断开再重连。冒充任意全速设备所需的能力都有。
  - 全速 USB 最多约 1 MB/s，S3 的 WiFi 实际能跑每秒 1–2 MB，够用。
  - 盒子只和旁边的 Mac 在同一个局域网里通信，由 Mac 通过 RDMachine 或 Tailscale 转发到对面，盒子上不需要跑 Tailscale。
  - 能冒充：CH340、CP2102、FT232R 这类串口转换芯片，很多加密狗、键盘、鼠标，便宜的 CMSIS-DAP 调试器，ST-Link V2。
  - 冒充不了：高速设备（新版 J-Link、FT2232H 类调试器、U 盘、移动硬盘），以及端点很多的设备（S3 大约只有 5–6 个端点）。
  - WiFi 延迟高而且会抖动，要关掉 WiFi 省电模式。JTAG 这种频繁一来一回的小包操作会明显变慢，串口、加密狗影响不大。
- **ESP32-P4 加 C6**：开发板约 20–30 美元（以商家报价为准），能冒充高速设备；但 WiFi 经过 C6 中转，实际只有每秒几 MB，高速设备能用但跑不满。
- 如果主要转发串口（ESP32、Arduino 一类），S3 方案能把"便宜、Mac 免驱、能跨网络"压到十几美元一个盒子，Icron 那类要四五百美元。

挑开发板的要点：USB 口直接连在芯片的控制器上（中间没有 hub 芯片或转串口芯片）；VBUS 不是固定对外供电；有以太网 PHY；能用 SWD 调试；能单独供电；原理图公开。

## 市场

- **纯软件**：VirtualHere、FlexiHub / USB Network Gate。用设备的 Mac 要装内核扩展，Apple Silicon 上有装不上、新系统上崩溃的报告。
- **成对硬件，免驱**：Icron USB 2.0 Ranger 2304GE-LAN（约 430–580 美元）、StarTech USB 延长器。支持 macOS，大概率只能在同一个局域网里用。
- 空档是"便宜、Mac 免驱、能跨网络"（用 ESP32-S3 一个盒子能压到十几美元，只能冒充全速设备），但买家很少：加密狗常被许可协议禁止网络共享；嵌入式开发已经有免费的远程调试工具；U 盘有文件传输替代。
- 做硬件还有固定成本：设备兼容性和售后、FCC/CE 认证、USB 厂商 ID（向 USB-IF 申请约 6000 美元，开源项目可以用 pid.codes）。
- 结论：不值得当商业产品做。要做的话，就当开源硬件，在 Crowd Supply 或 Tindie 上用众筹检验需求。

## 重拾时从哪开始

1. 用 ESP32-S3（最便宜，只能冒充全速设备）或 FRDM-MCXN947（能冒充高速设备），加一个 CH340 串口小板，做自环测试：串口小板插在 Mac mini 上，被接管后经板子冒充，再插回同一台 Mac mini。这样不用客户端，也不用人在现场插拔。
2. 如果 Apple 批了 `usb.host-controller-interface` 授权，就可以不要硬件、纯软件实现，先写一个小探测程序验证。
