# rdmachine

开源的远程桌面应用，体验对标 NoMachine。画面采集、编码和弱网传输由 [rdaccel](https://github.com/lroel9527/rdaccel) 提供，rdmachine 负责其上的产品层：服务端、登录认证、会话管理和客户端。

第一阶段只支持 Mac ↔ Mac（Intel 和 Apple Silicon，macOS 12.3+）。现在处于 M0（可用原型）开发中：命令行服务端和 Mac 客户端窗口已经有了，还没在真机上验证过。

## 文档

| 文档 | 内容 |
| --- | --- |
| [第一阶段的决定](docs/decisions.md) | 范围、语言、界面、认证、端口、会话模型 |
| [控制协议 v0](docs/protocol.md) | 登录、密钥下发、会话控制的线上格式 |
| [NoMachine 功能清单](docs/research/nomachine-features.md) | 调研：NoMachine 有哪些功能，P0/P1/P2 |
| [rdaccel 能力评估](docs/research/rdaccel-assessment.md) | 调研：rdaccel 能做到什么、缺什么 |
| [同类开源方案](docs/research/competitive-analysis.md) | 调研：RustDesk、VNC、xrdp、Guacamole、X2Go、Sunshine |

## 构建

```bash
git clone --recursive https://github.com/lroel9527/rdmachine && cd rdmachine
# macOS：Xcode 命令行工具 + CMake。通用二进制（Intel + Apple Silicon）：
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build -j
ctest --test-dir build
```

除了 Mac 客户端窗口，其余部分也能在 Linux 上构建和测试（CI 就是这样跑的）。需要 FFmpeg、X11 和 PAM 的开发包：

```bash
sudo apt-get install ninja-build pkg-config xvfb libavcodec-dev libavutil-dev libswscale-dev libx265-dev \
  libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxtst-dev zlib1g-dev libpam0g-dev
cmake -S . -B build -G Ninja && cmake --build build
xvfb-run -a ctest --test-dir build      # 端到端测试会采集 Xvfb 的屏幕
# 只构建协议核心：加 -DRDM_WITH_RDACCEL=OFF
```

## 试用（M0）

被控的 Mac 上运行服务端（第一次运行时 macOS 会要求给运行它的程序，比如“终端”，授予“屏幕录制”权限；控制键鼠还需要“辅助功能”权限，没有的话只能看不能控）：

```bash
./build/bin/rdmachine-server --pairing-code
# 打印本机指纹和一个 8 位配对码；按回车换新的配对码
```

另一台 Mac 上打开 `build/bin/rdmachine-client`，输入服务端地址，用被控 Mac 当前登录账号的密码，或者用配对码登录。第一次连接会显示服务端指纹，和服务端打印的核对一致后再信任。

不开窗口也能测试连接：`./build/bin/rdmachine-connect 服务端地址 --code=配对码`（或 `--user=账号`，密码从终端输入）。

端口：TCP 4100 是控制连接，UDP 4100 是画面，UDP 不通时画面改走 TCP 4101。

## 许可

[Apache License 2.0](LICENSE)
