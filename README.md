# rdmachine

开源的远程桌面应用，体验对标 NoMachine。画面采集、编码和弱网传输由 [rdaccel](https://github.com/lroel9527/rdaccel) 提供，rdmachine 负责其上的产品层：服务端、登录认证、会话管理和客户端。

第一阶段只支持 **Mac ↔ Mac**（Intel 和 Apple Silicon，macOS 12.3+），当前版本 3.1.0。rdmachine 按自用设计：登录过就可信，尽量少打扰。

## 功能

- **桌面跟着窗口走（HiDPI）**：服务端按客户端窗口的像素和缩放系数建一块虚拟显示器，一个像素对一个像素；调整窗口、进出全屏、换屏幕后桌面跟着重建。
- **服务、登录窗口和锁屏**：服务端装成服务后开机就能连，连得上登录窗口和锁屏，并替你填好密码；有人登录、退出时会话在同一条连接上换到新桌面。
- **自动登录和重连**：密码存进本机钥匙串，打开客户端直接连上次那台电脑，断线后在同一个窗口自动重连。
- **多台电脑**：客户端主窗口列出连过的电脑和它们的状态，可以同时连多台，每台一个窗口。
- **复制粘贴**：两边互相复制粘贴文本、富文本和图片，对方真正粘贴时才传。
- **中文输入**：客户端切换输入法，服务端跟着切换。
- **省流量**：文字画面的流量降到平时的 1/3 到 1/7，体验不变。
- **弱网**：UDP 传画面，不通时自动改走 TCP；心跳 2 秒，服务端没了 12 秒内就知道。

还没有的：公钥登录、声音、文件传输（以后做单独的传输窗口），见[第一阶段的决定](docs/decisions.md)。控制协议仍是 v0，两端要用同一个版本。

## 安装

从 [Releases](https://github.com/lroel9527/rdmachine/releases) 下载 `RDMachine-版本号.pkg`，双击安装：

- 被控的 Mac 勾 **RDMachine Server**：装完就作为服务运行（守护进程 + agent）；
- 用来连接的 Mac 勾 **RDMachine**（客户端）；两个都勾也行。

pkg 没有签名（签 pkg 要 Apple Developer 的安装器证书），第一次打开会被拦下：macOS 12–14 在 pkg 上右键“打开”；macOS 15 起去“系统设置 → 隐私与安全性”底部点“仍要打开”。也可以在终端里装：

```bash
sudo installer -pkg RDMachine-版本号.pkg -target /
```

里面的应用用固定的自签证书签名，升级后不用重新授权。

**被控的 Mac 第一次运行时**，在“系统设置 → 隐私与安全性”里给 **RDMachine Server** 打开“屏幕录制”（画面）和“辅助功能”（键鼠控制，没有的话只能看不能控）；macOS 防火墙如果开着，允许它接受传入连接。

## 使用

### 服务端

菜单栏里的显示器图标（没有 Dock 图标），点开可以：

- 看状态：监听的端口、谁从哪个地址连着、走 UDP 还是 TCP、连了多久，以及“断开当前连接”；
- 生成配对码（自动复制，5 分钟内有效，用一次就失效）、复制本机指纹；
- 看屏幕录制和辅助功能权限，没允许的话点一下打开对应的系统设置；
- “设置…”：端口、是否允许配对码、只看不控、剪贴板方向、帧率和码率上限。保存在 `~/Library/Application Support/rdmachine/server-settings.json`；改了端口等设置后服务重新监听，正在进行的连接会断开。

### 客户端

客户端界面是中文。主窗口是连过的电脑列表（最近的在上面），每台显示状态：已连接（时长、UDP/TCP、延迟、分辨率）、正在重连、在线或离线（每 15 秒试一下那台电脑的控制端口）、是否已保存密码。

- **连接**：“新建连接…”填地址，用被控 Mac 上账号的密码或配对码登录。第一次连接会显示服务端指纹，和服务端菜单里的核对一致后再信任；服务端重装、指纹变了时会再问一次。
- **之后**：双击列表里的电脑就用保存的密码直接连；打开客户端会自动连上次那台。右键可以编辑地址或账号、忘记密码、从列表删除。被控 Mac 改了密码时，客户端会请你输一次新的。
- **多台**：已经连着时再点程序坞图标（或再打开一次客户端）会显示列表，选另一台开一个新窗口，选已连着的那台把它的窗口调到前面。关掉远程窗口、按住 ⌥ 打开客户端、或者菜单里选“连接到其他电脑…”都回到列表。
- **断线**：画面变暗，上面的卡片说明发生了什么，倒计时自动重连（最多 1 分钟），也可以立即重试或断开。连接结束或失败后，列表顶部有一条提示，可以重新连接。第二台客户端连上来时接管会话，前一台断开。
- **窗口**：远程窗口下次按上次的样子打开（全屏或原来的大小）。会话期间被控 Mac 的真实显示器镜像这块虚拟屏，断开后恢复原样。

### 键盘和输入法

连上以后键盘全部发给对方（包括 ⌘Q、⌃⌘F）；只有 **⌃⌥⌘F** 留在本机，用来进入和退出全屏。全屏时本机的菜单栏和程序坞完全隐藏，屏幕上下边缘点到的是对方的菜单栏和程序坞。

中文输入：在客户端用 Caps Lock 或 ⌃空格切换中英文，服务端的输入法跟着切换（View → Sync Keyboard Language，默认打开）。服务端要装有对应的输入法，候选词出现在服务端的光标旁。

### 复制粘贴

在任一台 Mac 上复制（⌘C）的文本、富文本、图片，可以在另一台上粘贴（⌘V）。内容只有在对方真正粘贴时才传过去：本机复制的密码不在远程那边粘贴就不会离开本机，连接之前就在剪贴板里的也不会。在访达里复制的文件只传文件名。

### 省流量

View → Data Saver (Sharp Text)。静止的和正在打字的文字用更紧凑的方式发（颜色略有减少），流量是平时的 1/3 到 1/7；滚动、拖动窗口省三到六成；影片基本不省。选择会记住，会话中随时可以切换（见[控制协议](docs/protocol.md)的“文字模式”）。

开着时默认改用 1 倍的桌面（Retina 屏上字粗糙一些，但像素只有四分之一）；想保留 Retina 桌面就取消勾选 View → Data Saver at Standard Resolution (1x)。

### 统计浮窗

远程窗口右上角的半透明小浮窗（View → Statistics）显示连接时长、UDP/TCP、帧率、延迟（RTT）、丢包，以及“操作到画面”的延迟（画面静止时按键、点击或滚动，到下一帧到达的时间，最接近手感）；还可以勾选码率、带宽、分辨率、编码器、输入法，背景透明度可选。鼠标点击会穿过它，落到远程画面上。

## 服务、登录窗口和锁屏

用 pkg 安装服务端时，它就作为服务运行（参照 NoMachine）：开机就能连，退出登录、锁屏时也能连，崩溃后自动重启。服务由三部分组成，都由 launchd 启动：

- **守护进程**（root，开机运行）：监听端口、负责登录，把会话交给当前占着屏幕的一方；服务端身份复制给它，客户端看到的指纹不变。
- **登录窗口 agent**：没人登录时显示登录窗口。
- **你会话里的菜单栏应用**：显示你的桌面，作为守护进程的 agent 运行。

行为：

- **锁屏**：用账号密码连接时，服务端在画面连上后替你把密码填进锁屏；用配对码连接时在远程画面里自己输。
- **登录窗口**：用这台 Mac 上任一账号的密码连接，服务端替你填进登录窗口并登录，画面随后换成登录后的桌面（同一条连接）。在远程退出登录，画面回到登录窗口。登录窗口不提供配对码。
- 有人登录时只能用那个账号连接；连接期间被控 Mac 上换了另一个人登录，会话结束。
- 开着 FileVault 时，重启后停在开机解锁界面，这时还连不上。

`rdmachine-service` 在 `/Applications/RDMachine Server.app/Contents/Resources/` 里：

| 命令 | 作用 |
| --- | --- |
| `rdmachine-service status` | 装了没有、谁在运行、监听什么端口 |
| `sudo rdmachine-service install` | 装成服务（手动打开或从 dmg 装的服务端） |
| `sudo rdmachine-service start` / `stop` / `restart` | 启动、停止、重启 |
| `sudo rdmachine-service startmode manual` / `automatic` | 不自动启动 / 自动启动 |
| `sudo rdmachine-service uninstall` | 卸载（之后菜单栏应用照旧独立运行） |

改了端口或重新生成身份后，再运行一次 `install`。macOS 可能在“系统设置 → 通用 → 登录项与扩展 → 允许在后台”里列出 RDMachine Server，保持打开。

## 端口

| 端口 | 用途 |
| --- | --- |
| TCP 4100 | 控制连接 |
| UDP 4100 | 画面 |
| TCP 4101 | UDP 不通时的画面 |

## 命令行

- 在终端里跑服务端、看日志（菜单栏图标照样有）：`./dist/rdmachine-server.sh --pairing-code`，打印本机指纹和一个 8 位配对码，按回车换新的，Ctrl-C 停止。`--port`、`--view-only`、`--fps` 等参数只对这一次运行生效，优先于保存的设置；`--headless` 不显示菜单栏图标。脚本通过 `open` 启动应用，权限记在 RDMachine Server 名下；直接跑 `./build/bin/rdmachine-server` 时权限要授给“终端”。
- 带地址启动客户端：`RDMachine.app/Contents/MacOS/rdmachine-client 服务端地址[:端口]` 只填好地址，加 `--code=配对码` 直接连接（密码不从命令行传）。
- 不开窗口测试连接：`./dist/rdmachine-connect 服务端地址 --code=配对码`（或 `--user=账号`，密码从终端输入）。

## 从源码构建

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

没有 Apple Developer 账号时，用本机自签的证书签名。macOS 按“证书 + 应用标识”记住屏幕录制和辅助功能权限，证书固定下来以后，重新编译打包的应用不用再授权；不签或临时签名（ad hoc）的话，每次重编都得重新授权。

1. **每台用来编译的 Mac 做一次**：创建签名证书。中途会弹窗要登录密码（把证书设为“代码签名”可信），之后第一次签名时如果问是否允许 codesign 使用钥匙串，点“始终允许”。

   ```bash
   packaging/macos/create-signing-identity.sh
   # 在登录钥匙串里创建 “rdmachine Local Signing”，再运行一次不会重复创建
   ```

   这张证书要留着：删掉重建的话，权限都要重新授予。换到另一台 Mac 编译，在“钥匙串访问”里连私钥一起导出、到那台导入即可。

2. **编译并打包**（通用二进制）：

   ```bash
   packaging/macos/package.sh --build --pkg --zip --dmg
   ```

   已经编译过的话去掉 `--build`，默认从 `build/bin` 取二进制（`--build-dir` 可改）。`--identity -` 是临时签名，只适合一次性试试。产物在 `dist/`：

   | 文件 | 用途 |
   | --- | --- |
   | `RDMachine-版本号.pkg` | **安装程序**（推荐，发布只附这个）：可选服务端、客户端；服务端装完就作为服务运行 |
   | `RDMachine-版本号.dmg` | 安装盘：把应用拖进“应用程序”；服务端这样装只能独立运行，要装成服务再跑 `rdmachine-service install` |
   | `rdmachine-版本号-macos.zip` | 下面这些文件的压缩包 |
   | `RDMachine.app` | 客户端 |
   | `RDMachine Server.app` | 服务端 |
   | `rdmachine-server.sh` | 在终端里启动服务端 |
   | `rdmachine-connect` | 命令行测试客户端 |

   用 dmg 或 zip 装时，自签应用第一次打开同样会被拦下，处理方法和 pkg 相同，或者 `xattr -dr com.apple.quarantine RDMachine*.app` 去掉下载标记。

## 文档

| 文档 | 内容 |
| --- | --- |
| [第一阶段的决定](docs/decisions.md) | 范围、语言、界面、认证、端口、会话模型 |
| [控制协议 v0](docs/protocol.md) | 登录、密钥下发、会话控制的线上格式 |
| [NoMachine 功能清单](docs/research/nomachine-features.md) | 调研：NoMachine 有哪些功能，P0/P1/P2 |
| [rdaccel 能力评估](docs/research/rdaccel-assessment.md) | 调研：rdaccel 能做到什么、缺什么 |
| [同类开源方案](docs/research/competitive-analysis.md) | 调研：RustDesk、VNC、xrdp、Guacamole、X2Go、Sunshine |

## 许可

[Apache License 2.0](LICENSE)
