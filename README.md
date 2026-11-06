# rdmachine

开源的远程桌面应用，体验对标 NoMachine。画面采集、编码和弱网传输由 [rdaccel](https://github.com/lroel9527/rdaccel) 提供，rdmachine 负责其上的产品层：服务端、登录认证、会话管理和客户端。

第一阶段只支持 Mac ↔ Mac（Intel 和 Apple Silicon，macOS 12.3+）。现在处于 M0（可用原型）开发中，还不能用。

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
# 其他系统只构建和测试协议核心：加 -DRDM_WITH_RDACCEL=OFF
```

## 目录

```text
core/              平台无关的产品核心（C++20）：控制协议、TLS 1.3 控制连接（mbedTLS）、登录与会话状态机
server/            Mac 服务端（Objective-C++）
client/            Mac 客户端（AppKit）
third_party/rdaccel  rdaccel（git submodule，固定在发布版本）
docs/              决定、协议和调研文档
```

## 许可

[Apache License 2.0](LICENSE)
