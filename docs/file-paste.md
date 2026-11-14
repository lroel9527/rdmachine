# 文件复制粘贴（方案）

2026-11-14 和 lroel 一起定下，3.3.0 实现了基础版本。依据是同一天在一台 Mac mini（macOS 27.2，Apple 芯片）上做的实验，测试程序没有放进仓库。

## 目标

在一台 Mac 的访达里复制（⌘C）文件，到另一台的访达里粘贴（⌘V），两个方向都能用，全屏和窗口模式都能用。这样一个功能，必须比 `scp` 用起来更顺手，否则不值得做。

规则和文本、图片一样：**文件只在对方真正粘贴时才传**。rdmachine 在后台时，宿主机上的复制不能引起任何上传。

不做的：拖放（全屏是最常用的方式，那时看不到本机的访达，有复制粘贴就够了）、SFTP 式的传输窗口（本质上还是在两边找路径）。见[决定](decisions.md)的“文件复制粘贴”。

## 苹果的屏幕共享怎么做

在本机系统里查了它的程序引用了哪些函数（`nm -u`、`strings`）：

- 复制粘贴：被控端的辅助进程 `ScreensharingAgent.bundle/Contents/Support/SSPasteboardHelper` 用 Carbon 的 Pasteboard 接口：`PasteboardSetPromiseKeeper`、`PasteboardPutItemFlavor`（`com.apple.pasteboard.promised-file-url`、`promised-file-content-type`）、`PasteboardResolvePromises`，日志字样有 “added URL promise”“%s is wanted for the paste”。
- 拖放：`AppleVNCServer.bundle/Contents/Support/SSDragHelper.app` 用私有的 CoreDrag 接口（`CoreDragCreateWithPasteboardRef`、`CoreDragStartDragging`），在被控端发起真正的拖动。
- 会话期间关掉通用剪贴板（字样 “disable universal clipboard”；苹果文档也说屏幕共享期间通用剪贴板不可用）。

## 实验结论

| 试了什么 | 结果 |
| --- | --- |
| 剪贴板上只放 Carbon 文件承诺（`promised-file-url` + `promised-file-content-type`） | 访达的“粘贴”是灰的，没有人来取 |
| 只放 `NSFilePromiseProvider` | 同样是灰的 |
| 放一个延迟提供的 `public.file-url` | 1 毫秒内就被读走：读的是系统的通用剪贴板（pboard 日志 “Remote pasteboard fetching local data”），我们自己的服务端也会读（`Read()` 里的 `canReadObjectForClasses`） |
| 加 `prepareForNewContents(with: .currentHostOnly)`（只限本机）和 `org.nspasteboard.TransientType` | 没有人提前读了 |
| 切到访达、按 ⌘A、⌘1 | 访达不读 |
| 按 ⌘V，或者打开“编辑”菜单 | 访达立刻来要文件地址，在主线程上等着；模拟 10 秒传输，访达就卡 10 秒，之后粘贴成功 |
| 第一次来要时不给 | 访达不会再来要，⌘V 报“一个或多个项目已删除或不再可用” |
| 文件地址指向 File Provider 域里的占位文件（50 MB，0 个磁盘块，`dataless`） | ⌘V 立刻返回；访达弹出自己的“拷贝”进度窗口，期间一直响应（辅助功能查询 0.15 秒内回答）；扩展的 `fetchContents` 用了 20 秒，文件完整到达 |

两点修正 2.2.0 当时的结论：

- “访达一读文件路径就开始传”，主要读的其实是通用剪贴板和我们自己的服务端。访达只在粘贴或打开“编辑”菜单时才读。
- “访达的粘贴不接受文件承诺”是对的：访达粘贴只认真实的文件地址。所以要“粘贴时才传”，得让地址先指向一个还没有内容的文件，这就是 File Provider。

File Provider 的条件，都已验证：

- 自签证书（`rdmachine Local Signing`）就可以，不需要开发者账号；只用命令行工具打包，不需要 Xcode。扩展只要沙盒权限，App 放在“应用程序”里，用 `lsregister` 和 `pluginkit` 登记。
- 隐藏的域（`hidden`）永远不会被启用，只能用可见的域，访达侧边栏的“位置”里会多一项。
- 域刚建好时是停用的，用户要在“系统设置 → 通用 → 登录项与扩展”里打开一次。

## 方案

1. **复制的一方**：照现在的剪贴板，只宣布，不传内容。复制的是文件时，宣布每个文件的名字、大小、类型和修改时间（文件夹带上里面的结构）。
2. **粘贴的一方**：
   - 在自己的 File Provider 域里为这些文件建占位项（只有元数据）；
   - 往剪贴板上放一个延迟提供的 `public.file-url`，指向这些占位项；用 `currentHostOnly`，加上我们自己读剪贴板时会跳过的标记。
3. **⌘V**：访达拿到地址，立刻开始拷贝，进度由访达自己显示，可以在它的窗口里取消。系统回调扩展的 `fetchContents`，扩展通过现有的通道向对方要内容（走 rdaccel 的应用消息，沿用剪贴板的分块、确认和 `transfer_pacer.h` 的节奏，画面优先），写进临时文件交给系统。
4. **不传的情况**：对方又复制了别的、会话结束、用户在访达里取消，都取消传输，占位项作废。

两边都要有扩展：服务端和客户端各带一个（远程 → 本机时，粘贴的一方是客户端）。扩展是沙盒进程，和主程序之间用 XPC 通信（`NSFileProviderServicing`），主程序负责网络。

## 实现（3.3.0）

- `core/`：`file_transfer.h`（清单、流、收发；平台无关，POSIX 文件），接在 `ClipboardSync` 里，消息见[协议](protocol.md)的“文件（剪贴板版本 4）”。
- `fileprovider/files_extension.m`：扩展，只放当前这一份复制，在内存里；文件夹的内容要用时才问（list），文件要用时才取（fetch）。和主程序之间的 XPC 协议在 `fileprovider/rdm_files_xpc.h`：主程序连扩展在域根目录上提供的服务，扩展在同一条连接上回调主程序；取到的文件以文件句柄交过去，扩展克隆进系统的临时目录。
- `session/src/file_target_mac.mm`：主程序这一侧，进程里的会话共用一个域和一条连接，最后发布的会话拥有当前这份。
- `session/src/system_clipboard_mac.mm`：复制时读出文件路径；粘贴的一方每个文件一项、延迟提供的文件地址，整个承诺都用 `currentHostOnly`。
- 占位文件要等它的文件夹被列过才会出现在磁盘上（在那之前系统让它等父目录，fileproviderctl 里是 `⧗parentCreation`）。3.3.2 起发布后主动列一次这份复制的文件夹，剪贴板给出文件地址前也列一次父目录；3.3.0 和 3.3.1 在两台 Mac 之间粘贴不出来就是因为这个（本机测试碰巧先 `ls` 过）。
- 打包：`package.sh` 把 `RDMachine Files.appex`（标识是应用的标识加 `.files`）放进两个应用，只给沙盒权限，单独签名。
- 验证：core 的单元测试（流、路径、遍历、整棵文件夹、取消、老版本）；在这台 Mac 上用一个测试应用（两端在同一个进程里，网络以外都是真的）在访达里粘贴一个 30 MB 文件夹（子文件夹、中文名、符号链接）和一个 5 MB 文件：⌘V 后访达立刻开始，几秒内全部到达，校验和一致，没有锁定标记。真实连接上的端到端测试（`FilesE2E`）要录屏权限，还没跑过；两台 Mac 之间还没试过。

## 还要解决的

- 权限只保留了自己的读写执行：File Provider 的条目只有 `fileSystemFlags`，组和其他人的权限位丢了（原来 0644 的文件粘出来是 0600）。
- 扩展属性（访达标签等）还没带。
- 正在拷贝时在任一边又复制了别的，传输会被取消，访达报错。
- 断线重连后不会接着传。
- 访达的拷贝窗口：下载期间粘贴目标一直是 0 字节，访达等整个文件下载完才开始拷到目标，它的进度条量的是拷到目标的字节，所以可能不跟着下载走；系统那边有真实的下载进度（`fileproviderctl dump` 里的 download progress）。3.3.3 起进度每 256 KB 报一次，大小一开始就给系统，还要在两台 Mac 之间看访达的窗口是否跟着动。
- 条目标识（3.3.4 修）：每次运行都从 `c1` 开始编号，系统按标识记条目，上一次运行留下的 `c1/0`（取失败、正在退避重试）顶替了这一次的，粘贴时根本没去取。现在复制的标识带上时间（`c<秒数>-<序号>`）。
- 远的链路（3.3.3 修）：节奏控制原来按局域网调，确认包比最快的慢 25 毫秒就当成排队并降速、每 100 毫秒判断一次。往返 200 毫秒、抖动几十毫秒的 VPN 上，抖动本身一直触发降速，客户端发往服务端只有每秒 50 KB 左右。现在一个判断周期至少两个往返，排队的门槛放宽到往返延迟的一半（至少 25 毫秒）；剪贴板的大图也受益。

- 锁定标记：只能读的条目会被打上 `uchg`，访达连标记一起拷出来。3.3.0 的条目声明了可写（能力里加上写、改名、删除，真改时拒绝），粘出来的是普通文件。
- 侧边栏那一项：命名（比如 “RDMachine”），要不要把它当作“对方复制的文件”的入口。
- 第一次使用：没打开时只粘贴出文件名。3.3.1 起安装包以登录用户的身份运行 `--register-files` 注册域（域不存在时设置里的开关拨不动），装完开关就能拨。自动打开开关做不到：`NSFileProviderDomainTestingModeAlwaysEnabled` 需要 `com.apple.developer.fileprovider.testing-mode` 权限，带它的自签程序一启动就被系统杀掉（试过），要开发者账号和描述文件，而且苹果说那是测试用的。客户端还没有提示用户去打开。
- 用鼠标打开“编辑”菜单也会触发拷贝，比粘贴稍早，可以接受。
- 文件夹、很多小文件、同名文件，以及断线重连后接着传。
- 和 `scp` 在同一条 Tailscale 链路上比一下速度；跟不上就给文件单开一条 TCP 连接。
- 登录窗口时不提供（那时没有用户会话）。
