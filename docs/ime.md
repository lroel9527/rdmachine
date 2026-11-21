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

菜单栏上单独的“输入法”菜单，三选一（6.3.1；6.3.0 放在“显示”菜单里、每项前缀“输入：”，lroel 嫌挤、不好看；6.2 是两个会冲突的开关）：

- **用这台 Mac 的输入法**（默认）：服务端支持（第 5 节）、状态 `available` 且不 `secure`、不是只看模式时，客户端发 `mode:"on"`，服务端**一直**停在 RDMachine Input，不管客户端当前是拼音还是 ABC（lroel 要求：选了哪种方式，服务端就自动选对应的输入法）。客户端是输入法（`input_method` 为真）时按键先交给本机输入法；是普通布局（ABC）时按键原样发，经 RDMachine Input 放过，在服务端按 ABC 解释。条件不满足时（老服务端、没加 RDMachine Input、密码框）退回同步键盘语言。
- **同步键盘语言**：发 `mode:"off"`，服务端切回原来的输入源，之后跟着客户端的输入源切（和 6.1 一样）。
- **按键原样发**：两样都不做。

已知的限制：用法语之类非 ASCII 布局又选了第一项时，切到 ABC 布局打的键在服务端按 ABC 解释。

键盘语言同步的消息在服务端支持时也走输入通道（6.3.1）：lroel 来回切换三项时两边一度对不上，过一会儿才恢复。原因是从“用这台 Mac 的输入法”切到“同步”时，`mode:"off"` 走输入通道、同步走消息通道，两条通道之间不保序；同步先到时服务端还在 RDMachine Input 模式，把它忽略了，随后切回原来的输入源，要等客户端下次重发同步才对上。同一版还修了另一处：画面连接比窗口接上会话更早，那时到的 ime state 被丢掉，客户端要等下一条状态才进入模式。

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

- 光标位置要在打第一个字母**之前**就有（6.3.0；lroel 发现空输入框打第一个字时候选窗位置不对：系统拼音只在下一个按键时才重新问位置，`invalidateCharacterCoordinates` 不会立刻挪动第一个字的候选窗）。所以服务端不等组字就报：输入框获得焦点时（RDMachine Input 的 `activateServer:`，30 ms 后）、每次鼠标抬起和按键抬起后（agent 让它 60 ms 后报，后来的请求取代还在等的）。空输入框问字符 0 也有位置（探测第一版：Safari、Claude 都有）。
- 还是没有远程位置时，先用上一次的 `caret`；也没有的话用鼠标所在的位置。
- 组字期间只报组字起点，查不到就不报（6.3.3）。lroel 发现候选窗在 Claude 里抖，调试日志（`ImeDebugLog`，6.3.2）显示：Chromium 的 `markedRange` 有时是编的（报成第 5774 个字符，光标在第 2 个），按它查不到，6.3.0 的顺序就退到“光标前一个字”，候选窗跟着光标一格格右移；148 次上报里 112 次组字时什么也查不到。现在组字起点查不到时再用“光标 − 组字长度”算一次，都没有就让客户端保持原位。
- 不组字时先查光标本身，再查前一个字的右边缘（6.3.3）：Chromium 只有从光标开始的查询走光标缓存，其余走选区缓存，点鼠标后那里还是旧位置（日志里光标点回第 59 个字，报的还是第 84 个字的位置，整段组字的候选窗停在旧处）。点鼠标或按键后除了 60 ms 那次，250 ms 时再确认一次。
- 模式里的输入源被系统换掉时再选回 RDMachine Input（6.3.4）。会话从锁屏开始时，被控端在解锁前后选了 RDMachine Input，系统随后又把它换成 ABC，被控端仍以为在 RDMachine Input：客户端收不到可输入的字段，打字只剩英文。现在每秒检查一次，模式里当前输入源不是 RDMachine Input 且没有安全输入（密码框、锁屏）时就再选一次，并记一行日志；与关模式在同一把锁下，不会在恢复原输入源之后又选回来。
- 模式里打文字不再切走 RDMachine Input（6.3.5，rdaccel 3.3.0）。上屏找不到能接的输入框时退回 `RDA_INPUT_TEXT`，rdaccel 打文字前会从输入法切到 ASCII 布局，要等之后有按键才切回；这个模式里字母都在客户端组字，被控端只收到上屏，于是一直停在 ABC（6.3.4 之后变成每秒来回切：探针记到 08:53:29–37 每秒一次）。现在进入模式时用 `rda_injector_set_text_leaves_input_method(0)` 关掉这一步，退出时打开；RDMachine Input 不接任何按键，文字原样进去。
- 每次上报都带上“这个输入框有没有焦点”（6.3.6）。被控端每次开会话都把“有输入框”清成否，RDMachine Input 只在焦点变化时才发 `client`，会话开始前已经点进输入框就一直是否：lroel 在 Claude 里打字的整段调试日志，128 条状态全是 `client:false`。客户端于是当成不支持输入法的应用，另开一个小窗显示拼音（旧版的样子），候选窗跟着乱动。现在每条 `state` 带 `active` 和 `app`，有焦点的那条就把状态改成有；会话开始时也先按桥上记着的状态填好。
- 上屏后候选窗先跟到猜的位置（6.3.7，客户端）。lroel 在高延迟下发现光标对、候选窗不对：选完一个词马上打下一个拼音时，被控端的新位置要一个来回才到，客户端的输入法当场来问，拿到的是上一段的组字起点，正好压在刚上屏的字上。更糟的是被控端的回答带的是上屏那条的序号，比已经开始的新组字小一号，客户端按“旧消息”忽略，这一整段组字候选窗都不再挪。现在上屏时按系统字体量出上屏文字的宽度，按之前几次上屏的实际宽度换算成像素，先给输入法一个猜的位置；被控端的回答到了就换成真的，上屏那条的回答也算数。
- 换了输入框就丢掉旧位置（6.3.8）。lroel 连上后第一段拼音的候选窗在错的地方：日志里位置还是之前 Finder 的（5,572），点进 Claude 后一直没变。被控端原来只在“没有输入框了”时清掉位置，切换应用时新应用的激活先到、旧的放手时已不是当前输入框，这句不会来；Chromium 刚获得焦点的输入框在第一次上屏前什么都答不出来（`sel=-`），也没有新位置顶替。现在任何输入框激活都清掉旧位置；客户端在换了应用时也清掉它记着的位置，没有位置时用最后一次点击的地方，而不是指针现在的位置。
- 只认有焦点的输入框的上报（6.3.9）。每个应用、每个输入框在 RDMachine Input 里各有一个控制器，失去焦点后它的定时上报还会再发几次。lroel 切到 Finder 后，正在打字的改名框报了 (853,244)，80 ms 后已经没有焦点的桌面又报了 (5,572)，把它盖掉，候选窗跳到左下角。上报从 6.3.6 起就带 `active`，现在被控端不用 `active:false` 的上报，RDMachine Input 也不再从失去焦点的控制器发。
- 系统在被控端结束组字时，只插入我们自己标记的文字（6.3.10）。lroel 打字时出现同一段字打了两次。点击、切换焦点或 Chromium 自己结束组字时，系统调用 RDMachine Input 的 `commitComposition`，原来向应用要“标记范围里的文字”再插入；Chromium 的标记范围可能是编的（见 6.3.3），拿到的可以是框里已有的文字，于是又打了一遍。现在插入的是自己最后一次标记的文字。调试日志补记被控端结束组字（ended）和输入框焦点变化。当时调试日志是关的，原因是按代码推断的，没有实测确认。
- 6.3.10 的推断不对，6.4.0 改回（见下）。Fable 独立审阅 6.3.4–6.3.11 后，对照代码和日志核实：打两次的真正原因是上屏回执按“收到过的最大序号”判断（见下第一条）。
- 重连后整段会话“没有输入框”（6.3.11 诊断）。IMK 自己的日志和 6.3.11 加的控制器日志显示：20:31:50 重连时 macOS 激活 Claude 的控制器后同一毫秒又把它停用，之后没有激活；20:33:03 从 Finder 切回 Claude 时也一样。Claude 有两个控制器（A 是聊天输入框，B 是另一个视图，从没答出过位置），macOS 在两者之间切换的顺序不固定，有时最后一条是停用。原因在 macOS 和 Chromium 里，我们改不了顺序，只能不依赖它（6.4.0 的几条）。

6.4.0 按审阅逐条修（都核实过）：

- **上屏回执按编号精确对**。原来记“收到过的最大回执序号”，从不清零；客户端每次连接序号从 1 重来，上一段会话的序号（比如 777）比新会话的大，新会话的上屏一发出就被当成“已回执但失败”，按文字注入，RDMachine Input 也放了一遍：同一段字两次，每次重连后的头几次上屏都可能这样。现在 agent 给每条组字、上屏、取消一个自己的 `id`（agent 运行期间一直递增），回执带同一个 `id`，只认这一条的回执。
- **上屏没回执时不再按文字注入**：输入法拿到了，迟早会放；再注入就是两次。它之后若回执“没放进去”，再按文字注入。
- **位置的序号是位置自己的**。原来状态里的 `seq` 是 agent 最近收到的客户端消息，上屏前的旧位置可能带着上屏的序号，客户端的“组字起点”“上屏后的新位置”判断都建立在它上面。现在 RDMachine Input 每次报告带 `after`（报告时已放进去的最后一个 `id`，不分控制器），agent 换算成客户端的序号：第一条还没放进去的消息之前的都算反映了（`mode` 不进输入法，也算）；状态的 `seq` 只在报告里有位置时更新，是这个位置已经反映到的那条。客户端：`seq + 1` 不小于组字第一条的序号就是这段组字的起点，不小于刚上屏那条就是上屏后的新位置（6.3.7 的“位置变了就算回答”的凑合判断去掉了）。
- **同一个输入框失去焦点又拿回来，位置不丢**。每个控制器有编号（`ctl`），激活时编号和上一个相同就是同一个输入框；停用时不再清掉位置（6.3.8 每次激活、停用都清，Claude 停用又激活之后答不出位置，候选窗只能退到点击处）。状态新增 `field`，换了输入框才变，客户端在它变时丢掉记着的位置。
- **只把当前这段组字的结束告诉客户端**。一个控制器失去焦点后还记着它的组字，之后系统让它结束时会插入并发“结束”，客户端把正在打的组字丢掉。现在“结束”带那段组字的 `id`，agent 只在它是当前这段时才转告；控制器重新激活时，应用里已经没有组字的话就清掉它记着的。
- **系统结束组字时，只在输入框里还有组字时插入**（撤回 6.3.10 的无条件插入）。Chromium 失去焦点时可能已经把组字当普通文字留下了，再插就是两次；插入的仍是自己标记的文字，不用应用报的标记范围。
- **按键到了谁那里，谁就有焦点**。macOS 停用了仍有焦点的控制器后，退路是按文字注入；如果这些按键（或方向键、回车）被送到 RDMachine Input 的某个控制器，说明它其实是当前的，马上恢复。
- **当前控制器被释放时说一声**“没有输入框了”（原来弱引用被清零，什么也不发）。
- **RDMachine Input 重连时清掉旧状态**：原来 agent 还按旧的“有输入框”告诉客户端，实际上传输已断。
- **切回 RDMachine Input 不再持有会话状态的锁等主线程**：原来每秒的检查拿着 `ime_mu_` 调用要等主线程的接口（每次最多 500 ms），主线程刷新菜单时也要这把锁，两边互等到超时。现在输入源的切换用单独一把锁。
- **只对 RDMachine Input 不切到 ABC**（rdaccel 3.4.0 `rda_injector_set_text_passthrough_source`）：3.3 的开关对所有输入法都不切，系统把输入源换成拼音的那一会儿，按文字注入的字会被拼音吞掉。
- **客户端的输入法回应“丢掉组字”时插入的那次不再发**（Cmd/Ctrl 快捷键、点击、远程结束时），只发我们自己的那次上屏，直到下一个按键交给输入法。
- 调试日志的每条报告记 `answers`（反映到哪条）和 `ctl`。RDMachine Input 在系统日志里记控制器的创建、释放、激活、停用和按键接管（不含文字）：`/usr/bin/log show --predicate 'subsystem == "org.rdmachine.inputmethod"'`。

6.5.0：**被控端只在用户改模式时切换输入源**。6.4.0 装上后重连仍然“没有输入框”：IMK 日志里，每次被控端从后台选中 RDMachine Input，前台应用里都是激活后 1 毫秒又停用（今早 6.3.4 每秒重选 6 次，6 次都是；测试工具从后台切换也是；今晚两次重连也是），之后要等焦点变化或一个真实按键才再激活；单按 Control、Shift 都不行，被控端逐字补打的文字也不行（6.4.0 的“按键到了谁那里”从没触发：按键根本没进 RDMachine Input）。lroel 拒绝了“用隐形窗口抢一下焦点”的绕法：根本原因是被控端在切换，而 lroel 最初的想法就是被控端不用切换输入法。原来的切换时机：

- 每次会话开始切到 RDMachine Input、结束切回（重连 = 一来一回）；
- 客户端在“用这台 Mac 的输入法”下连接时仍同步键盘语言，被控端先切到拼音再切回；
- 进出密码框时客户端发 `off`/`on`，被控端切回拼音又切回来；
- 6.3.4 的每秒检查（`KeepImeSource`）自己切回 RDMachine Input，也会和用户的手动切换打架。

现在：会话结束不切回；客户端在这个模式下不同步键盘语言；密码框由客户端改发原始按键（RDMachine Input 原样放过，macOS 在密码框里本来也不用输入法）；删掉每秒检查；客户端每次连接说一次模式，`off` 时被控端把记下的原输入源放回去（换成“同步键盘语言”或“按键原样发”时）。代价：远程用完后 Mac mini 停在 RDMachine Input，本机打中文要自己切回拼音（Ctrl+空格）。仍会发生的：用户在被控端切换应用时，macOS 偶尔也给出“激活后马上停用”（20:33:03），这时退回客户端小窗和逐字补打，下一次焦点变化恢复，不做绕法。

6.5.1：**模式打开时被控端主动要一次光标位置**。6.5.0 的重连保住了输入框（整段 `client:true`），但会话第一个状态里没有光标：以前每次连接都切换输入源，macOS 激活 RDMachine Input 时它会报一次位置；现在不切换就没有激活，客户端组字时又不发按键，到第一个字母之前没人要位置，候选窗先落在鼠标指针处，第一个字母的报告到了才挪过去。现在客户端说“用这台 Mac 的输入法”时，被控端就请 RDMachine Input 报一次。

6.5.2：**只有真的接住了组字的输入框才算“有输入框”**。6.5.1 里重连和第一个候选窗都对了，但 Claude 有时自己把焦点交给它的另一个视图（日志 21:31:18、21:32:04：聊天输入框 ctl 1 停用 250 ms 后另一个视图 ctl 2 激活，中间没有点击）。那个视图收下 `setMarkedText` 却不显示，每次组字后的报告都是 `marked=-`；被控端仍说“有输入框”，客户端就不显示自己的小窗，拼音哪儿都看不到，lroel 只能点一下（日志里的 cancel）让焦点回到输入框。现在 RDMachine Input 在组字 80 ms 后检查输入框里有没有组字，没有就告诉 agent“没有接受输入法的输入框”（客户端改用自己的小窗显示拼音，上屏按文字注入，和真键盘打字走同一条路）；上屏时组字从没出现过，也不往里插，回执“没放进去”，由 agent 按文字注入。之后哪次组字它接住了，或者别的输入框激活，就恢复。判断依据是输入框的实际行为，不是 macOS 的激活顺序。

6.5.3：**没有能输入的地方，就不组字**（lroel 在 Mac 上实测：点到不能输入的地方，任何输入法都不出组字框）。6.5.2 在那种情况下改用客户端小窗显示拼音、上屏按文字注入，和本机行为不一致。现在：客户端只在状态说“有接受输入法的输入框”时才用本机输入法组字，否则按键原样发（服务端的 RDMachine Input 原样放过，和插在那台 Mac 上的键盘一样）；客户端的小窗去掉了。RDMachine Input 的控制器激活后，要等输入框表明能输入（答得出选区，或接住了组字）才说“有输入框”，没表明就过 30 ms、280 ms 再问，之后有焦点时每半秒问一次；Claude 的另一个视图从来答不出选区，所以在打字之前就认出来了，不再要等第一个字母。已经能输入的输入框在一次报告里答不出选区了（同一个视图里点到了不能输入的地方），也改回“没有输入框”。

6.5.4：**远程那边没有能输入的地方时，客户端的远程窗口对 macOS 也不是输入框**（客户端）。lroel 发现在 Claude 的对话里用触控板滚动时，客户端自己的“拼音/ABC”输入源提示频繁弹出。服务端日志：滚动时 Claude 把焦点从聊天输入框移到它的另一个视图，2 秒后再移回来，这段时间被控端正确地说“没有能输入的输入框”，客户端也不再组字；但客户端的远程视图始终向 macOS 提供文字输入上下文，macOS 一直把它当输入框，没有远程光标时“光标”还落在最后一次点击处或指针处。本机 Mac 上，没有能输入的地方就没有输入法、也不出提示。现在远程视图只在被控端有能接受输入法的输入框（且用本机输入法、不在密码框、不是只观看）时才向 macOS 提供输入上下文（`-inputContext`），否则返回空，变化时请 AppKit 重新取。客户端自己调用输入法（取消组字、让候选窗重新定位、把按键交给输入法）用的是视图自己的上下文，不受影响。
- 收到对应当前 `seq` 的新 `caret` 后调用 `[inputContext invalidateCharacterCoordinates]`，本机输入法立即把候选窗挪过去。实际效果：第一个字母后约一个来回内候选窗挪到位，之后的字母不动（组字起点不变）。

### 6.4 远程没有输入框时

状态里 `client` 为假（游戏、某些不支持输入法的应用、焦点不在文字上）：客户端不用本机输入法组字，按键原样发过去，和本机 Mac 一样：lroel 在 Mac 上实测，点到不能输入的地方，任何输入法都不出组字和候选窗。服务端停在 RDMachine Input，它把按键原样放过。（6.5.3 以前这里仍用本机输入法组字、客户端画一个小浮层显示组字文字、上屏按文字注入；那和本机行为不一致，已去掉。）

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

- 客户端在“用这台 Mac 的输入法”模式下发 `mode:"on"`（不论它当前是拼音还是 ABC）：agent 记下当前输入源，选中 RDMachine 输入法（`rda_injector_select_input_source`）。它放过的按键由应用按它关联的 ASCII 布局（ABC）解释，所以不组字时和 ABC 一样。
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
