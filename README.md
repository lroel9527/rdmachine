# rdmachine

开源的远程桌面应用，体验对标 NoMachine。画面采集、编码和弱网传输由 [rdaccel](https://github.com/lroel9527/rdaccel) 提供，rdmachine 负责其上的产品层：服务端、登录认证、会话管理和客户端。

第一阶段只支持 Mac ↔ Mac（Intel 和 Apple Silicon，macOS 12.3+）。1.0.0 是第一个正式版本：服务端（菜单栏应用，也能在命令行跑）和 Mac 客户端已经在两台 Mac 之间实际使用，远程桌面按客户端窗口的像素和 HiDPI 缩放建出来。两边可以互相复制粘贴文本、富文本和图片。3.0.0 起服务端可以装成服务：守护进程开机运行，连到登录窗口和锁屏，有人登录、退出时会话跟着换到新的桌面（见下文“服务、登录窗口和锁屏”）。还没有的：公钥登录、声音、文件传输（以后做单独的传输窗口）（见[第一阶段的决定](docs/decisions.md)）；控制协议仍是 v0，两端要用同一个版本。

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
   packaging/macos/package.sh --build --pkg --zip --dmg
   ```

   产物在 `dist/`：

   | 文件 | 用途 |
   | --- | --- |
   | `RDMachine.app` | 客户端 |
   | `RDMachine Server.app` | 服务端（菜单栏应用，没有 Dock 图标） |
   | `rdmachine-server.sh` | 在终端里启动服务端，输出和配对码显示在终端 |
   | `rdmachine-connect` | 命令行测试客户端 |
   | `rdmachine-版本号-macos.zip` | 以上全部，拷到另一台 Mac 用 |
   | `RDMachine-版本号.pkg` | **安装程序**（推荐）：可选服务端、客户端；服务端装完就作为服务运行（守护进程 + agent） |
| `RDMachine-版本号.dmg` | 安装盘：把应用拖进“应用程序”；服务端这样装只能独立运行，要装成服务再跑 `rdmachine-service install` |

   已经编译过的话去掉 `--build`，默认从 `build/bin` 取二进制（`--build-dir` 可改）。`--identity -` 是临时签名，只适合一次性试试。

3. **装到 Mac 上**：用 U 盘、`scp` 或 AirDrop 把 pkg 拷过去，双击安装：被控的 Mac 勾“RDMachine Server”，用来连接的 Mac 勾“RDMachine”（两个都勾也行），输一次管理员密码。pkg 没有签名（签 pkg 要 Apple Developer 的安装器证书），第一次打开会被拦下：macOS 12–14 在 pkg 上右键“打开”；macOS 15 起去“系统设置 → 隐私与安全性”底部点“仍要打开”。里面的应用照旧用固定的自签证书签名，升级后屏幕录制、辅助功能权限不用重新授予。也可以在终端里装：`sudo installer -pkg RDMachine-版本号.pkg -target /`。不想装成服务时用 dmg 或 zip，自签应用第一次打开同样会被拦下，处理方法相同，或者 `xattr -dr com.apple.quarantine RDMachine*.app` 去掉下载标记。

## 试用（M0）

被控的 Mac 上双击打开 `RDMachine Server.app`，菜单栏会出现一个显示器图标，点开可以：

- 看当前状态：监听的端口、有没有人连着（谁、从哪个地址、走 UDP 还是 TCP、连了多久），以及“断开当前连接”；
- 生成配对码（自动复制，5 分钟内有效，用一次就失效）、复制本机指纹；
- 看屏幕录制和辅助功能权限是否已经允许，没允许的话点一下打开对应的系统设置；
- “设置…”：端口、是否允许配对码、只看不控、剪贴板方向、帧率和码率上限。保存在 `~/Library/Application Support/rdmachine/server-settings.json`；改了端口等设置后服务会重新监听，正在进行的连接会断开。

想在终端里看日志，用脚本启动（菜单栏图标照样有）：

```bash
./dist/rdmachine-server.sh --pairing-code
# 打印本机指纹和一个 8 位配对码；按回车换新的配对码，Ctrl-C 停止
```

命令行参数（`--port`、`--view-only`、`--fps` 等）只对这一次运行生效，优先于保存的设置；`--headless` 不显示菜单栏图标。

第一次运行时 macOS 会请求权限，在“系统设置 → 隐私与安全性”里给 **RDMachine Server** 打开“屏幕录制”（画面）和“辅助功能”（键鼠控制，没有的话只能看不能控），然后 Ctrl-C 停掉再启动一次。脚本通过 `open` 启动应用，所以权限记在 RDMachine Server 名下，而不是“终端”名下。没有打包时也可以直接跑 `./build/bin/rdmachine-server --pairing-code`，这时权限要授给“终端”。

另一台 Mac 上打开 `RDMachine.app`（或未打包的 `build/bin/rdmachine-client`），输入服务端地址，用被控 Mac 当前登录账号的密码，或者用配对码登录。第一次连接会显示服务端指纹，和服务端打印的核对一致后再信任。连上以后键盘全部发给对方（包括 ⌘Q、⌃⌘F）；只有 **⌃⌥⌘F** 留在本机，用来进入和退出全屏。中文输入：在客户端用 Caps Lock 或 ⌃空格切换中英文，服务端的输入法会跟着切换（View → Sync Keyboard Language，默认打开）；服务端要装有对应的输入法，候选词出现在服务端的光标旁。调试数据：远程窗口右上角有一个半透明的小浮窗（View → Statistics），显示连接时长、UDP/TCP、帧率、延迟（RTT）、丢包，以及“操作到画面”的延迟（在画面静止时按键、点击或滚动，到下一帧画面到达的时间，最接近手感），还可以勾选码率、带宽、分辨率、编码器、输入法，背景透明度可选；鼠标点击会穿过它，落到远程画面上。**桌面跟着窗口走（HiDPI）**：客户端连接时把窗口的像素尺寸和缩放系数（Retina 是 2）发给服务端，服务端按这个规格建一块虚拟显示器作为会话的桌面，画面一个像素对一个像素，不经过缩放；拖动窗口大小、进出全屏、把窗口挪到另一块屏幕之后，桌面跟着重建（见[控制协议](docs/protocol.md)的“请求桌面”）。会话期间被控 Mac 的真实显示器镜像这块虚拟屏，断开后恢复原样。全屏时本机的菜单栏和程序坞完全隐藏，屏幕上下边缘点到的是对方的菜单栏和程序坞。**省流量**：View → Data Saver (Sharp Text)。体验和平时一样，流量更少：静止的和正在打字的文字用更紧凑的方式发（颜色略有减少），流量是平时的 1/3 到 1/7；滚动、拖动窗口省三到六成；影片基本不省。选择会记住，可以在会话中随时切换（见[控制协议](docs/protocol.md)的“文字模式”）。开着时默认改用 1 倍的桌面（Retina 屏上字会粗糙一些，但像素只有四分之一，流量更省）；想保留 Retina 桌面就取消勾选 View → Data Saver at Standard Resolution (1x)。不在乎流量时不用开。

**复制粘贴**：在任一台 Mac 上复制（⌘C）的文本、富文本、图片，可以在另一台上粘贴（⌘V）。内容只有在对方真正粘贴时才传过去：在本机复制的密码等内容，不在远程那边粘贴就不会离开本机；连接之前就在剪贴板里的也不会。在访达里复制的文件只传文件名，文件本身以后用单独的传输窗口传。旧版客户端连新服务端时只同步服务端复制的文本。

**自动登录和重连**（3.1.0 起；rdmachine 按自用设计：登录过就可信）：第一次用密码登录成功后，客户端把账号密码存进本机钥匙串；之后打开客户端会直接连上次那台电脑，锁屏或停在登录窗口时服务端替你填好密码。断线后在同一个窗口自动重连（最多 1 分钟）。客户端（界面是中文）的主窗口是连过的电脑列表（最近的在上面），每台显示状态：已连接（连了多久、UDP/TCP、延迟、分辨率）、正在重连、在线或离线（客户端每 15 秒试一下那台电脑的控制端口）、是否已保存密码：双击就用保存的密码直接连，右键可以编辑地址或账号、忘记密码、从列表删除；“新建连接…”才需要填地址和密码。关掉远程窗口、按住 ⌥ 打开客户端、或者菜单里选“连接到其他电脑…”都回到这个列表。可以同时连多台电脑，每台一个窗口：已经连着时再点程序坞里的图标（或者再打开一次客户端）会显示列表，选另一台就开一个新窗口；选已经连着的那台只是把它的窗口调到前面。被控 Mac 改了密码时，客户端会请你输一次新的。第二台客户端连上来时接管会话，前一台断开。远程窗口下次按上次的样子打开（全屏或原来的大小）。服务端重装、指纹变了时，客户端会问你是否信任新指纹。

Mac 客户端也能从命令行带地址启动：`RDMachine.app/Contents/MacOS/rdmachine-client 服务端地址[:端口]` 只填好地址，加 `--code=配对码` 直接连接（密码不从命令行传）。

不开窗口也能测试连接：`./dist/rdmachine-connect 服务端地址 --code=配对码`（或 `--user=账号`，密码从终端输入）。

端口：TCP 4100 是控制连接，UDP 4100 是画面，UDP 不通时画面改走 TCP 4101。第一次运行时 macOS 防火墙（如果开着）会问是否允许 RDMachine Server 接受传入连接，选允许。

## 服务、登录窗口和锁屏

用 pkg 安装服务端时，它就作为服务运行（参照 NoMachine）：开机就能连，退出登录、锁屏时也能连，崩溃后自动重启。用 dmg 装的或者手动打开的服务端，可以之后再装成服务：

```bash
sudo "/Applications/RDMachine Server.app/Contents/Resources/rdmachine-service" install
```

服务由三样东西组成，都由 launchd 启动、出错后重启：

- **守护进程**（root，开机运行）：监听端口、负责登录，把会话交给当前占着屏幕的那一方；你的服务端身份会复制给它，客户端看到的指纹不变。
- **登录窗口 agent**：没人登录时显示登录窗口。
- **你会话里的菜单栏应用**：显示你的桌面，现在作为守护进程的 agent 运行。

`rdmachine-service` 在服务端应用里（`/Applications/RDMachine Server.app/Contents/Resources/`，DMG 的 Command Line Tools 里也有一份）：

| 命令 | 作用 |
| --- | --- |
| `rdmachine-service status` | 装了没有、谁在运行、监听什么端口 |
| `sudo rdmachine-service start` / `stop` / `restart` | 启动、停止、重启 |
| `sudo rdmachine-service startmode manual` / `automatic` | 不自动启动 / 自动启动 |
| `sudo rdmachine-service uninstall` | 卸载（之后菜单栏应用照旧独立运行） |

- **锁屏**：用账号密码连接时，服务端在画面连上后替你把密码填进锁屏；用配对码连接时在远程画面里自己输。
- **登录窗口**：用这台 Mac 上任一账号的密码连接，服务端替你填进登录窗口并登录，画面随后自动换成登录后的桌面（同一条连接，不用重连）。在远程退出登录，画面回到登录窗口。登录窗口不提供配对码。
- 有人登录时只能用那个账号连接；连接期间如果被控 Mac 上换了另一个人登录，会话结束。
- 改了端口或重新生成身份后，再运行一次 `install`。macOS 可能在“系统设置 → 通用 → 登录项与扩展 → 允许在后台”里列出 RDMachine Server，保持打开。
- 开着 FileVault 时，重启后停在开机解锁界面，这时还连不上。

## 许可

[Apache License 2.0](LICENSE)
