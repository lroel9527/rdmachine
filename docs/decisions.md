# 第一阶段的决定

2026-11-06 和 lroel 一起定下，详细的备选方案和取舍见方案文档。

| 编号 | 决定 | 要点 |
| --- | --- | --- |
| 范围 | 第一阶段只做 Mac ↔ Mac，Intel 和 Apple Silicon 都支持 | Linux、Wayland、Windows、浏览器、移动端放到后续阶段 |
| D1 | 服务端：平台无关的 C++20 核心（`core/`）+ Objective-C++ 的 Mac 外壳 | 核心以后给 Linux 复用；认证解析代码配 fuzz |
| D2 | 客户端：原生 AppKit | 在 rdaccel 的 `examples/remote_mac` 基础上扩展；界面逻辑尽量放进核心 |
| D3 | 控制连接：TLS 1.3 + 首次确认证书指纹 | M0 支持系统账号密码和配对码；每次会话生成新的 rdaccel 密钥（[协议](protocol.md)） |
| D4 | TCP 4100 控制 + rdaccel UDP 画面，UDP 不通退回 rdaccel TCP | 换网络不断线以后靠 rdaccel 加连接 ID |
| D5 | M0 只控制已登录用户的物理桌面，M1 补登录窗口和锁屏 | 无显示器的虚拟屏放到 M2 实验 |
| 系统版本 | 最低 macOS 12.3 | 为了 2016 款 Intel MacBook Pro；rdaccel 从 1.0.1 起支持（[rdaccel#39](https://github.com/lroel9527/rdaccel/issues/39)），rdmachine 的子模块现在用 1.4.1 |
| 签名 | 暂无 Apple Developer 账号，M0 用本机证书签名自测 | 不做公证分发；用 `packaging/macos/create-signing-identity.sh` 建一张固定的自签证书，应用标识固定为 `org.rdmachine.server` / `org.rdmachine.client`，重新编译后权限不丢 |
| 音频 | 第一阶段不做声音 | 不采集、不传输声音，不碰两端的音量（rdaccel 的屏幕采集不收声音：macOS 13 起显式设 `capturesAudio = NO`，12.x 没有这个属性）；测试也不涉及声音 |
| 服务端界面 | 菜单栏应用，不静默运行（2026-11-09） | 菜单栏图标显示状态、配对码、指纹、权限，提供断开和设置；调试数据浮窗在客户端（远程窗口右上角，点击穿透），服务端每秒发统计过去；`--headless` 给脚本和自动测试用 |
| rdaccel 改动 | 在 rdaccel 仓库里改，边界不混 | rdaccel 只放通用的采集、编码、传输和测量（例如 `rda_host_stats` 的丢包计数）；登录、会话、界面、设置、统计的展示都在 rdmachine。rdmachine 以 submodule 固定在 rdaccel 的版本上 |
