# rdmachine

开源的远程桌面应用，体验对标 NoMachine。画面采集、编码和弱网传输由 [rdaccel](https://github.com/lroel9527/rdaccel) 提供，rdmachine 负责其上的产品层：服务端、登录认证、会话管理和客户端。

第一阶段只支持 Mac ↔ Mac（Intel 和 Apple Silicon，macOS 12.3+）。现在处于 M0（可用原型）开发中：服务端（菜单栏应用，也能在命令行跑）和 Mac 客户端窗口已经有了，还没在两台真机之间验证过。

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

除了 Mac 客户端窗口，其余部分也能在 Linux 上构建和测试。需要 FFmpeg、X11 和 PAM 的开发包：

```bash
sudo apt-get install ninja-build pkg-config xvfb libavcodec-dev libavutil-dev libswscale-dev libx265-dev \
  libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxtst-dev zlib1g-dev libpam0g-dev
cmake -S . -B build -G Ninja && cmake --build build
xvfb-run -a ctest --test-dir build      # 端到端测试会采集 Xvfb 的屏幕
# 只构建协议核心：加 -DRDM_WITH_RDACCEL=OFF
```

## 打包和签名（macOS）

没有 Apple Developer 账号时，用本机自签的证书签名。macOS 按“证书 + 应用标识”记住屏幕录制和辅助功能权限，所以证书固定下来以后，重新编译打包的应用不用再授权；不签或临时签名（ad hoc）的话，每次重编都得重新授权。

1. **每台用来编译的 Mac 做一次**：创建签名证书。中途会弹窗要登录密码（把证书设为“代码签名”可信），之后第一次签名时如果问是否允许 codesign 使用钥匙串，点“始终允许”。

   ```bash
   packaging/macos/create-signing-identity.sh
   # 在登录钥匙串里创建 “rdmachine Local Signing”，再运行一次不会重复创建
   ```

   这张证书要留着：删掉重建的话，两台 Mac 上的权限都要重新授予。想换到另一台 Mac 编译，在“钥匙串访问”里连私钥一起导出、到那台导入即可。

2. **编译并打包**（通用二进制，Intel 和 Apple Silicon 通用）：

   ```bash
   packaging/macos/package.sh --build --zip --dmg
   ```

   产物在 `dist/`：

   | 文件 | 用途 |
   | --- | --- |
   | `RDMachine.app` | 客户端 |
   | `RDMachine Server.app` | 服务端（菜单栏应用，没有 Dock 图标） |
   | `rdmachine-server.sh` | 在终端里启动服务端，输出和配对码显示在终端 |
   | `rdmachine-connect` | 命令行测试客户端 |
   | `rdmachine-版本号-macos.zip` | 以上全部，拷到另一台 Mac 用 |
   | `RDMachine-版本号.dmg` | 安装盘：打开后把两个应用拖进“应用程序”，命令行工具在 `Command Line Tools` 文件夹里 |

   已经编译过的话去掉 `--build`，默认从 `build/bin` 取二进制（`--build-dir` 可改）。`--identity -` 是临时签名，只适合一次性试试。

3. **拷到另一台 Mac**：用 U 盘、`scp` 或 AirDrop 拷 dmg 过去，打开后把应用拖进“应用程序”；或者拷 zip 过去解压。自签证书不被别的 Mac 认可，第一次打开会被拦下：macOS 12–14 在应用上右键“打开”；macOS 15 起去“系统设置 → 隐私与安全性”底部点“仍要打开”。也可以直接 `xattr -dr com.apple.quarantine RDMachine*.app` 去掉下载标记。

## 试用（M0）

被控的 Mac 上双击打开 `RDMachine Server.app`，菜单栏会出现一个显示器图标，点开可以：

- 看当前状态：监听的端口、有没有人连着（谁、从哪个地址、走 UDP 还是 TCP、连了多久），以及“断开当前连接”；
- 生成配对码（自动复制，5 分钟内有效，用一次就失效）、复制本机指纹；
- 看屏幕录制和辅助功能权限是否已经允许，没允许的话点一下打开对应的系统设置；
- 打开“显示调试浮窗”：屏幕右上角一个半透明的小窗，每秒刷新连接时长、通道（UDP/TCP）、分辨率、帧率（发送/采集）、码率、带宽估计、延迟、丢包/重传和编码器。浮窗可以拖动，右键可以隐藏。它会跟着画面一起传到客户端，在客户端那边调试时也能看到；
- “设置…”：端口、是否允许配对码、只看不控、剪贴板方向、帧率和码率上限、调试浮窗。保存在 `~/Library/Application Support/rdmachine/server-settings.json`；改了端口等设置后服务会重新监听，正在进行的连接会断开。

想在终端里看日志，用脚本启动（菜单栏图标照样有）：

```bash
./dist/rdmachine-server.sh --pairing-code
# 打印本机指纹和一个 8 位配对码；按回车换新的配对码，Ctrl-C 停止
```

命令行参数（`--port`、`--view-only`、`--fps` 等）只对这一次运行生效，优先于保存的设置；`--headless` 不显示菜单栏图标。

第一次运行时 macOS 会请求权限，在“系统设置 → 隐私与安全性”里给 **RDMachine Server** 打开“屏幕录制”（画面）和“辅助功能”（键鼠控制，没有的话只能看不能控），然后 Ctrl-C 停掉再启动一次。脚本通过 `open` 启动应用，所以权限记在 RDMachine Server 名下，而不是“终端”名下。没有打包时也可以直接跑 `./build/bin/rdmachine-server --pairing-code`，这时权限要授给“终端”。

另一台 Mac 上打开 `RDMachine.app`（或未打包的 `build/bin/rdmachine-client`），输入服务端地址，用被控 Mac 当前登录账号的密码，或者用配对码登录。第一次连接会显示服务端指纹，和服务端打印的核对一致后再信任。连上以后键盘全部发给对方（包括 ⌘Q、⌃⌘F）；只有 **⌃⌥⌘F** 留在本机，用来进入和退出全屏。全屏时本机的菜单栏和程序坞完全隐藏，屏幕上下边缘点到的是对方的菜单栏和程序坞。

不开窗口也能测试连接：`./dist/rdmachine-connect 服务端地址 --code=配对码`（或 `--user=账号`，密码从终端输入）。

端口：TCP 4100 是控制连接，UDP 4100 是画面，UDP 不通时画面改走 TCP 4101。第一次运行时 macOS 防火墙（如果开着）会问是否允许 RDMachine Server 接受传入连接，选允许。

## 许可

[Apache License 2.0](LICENSE)
