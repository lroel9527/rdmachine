# 客户端输入法

2026-11-19 和 lroel 一起定下，6.2.0 实现（rdaccel 3.2.0）。6.2.1 装上后 lroel 实际用过，评价“正常，跟手”（2026-11-20）；第 8 节的数值还没有量。依据是同一天在 Mac mini（macOS 27.2，Apple 芯片）上做的探测程序，探测程序没有放进仓库。

## 目标

现在打中文是这样的：客户端把输入源同步给服务端（[协议](protocol.md)的“键盘语言同步”），按键原样发过去，组字、候选窗都在服务端由服务端的输入法画好，再编码传回来。每打一个字母、每翻一页候选、每选一个字，都要等一个来回加截屏、编码、解码。

目标：**用客户端本机的输入法打字，候选窗在客户端本机画，贴在远程应用的光标旁边**，细腻度要达到苹果的 RemoteTextInput（iPhone 镜像、iPhone 给 Apple TV 打字用的那套）：光标位置、组字范围、选区、所属应用，每个按键都有。

顺带的好处：

- 用的是客户端自己的输入法和它的词库、习惯，服务端不用装同样的输入法。
- 候选窗不再出现在服务端的屏幕上，不进视频，省一点流量（省流模式正需要）。
- 表情与符号（⌃⌘空格）等客户端的文字输入也能直接用。

不做的：语音输入、重新转换（选中已上屏的字再改）、把组字文字画在客户端本地浮层上（见第 9 节，先测再说）。

## 1. 为什么不用别的办法

- **屏幕共享（Mac 到 Mac）不这样做。** 它的客户端只读输入源和用 `UCKeyTranslate` 翻译按键，被控端（`ScreensharingAgent`、`AppleVNCServer`）用 `TISSelectInputSource` 切输入法、`CGEventKeyboardSetUnicodeString` 注入字符。工具栏上的 “Sync Keyboard Language” 就是我们现在的键盘语言同步。组字在被控端。
- **RemoteTextInput 用不上。** 这台 Mac 上只有“iPhone 镜像”（经 ScreenSharingKit）用它，类是 `RTIInputSystemService`、`RTIDocumentState`（有 `caretRectInWindow`、`selectedTextRange`、`markedTextRange`）、`RTITextOperations`。被打字的一端要让每个输入框都能报告自己的状态：iOS 的 UIKit 天生支持，Mac 的 AppKit 没有链接这个框架，所以 Mac 当不了被控端。它还是私有接口，服务端要查 entitlement。
- **辅助功能接口（AX）不够。** 读 `kAXSelectedTextRange` 和 `kAXBoundsForRangeParameterizedAttribute` 能拿到一部分应用的光标，但 Electron 应用要先打开 `AXManualAccessibility`，有的应用干脆不给，后台应用拿不到焦点元素。
- **VMware Horizon、Citrix** 有类似的功能（“Extend the local IME”、“Client IME”），说明这条路是通的；开源项目（FreeRDP、Guacamole、RustDesk、noVNC）里没有完整实现可以拿来用。

**我们的办法：在服务端装一个自己的输入法（InputMethodKit）。** macOS 上凡是能打中文的应用，都会把当前输入框交给输入法（`IMKTextInput`）。系统拼音能把候选窗贴在光标旁边，靠的就是这个。我们的输入法自己不组字，只当“服务端那一半”：报告输入框的状态，把客户端组好的字放进去。它拿到的信息和 RemoteTextInput 几乎一一对应：

| RemoteTextInput | InputMethodKit（`IMKTextInput`） |
| --- | --- |
| `caretRectInWindow` | `attributesForCharacterIndex:lineHeightRectangle:`，屏幕坐标，带行高 |
| `selectedTextRange`、`markedTextRange` | `selectedRange`、`markedRange` |
| `documentState`（光标前后的文字） | `attributedSubstringFromRange:` |
| `RTITextOperations` | `setMarkedText:selectionRange:replacementRange:`、`insertText:replacementRange:` |
| 应用 | `bundleIdentifier` |
| 密码框之类的特征 | 安全输入时系统不调用输入法，这本身就说明是密码框 |

## 2. 实测（2026-11-19）

探测程序是一个装在 `~/Library/Input Methods` 的输入法，自签名（rdmachine Local Signing）。lroel 通过 RDMachine 在各应用里打字。

**安装和启用**

- `TISRegisterInputSource` 成功，系统能看到它。
- `TISEnableInputSource` 返回成功，但输入源仍然是关的。**要用户在系统设置 → 键盘 → 输入法里手动添加一次。** 加上以后 `TISSelectInputSource` 可以由程序选中和切回。

**第一版：只读，不组字**（每个按键时问一次）

- 选区在所有应用里都对，包括退格。
- 光标矩形：VS Code、Safari 网页输入框、邮件每个字符都跟着动（约 7 px 一个字符）；Safari 地址栏、访达和系统设置的搜索框只有第一个字符对，之后为空；Claude 前 3 个字符对，之后停住。
- 原因：没有组字文字时，光标在文字末尾，“光标处的字符”不存在，系统就不给；Chromium 只在组字时有每个字的位置（见下文的源码）。
- `firstRectForCharacterRange:actualRange:` 经过输入法连接返回的是垃圾值，不能用；`attributesForCharacterIndex:lineHeightRectangle:` 可以用。
- `length` 在 Electron 和网页里是 0，不能用。

**第二版：像真输入法一样组字**（字母先放成带下划线的组字文字，空格或回车上屏，组字后立即问一次、60 ms 后再问一次）

| 应用 / 位置 | 组字时的位置 |
| --- | --- |
| Safari 地址栏 | 准（第一版第一个字母之后为空） |
| 访达搜索框 | 准（第一版第一个字母之后为空） |
| Safari 网页输入框、邮件 | 准 |
| 文本编辑，第一个词 | 准 |
| 文本编辑，第二个词 | 为空（4 个字母 16 ms 内一起到，原因未明） |
| Claude | 3 个字母都准（第 4 个以后未测） |

组字和上屏在这 5 个应用里都正常，没有异常。

**耗时**：原生文本框每次查询 0.3–2 ms；Safari、邮件在刚放完组字文字后立即查询要 6–48 ms（要等网页进程处理完），60 ms 后再查约 1 ms。所以**服务端不能在注入按键的线程上同步等这些查询**（第 5 节）。

**没测的几处，按源码定**（lroel 决定不再补测，2026-11-20）：

- **Chromium 类应用（Claude、VS Code、Chrome）**：`content/browser/renderer_host/render_widget_host_view_mac.mm`。
  - 组字时：`GetCachedFirstRectForCharacterRange` 把请求换算到组字范围里，用渲染进程送来的 `character_bounds`。代码检查它的个数等于组字长度，所以**组字的每个字都有位置，长度不限**。请求组字起点给第一个字的左边缘，请求组字末尾给最后一个字的右边缘（`GetFirstRectForCompositionRange`）。所以 Claude 长词不会再停住。
  - 没有组字时：只有请求正好从光标开始才用缓存的 `caret_rect`；别的位置用选区的矩形，或者落到 `SyncGetFirstRectForRange`，同步等渲染进程回答。这就是第一版停住、以及刚组字后立即查询要几十毫秒的原因。
  - 缓存是渲染进程异步送来的，刚 `setMarkedText` 后可能还是旧的，所以第 5 节要在 16 ms、60 ms 后再查。
- **iTerm2**：`sources/TerminalView/PTYTextView.m` 的 `firstRectForCharacterRange:actualRange:` 按字符网格算：列 × 字宽、行 × 行高，换成屏幕坐标，任何时候都准。系统终端不开源，没法从源码确认；它支持带下划线的组字，系统拼音的候选窗在里面位置正确，所以按同样的路子处理。
- **文本编辑第二个词为空**：文本编辑就是 `NSTextView`，它是 `NSTextInputClient` 的标准实现，系统拼音在里面位置正确，所以更可能是我的探测程序问法的问题（只问了 `attributesForCharacterIndex:`、只问了组字起点）。实现时按第 7.1 节的顺序多问几处，都为空才算拿不到。
- **信息**：不开源，是 AppKit 文本控件。拿不到时走第 6.3 节的退路。

## 3. 总览

```
客户端                                   服务端（用户会话里的 agent）          RDMachine 输入法（服务端）
本机输入法 ─ NSTextInputClient ─┐
  setMarkedText  ──────────────┼─ ime 消息（有序）──► MediaHost ──Unix 套接字──► setMarkedText 进远程应用
  insertText     ──────────────┤                                               insertText 进远程应用
  其他键          ── 原始按键 ──┘                       注入（CGEvent）────────► 输入法放过，应用照常处理
候选窗贴在远程光标旁 ◄──────── ime 状态消息 ◄──────── 坐标换算 ◄──────────── 光标矩形、组字范围、应用
```

- **客户端**：远程画面窗口实现 `NSTextInputClient`。客户端当前是输入法（拼音、日文、韩文等）时，按键先交给本机输入法；输入法要的组字和上屏发给服务端，输入法不要的键照原样发。候选窗的位置问 `firstRectForCharacterRange:`，客户端用服务端报来的远程光标位置回答。
- **服务端 agent**：会话期间选中“RDMachine 输入法”，把客户端的组字、上屏转给它，把它报来的光标矩形换算成画面坐标发回客户端。
- **RDMachine 输入法**：不组字。收到组字就 `setMarkedText`（远程应用里出现带下划线的拼音），收到上屏就 `insertText`；客户端没有在组字时，按键一律放过（`handleEvent` 返回 `NO`），所以 ⌘ 快捷键、方向键、Vim 都照旧。每次组字后把光标矩形、组字范围、选区、应用报给 agent。

组字文字**放在远程应用里**（inline），而不是画在客户端：实测显示普通文本框和 Chromium 只有在有组字文字时才给出准确位置，组字文字本身就是候选窗的锚点。代价是拼音字母仍要等一个来回才在远程应用里出现，但候选窗的出现、更新、翻页、选字都在本机，不等网络。是否再加本地浮层见第 9 节。

### 边界

- **rdaccel**：加一个通用功能：**和输入事件同序的应用消息**（第 4 节）。不知道输入法。
- **rdmachine**：协议消息、客户端的 `NSTextInputClient`、agent 里的转发和坐标换算、输入法程序、安装和启用、菜单。

## 4. rdaccel：和输入同序的应用消息（3.2.0）

组字、上屏和原始按键必须严格按客户端的顺序到达：上屏“你好”之后紧跟一个回车，回车不能先到；最后一次组字更新不能晚于上屏到达（晚到会在远程应用里留下一段多余的组字文字）。

现在按键走 input 通道，应用消息走 message 通道，两个通道各自有序、彼此不保序（[架构](../third_party/rdaccel/docs/architecture.md)的“传输”）。`RDA_INPUT_TEXT` 虽然在 input 通道上，但只能带一个字符，带不了组字文字和选区。

已在 rdaccel 3.2.0 实现（只加不改）：

```c
/* 3.2: an application message on the input channel, so the host gets it in
 * order with the input events sent before and after it (an input method's
 * text, then the keys typed after it). It arrives through the host's
 * on_message, on the thread that calls on_input. Type rules as
 * rda_viewer_send_message(); at most 64 KiB (RDA_ERR_INVALID_ARG above).
 * RDA_ERR_UNSUPPORTED with a host older than 3.2; RDA_ERR_BUSY before the
 * host accepted the viewer. */
RDA_API rda_status rda_viewer_send_input_message(rda_viewer* v, uint32_t type, const uint8_t* payload, size_t n);
```

- Host 不加新回调：照旧经 `on_message` 收到，但它是在 input 通道的解析循环里交出的，和 `on_input` 同一个线程、同一个顺序。应用靠消息类型区分。
- 线上：input 通道上允许 `AppMessage`。能力位 `kCapInputMessages`（128，进 `kCapsAll`），Host 在 `Welcome` 里带上；对方没有这一位时 viewer 返回 `RDA_ERR_UNSUPPORTED`，rdmachine 退回旧做法（键盘语言同步）。线上协议版本号不变。
- 测试（`tests/test_link.cpp`）：消息和按键交替发 300 条，Host 两个回调里看到的顺序和发出的完全一致；3.1 的 Host 不发；64 KiB 上限、只有 viewer 能发；C 接口。每个通道在可靠传输里是独立的有序字节流（丢包重传不打乱同一通道），所以模拟网络下的顺序由传输层保证，没有另写测试。

## 5. 协议（rdmachine）

两种新消息，内容是 JSON。

| 类型 | 方向 | 通道 | 内容 |
| --- | --- | --- | --- |
| `0x10014` ime（`kImeMessageType`） | 客户端 → 服务端 | input（第 4 节，`rda_viewer_send_input_message`） | 见下 |
| `0x10015` ime state（`kImeStateMessageType`） | 服务端 → 客户端 | message | 见下 |

**ime（客户端 → 服务端）**，每条带递增的 `seq`：

```json
{"seq":12,"mark":"ni'hao","sel":[6,0]}     // 组字更新：组字文字和其中的选区（UTF-16 下标）
{"seq":13,"commit":"你好"}                  // 上屏，结束组字
{"seq":14,"cancel":true}                    // 放弃组字（Esc，或客户端的输入法自己清空）
{"seq":15,"mode":"on"}                      // 客户端开始/停止用本机输入法："on"、"off"
```

**ime state（服务端 → 客户端）**，有变化才发：

```json
{"available":true,                // 服务端装了并启用了 RDMachine 输入法
 "client":true,                   // 远程有接受输入法的输入框（输入法 activate 了，且不是安全输入）
 "secure":false,                  // 安全输入（密码框）
 "app":"com.apple.Safari",
 "seq":12,                        // 这份状态对应客户端哪一条 ime 之后
 "caret":{"display":0,"x":1312,"y":384,"w":2,"h":48},   // 组字起点（没有组字时是光标）的矩形，画面像素
 "marked":[0,6],"sel":[6,0]}
```

- `caret` 是那块显示器画面（每块显示器一条连接，见协议的“扩展桌面”）的像素坐标，左上角为原点，客户端用和鼠标相反的换算得到窗口坐标。
- 服务端在每次组字更新后查询光标：立即查一次；结果为空或和上次相同，16 ms、60 ms 后各再查一次（实测网页进程要 6–48 ms 才能答准）。查询在输入法进程里异步做，**不挡后面的按键**。
- 状态里的 `seq` 让客户端知道这份位置是不是针对当前这段组字，过时的不用。

**能力协商**：服务端在画面连接建立时（`RDA_STATE_CONNECTED`）发一条 ime state，之后有变化再发。客户端收到过、其中 `available` 为真、且 `rda_viewer_send_input_message` 可用（第 4 节的能力位），才进入客户端输入法模式；否则照旧（键盘语言同步）。老服务端从不发，老客户端从不发 ime，各自照旧。（原先打算放进 `session_ready`，实现时改了：可用与否在会话中途也会变——用户刚在系统设置里加上输入法——而且这样不用经过守护进程。）

## 6. 客户端

### 6.1 什么时候用本机输入法

同时满足才用：服务端支持（第 5 节）、菜单 “View → Use This Mac’s Input Method” 打开（默认打开）、本机当前输入源是输入法（`input_method` 为真）、服务端最近的状态 `available` 且不 `secure`、不是只看模式、不在登录窗口。

否则按现在的方式发原始按键，并照旧同步键盘语言（客户端用普通布局时，服务端选对应的布局，和今天一样）。

### 6.2 按键怎么走

现在所有按键经本地监视器（`addLocalMonitorForEventsMatchingMask`）直接变成原始按键（`remote_window.mm` 的 `key:`）。改成：

1. 带 ⌘ 或 ⌃ 的键、修饰键本身（flagsChanged）、功能键：照旧发原始按键。正在组字时先让本机输入法结束组字（`[inputContext discardMarkedText]` 前输入法会自己决定上屏或放弃）。
2. 其他键按下：交给 `[inputContext handleEvent:]`。
   - 输入法调用 `setMarkedText:selectedRange:replacementRange:` → 发 `mark`。
   - 输入法调用 `insertText:replacementRange:` → 发 `commit`。
   - 输入法不要这个键（`handleEvent:` 返回 `NO`，或者调用 `doCommandBySelector:`，比如没在组字时的方向键、回车、退格）→ 发这个键的原始按键。
3. 键抬起：按下时发了原始按键的，抬起也发；按下被输入法吃掉的，抬起也吞掉（记下被吃掉的键码）。
4. 鼠标按下前：正在组字就先结束组字（和本机应用点别处时一样）。

`commit` 走 input 通道，和原始按键严格同序，所以“上屏 + 回车”“上屏 + ⌘S”都不会乱。

### 6.3 候选窗贴在哪

`firstRectForCharacterRange:actualRange:` 用服务端最近的 `caret` 回答（换算成屏幕坐标，矩形底边就是候选窗的顶边）。

- 一段组字刚开始（第一个字母）时还没有远程位置，先用上一次的 `caret`；也没有的话用鼠标所在的位置。
- 收到对应当前 `seq` 的新 `caret` 后调用 `[inputContext invalidateCharacterCoordinates]`，本机输入法立即把候选窗挪过去。实际效果：第一个字母后约一个来回内候选窗挪到位，之后的字母不动（组字起点不变）。

### 6.4 远程没有输入框时

状态里 `client` 为假（游戏、某些不支持输入法的应用、焦点不在文字上）：仍用本机输入法组字（和本机 Mac 一样，拼音在访达里也会吃字母），上屏的文字由服务端用 rdaccel 的 `RDA_INPUT_TEXT` 注入（现有路径：临时切 ASCII 布局，`CGEventKeyboardSetUnicodeString`）。这时远程看不到组字文字，客户端在 `caret`（或鼠标）旁画一个小浮层显示组字文字，只为这种情况。

### 6.5 密码框

`secure` 为真：不用本机输入法，发原始按键；服务端选 ASCII 布局（和登录窗口一样）。本机 Mac 遇到密码框也是这样。

## 7. 服务端

### 7.1 RDMachine 输入法（新程序）

- `RDMachine Input.app`，装在 `/Library/Input Methods`（pkg 以 root 安装），bundle id `org.rdmachine.inputmethod.Remote`，`InputMethodServerControllerClass`、`InputMethodConnectionName` 按 InputMethodKit 的要求写；用 rdmachine Local Signing 签名，和服务端一样。
- 控制器（每个输入框一个实例）：
  - `handleEvent:client:`：一律返回 `NO`。组字和上屏不经过按键，经过 agent 的消息。
  - 收到 agent 的 `mark`：`setMarkedText:` 到当前客户端（系统的下划线样式用 `markForStyle:`）；然后按第 5 节的时机查询 `markedRange`、`selectedRange` 和位置，报给 agent。位置依次问，拿到非空的就停：组字起点的 `attributesForCharacterIndex:lineHeightRectangle:`；组字最后一个字的同一查询（用它的右边缘）；光标前一个字（右边缘）。
  - 收到 `commit`：`insertText:replacementRange:`，回执给 agent；收到 `cancel`：`setMarkedText:@""`。
  - `activateServer:` / `deactivateServer:`：报给 agent（远程有没有输入框、是哪个应用）。
  - `commitComposition:`（系统在切应用、点别处时调用）：把当前组字原样上屏，告诉 agent “组字被远程结束了”，agent 转告客户端，客户端 `discardMarkedText`。
- 不记文字内容到日志。

### 7.2 agent 和输入法之间

- agent 在 `~/Library/Application Support/RDMachine/ime.sock` 开 Unix 套接字，帧格式和守护进程/agent 之间一样（`AgentChannel`：4 字节长度 + JSON）。输入法进程启动后连上来；agent 检查对方的代码签名和自己一致（和守护进程检查 agent 一样）。
- 顺序：agent 收到 `commit` 后，在输入法回执之前**不注入后面的原始按键**（排队）。回执很快（`insertText` 在原生应用里约 1 ms）；超过 100 ms 没回执就放行并记日志，免得一个卡住的应用让整个键盘停住。`mark` 不需要等。
- 输入法没连上（没启用、崩了）：状态 `available` 为假，客户端退回旧做法。

### 7.3 输入源

- 客户端发 `mode:"on"`：agent 记下当前输入源，选中 RDMachine 输入法（`rda_injector_select_input_source`）。它放过的按键由应用按它关联的 ASCII 布局（ABC）解释，所以不组字时和 ABC 一样。
- `mode:"off"`、会话结束：切回记下的输入源；客户端改用普通布局时按现有的 `ChooseInputSource` 选。
- 登录窗口不用（第三方输入法在那里不存在），照 3.2.3 选 ASCII 布局。
- 安全输入：agent 每次组字前和状态变化时查 `IsSecureEventInputEnabled()`，为真就报 `secure`。

### 7.4 坐标换算

输入法给的是屏幕坐标（点，左下角原点，主屏为基准）。agent 找出包含这个点的会话显示器（虚拟显示器或真实显示器，`CGDisplayBounds`），换成那块显示器画面的像素（乘缩放、翻转 y），发给那块显示器的连接上的客户端窗口。找不到显示器（矩形为空、在屏幕外）就不发 `caret`，客户端继续用旧的位置。

### 7.5 安装和启用

- pkg 安装 `RDMachine Input.app`；agent 启动时 `TISRegisterInputSource`。
- 实测启用必须用户手动做一次：服务端菜单里显示 “Client input method: not enabled — Open Keyboard Settings…”，点了打开系统设置的键盘页并说明加哪一项；启用后显示 “ready”。
- 没启用时一切照旧，不影响现有用户。

## 8. 效果预期和怎么量

| 动作 | 现在 | 方案 |
| --- | --- | --- |
| 打一个拼音字母，候选窗更新 | 一个来回 + 截屏、编码、解码 | 本机，立即 |
| 翻页、方向键选候选 | 同上 | 本机，立即 |
| 组字的拼音出现在远程应用里 | 同上 | 同上（没变，见第 9 节） |
| 上屏的字出现 | 同上 | 同上 |
| 候选窗挪到远程光标旁 | — | 一段组字的第一个字母后约一个来回 |

按“测量，不靠估计”：实现后在真实使用中量（客户端已有输入到画面的响应时间 `_responseMs`），并新增两项：按键到本机候选窗更新的时间（应接近 0）、一段组字开始到候选窗挪到位的时间。对比同一段文字在旧做法和新做法下的数值。注入按键或点击的测试先问 lroel：lroel 常通过 RDMachine 连着这台 Mac。

## 9. 以后再看

- **客户端本地画组字文字**：如果拼音字母等一个来回才出现仍然觉得不跟手，可以在客户端把组字文字画在 `caret` 处的浮层上，同时照样发给远程（远程仍需要组字文字来给位置）。难点是浮层的字体和远程不一致、和远程画面上随后出现的同一段文字重叠。先量第 8 节，再决定。
- **上下文**：有的输入法用光标前的文字做联想（`attributedSubstringForProposedRange:`）。可以让服务端带上光标前几十个字符。个人使用，隐私上可以接受，但先看有没有输入法真的用。
- **终端里的 Vim 普通模式**、游戏：和本机 Mac 一样，用户自己切到英文。

## 10. 阶段

1. **rdaccel 3.2.0**：和输入同序的应用消息，单元测试，CHANGELOG。
2. **服务端**：RDMachine 输入法、agent 的套接字和转发、坐标换算、输入源切换、菜单里的启用状态、pkg。只有服务端时客户端照旧，可以先装上验证输入法本身（用一个测试客户端发 ime 消息）。
3. **客户端**：`NSTextInputClient`、按键分流、候选窗位置、小浮层、菜单开关（“用这台 Mac 的输入法”，默认打开）。扩展桌面的显示器窗口里的按键也改走主画面的连接（按键进的是前台应用，和显示器无关），这样和输入法的消息同序。

前三个阶段已完成（6.2.0）。
4. **实测**：第 8 节的数值，包括终端、信息等源码定不了的应用，lroel 实际使用的感受，写进[决定](decisions.md)。

版本：rdaccel 3.2.0；rdmachine 6.2.0（客户端和服务端都要升级才生效，任何一边是旧的都照旧工作）。
