# IAP2 Tab5 播放控制端：需求与 UI 设计草案

> 状态：v0.1 草案，先对齐交互和范围，再开始写页面。
>
> 目标平台：M5Stack Tab5（ESP32-P4，5 英寸 1280×720，16 MB Flash / 32 MB PSRAM）
>
> 功能参考：`../iap2mediactrl`（iAP2/iOS 媒体控制）
>
> 交互与视觉参考：[rGnng/headunit](https://github.com/rGnng/headunit)（800×480、旋钮导航、MPD 控制端）

---

## 1. 项目目标

做一个 **iPod / iPhone 音乐控制端**：

- 通过 USB 连接 iOS 设备，读取 Now Playing、媒体库、封面、队列和设备状态；
- 通过 iAP2 HID 控制播放、暂停、切歌、跳转、音量、随机/循环等；
- 界面以 **旋钮 + 按键** 为主输入，触摸屏只是辅助，并且可以在设置里完全禁用触摸；
- 最终运行在 Tab5 上，界面为 1280×720 横屏。

当前阶段 **没有后端**。先把 UI、输入映射、焦点模型和 mock 播放器做出来，用桌面模拟器一点点调；真实 iAP2 后端后续替换 mock。

### 第一版范围

| 范围 | 内容 |
| --- | --- |
| 必做 | 5 个界面：Now Playing + Queue、Albums、Artists、Songs、Settings |
| 必做 | 全部界面可用旋钮和按键完成操作，不依赖鼠标 |
| 必做 | 触摸可作为辅助输入，设置里可关闭 |
| 必做 | Mock 媒体库、Mock 播放状态、Mock 队列 |
| 必做 | 1280×720 桌面模拟器，可截图、可注入按键 |
| 后续 | 真实 iAP2 连接、媒体库同步、封面、iAP2 队列、设备电量 |
| 暂不做 | AFC 数据库导入、HTTP API、Wi-Fi 控制、播放列表编辑、歌词 |

### 不在第一版里做的事

- 不做完整的 iAP2 / MFi / USB Host 实现；
- 不做多设备管理；
- 不做搜索输入法（硬件没有字母键）；
- 不做复杂的音效/均衡器设置。

---

## 2. 参考项目与取舍

### 2.1 `../iap2mediactrl`

提供最终功能范围和数据含义：

- Now Playing：标题、艺术家、专辑、专辑艺术家、时长、进度、播放状态、队列位置、随机/循环、封面、播放速率、设备名、电量；
- 媒体库：Tracks、Albums、Artists、Album Artists、Genres、Recently Added、Device Playlists；
- 队列：设备队列（只读）和 accessory 侧构建队列；
- 控制：播放/暂停、上一首/下一首、停止、seek、队列跳转、音量、静音、随机、循环；
- 设备状态：连接状态、link state、电池、充电状态、最后事件/错误；
- 封面：iAP2 FT 和 AFC 两条路径，最终 UI 只需要“有一张可显示的封面”；mock 使用彩色科幻占位封面。

Tab5 第一版只取其中与播放控制最相关的部分：**Now Playing、Albums、Artists、Songs、Settings、Queue**。Genres、Recently Added、Playlists、搜索、AFC 同步都后置。

### 2.2 `rGnng/headunit`

直接借用它的交互思想：

- 固定分辨率、深色、低干扰的“车机”视觉；
- 旋钮上下移动选择，按下确认；
- 分区键直接跳转，再按一次回到该分区顶部；
- Back 键逐级返回，根界面返回 Now Playing；
- 车机式分区导航和深色视觉（本项目不保留底部提示条）；
- 选择条 + 左侧色条，不只靠颜色表达焦点；
- Mock backend 让 UI 可以先于真实后端开发；
- 长按 Enter 打开上下文菜单（可作为后续扩展）。

### 2.3 `headunit-lvgl`

本地已经有一版 LVGL 8 的移植草稿，可以借用：

- 深色主题和列表焦点样式；
- `scr_nowplaying` / `scr_albums` / `scr_list` 的页面拆分；
- 分区导航和状态信息（本项目不使用底部状态栏）；
- `player.h` / `stub_player.c` 的后端抽象。

但它只有 800×480 的简单列表，输入驱动也是空的。本项目会在它之上重做焦点引擎和 1280×720 布局。

---

## 3. 硬件输入与映射

### 3.1 物理输入

- 1 个可按压旋钮：逆时针 / 顺时针 / 按下；
- 7 个按键：`key1` … `key7`；
- 输入设备以 **USB HID 键盘** 形式输出标准按键；
- 触摸屏：电容触摸，作为辅助输入，可禁用。

### 3.2 HID 到逻辑输入

固件/模拟器先统一成逻辑事件，UI 不直接读 HID：

| 物理动作 | HID 键 | 逻辑事件 |
| --- | --- | --- |
| 旋钮逆时针 | `Up` | `INPUT_PREV` |
| 旋钮顺时针 | `Down` | `INPUT_NEXT` |
| 旋钮按下 | `Enter` | `INPUT_ACTIVATE` |
| key1 | `1` | `INPUT_KEY_1` |
| key2 | `2` | `INPUT_KEY_2` |
| key3 | `3` | `INPUT_KEY_3` |
| key4 | `4` | `INPUT_KEY_4` |
| key5 | `5` | `INPUT_KEY_5` |
| key6 | `6` | `INPUT_KEY_6` |
| key7 | `7` | `INPUT_KEY_7` |

> 旋钮方向可以在设置里做“反向旋转”开关，方便适配不同硬件装配方向。

### 3.3 推荐按键功能（待确认）

先按“5 个界面 + 返回 + 播放/暂停”来设计：

| 键 | 建议功能 |
| --- | --- |
| key1 | Now Playing；已经在该界面时，把焦点移到 Queue |
| key2 | Albums |
| key3 | Artists |
| key4 | Songs |
| key5 | Settings |
| key6 | Back：返回上一级；根界面返回 Now Playing；Now Playing 的 Up Next 打开时先关闭面板 |
| key7 | Play / Pause（全局） |

额外 HID 键：

| 键 | 建议功能 |
| --- | --- |
| Page Up | 非 Now Playing：上一个分区；Now Playing：显示/隐藏 Up Next 面板 |
| Page Down | 下一个分区；到 Settings 停住 |
| Backspace | 同 key6：返回上一级；Albums/Artists 详情页的 Back 行同效 |

Page Down 只切换 menubar 当前项，到头停在 Settings。Page Up 在其它页面切到上一个分区；已经在 Now Playing 时改为切换 Up Next 面板：第一次显示，再按隐藏。Up Next 打开时按 Back 也会关闭面板。

Now Playing 直接显示正常播放布局，不显示加载页或页面标题。其它页签切换时先更新 menubar，并显示新页标题及 `Loading...`；首帧绘制完成后再构建内容，列表先于右侧详情预览显示。加载态与内容态复用同一标题布局，位置保持一致。连续切页会取消旧页待加载任务；加载期间收到的旋钮、Enter 等输入按顺序应用到当前页，媒体键保持即时响应，按键反馈不重复播放。

**上一首/下一首、随机/循环不进入焦点链。**

- 上一首/下一首/随机/循环：只支持触摸点击；
- 播放/暂停：key7 或触摸 play 按钮；
- Now Playing 的键盘焦点只包含 Up Next 队列行（队列可见时）；
- iPod 音量：D100H 按住旋钮并转动，顺时针增大、逆时针减小，每格发送一次原生相对音量指令；转动后松开不触发确认。普通旋钮点击在松开时确认，USB 与 BLE 行为一致；
- 更复杂的操作（加入队列、播放下一首等）：长按 Enter 打开上下文菜单。

如果硬件按键已经有丝印（例如 `PREV` / `NEXT` / `VOL`），需要重新排映射；这一点在开工前确认。

### 3.4 长按

当前模拟器已支持：

- 长按旋钮 650 ms：在 Now Playing 打开当前曲目的上下文菜单；
- 短按 Enter 在松开时触发，长按 Enter 只发送上下文菜单事件，不会先触发一次普通确认；
- 模拟器键盘也可以直接按 `C` 打开上下文菜单，方便调试。

长按由固件/模拟器统一产生“按下 / 长按 / 松开”事件，不能只发一次 Enter。真机固件需要沿用同样的事件模型；其它页面后续接入上下文菜单时，只需发送 `UI_KEY_CONTEXT`。

### 3.5 触摸

- 默认开启：点按 = 聚焦并激活；上下滑动 = 滚动列表；
- 设置里关闭后：触摸事件完全忽略，不改变焦点、不滚动、不触发点击；
- Settings 的 Touch input 行显示 `On/Off`；界面其它位置不显示触摸状态；
- 关闭状态写入 NVS，重启后保持。

---

## 4. 交互模型：焦点优先

### 4.1 基本原则

1. **任何时刻都有一个明确的焦点**，焦点用左侧 3 px 色条 + 背景高亮 + 加粗文字表示。
2. 旋钮上下 = 在当前焦点列表内移动；到边界不循环，避免误跳。
3. 旋钮按下 = 激活当前项。
4. Back = 退出一层：详情 → 列表；列表 → 根界面；根界面 → Now Playing。
5. 数字键 1–5 永远直接切界面，即使当前在详情页或编辑态。
6. 界面切换后恢复该界面上次的焦点；再次按当前分区键回到该分区顶部。
7. 列表焦点必须自动滚动到可见区域，不能出现“焦点在屏幕外”。

### 4.2 焦点层级

每个界面维护自己的焦点列表，不把全屏所有控件塞进一个全局 Tab 链：

```text
App
├── Now Playing
│   └── queue rows（仅 Up Next 可见时）
├── Albums
│   ├── album list
│   └── album detail track list
├── Artists
│   ├── artist list
│   └── artist songs
├── Songs
│   └── song list
└── Settings
    └── setting rows
```

- 进入详情页时，焦点切到详情内部的列表；
- Back 返回上一层并恢复之前的选中项；
- 分区键切换后，如果该界面有未完成的详情，先保留状态；再次按同一分区键回到根列表顶部。

### 4.3 值控件（亮度）

值控件不要和列表混在一起：

- 第一版的值控件只有 Settings 里的亮度；
- 焦点在亮度行时按 Enter 进入“编辑态”，旋钮上下调节，Enter 确认，Back 取消；
- 编辑态通过数值变化直接反馈，不额外占用界面空间。

### 4.4 列表行为

- Up/Down 移动一行；
- 长列表只创建/更新可见行；Songs 已使用 14 行复用池，Albums/Artists 的数据量还小，暂时保持全量行。
- 选中项保持在视口内，键盘焦点滚动使用即时定位，关闭 elastic rubber-band；
- 触摸滚动仍可拖动，但不会越过列表边界回弹；
- 当前播放项用 `▶` / 高亮标记，与“焦点”是两种不同状态：
  - 焦点：浅灰左边条 + 背景；
  - 正在播放：行首小播放图标 / 文字颜色，不使用同一套高亮。

### 4.5 按键提示

界面不设底部提示条。按键语义写在硬件丝印、产品说明和调试文档里，把屏幕高度留给内容；调试阶段可以在串口日志里查看当前焦点和按键事件。

---

## 5. 界面设计

### 5.1 全局框架与 menubar

1280×720 横屏，menubar 是贯穿全屏高度的竖栏，时间在它顶部，电池在它底部：

```text
┌──────┬──────────────────────────────────────────────────────────────┐
│13:00 │                                                              │  72 px 时间区
│──────│                                                              │
│      │                                                              │
│ ▶/⏸ │                                                              │
│  ◉   │                         内容区                               │  720 px
│  👤  │                       1160 × 720                            │
│  ♪   │                                                              │
│  ⚙   │                                                              │
│      │                                                              │
│──────│                                                              │
│ 🔋   │                                                              │  72 px 电池区
│ 86%  │                                                              │
└──────┴──────────────────────────────────────────────────────────────┘
 120 px
 menubar
```

menubar 也可以放到右侧，用户可以在 Settings 里切换；右侧布局只是把上图左右镜像：

```text
┌──────────────────────────────────────────────────────────┬──────────┐
│                                                          │ 13:00    │
│                                                          │──────────│
│                                                          │          │
│                                                          │  ▶/⏸     │
│                         内容区                           │  ◉       │
│                       1160 × 720                         │  👤      │
│                                                          │  ♪       │
│                                                          │  ⚙       │
│                                                          │          │
│                                                          │──────────│
│                                                          │  🔋      │
│                                                          │  86%     │
└──────────────────────────────────────────────────────────┴──────────┘
```

**menubar**（宽 108 px，高 720 px，左侧或右侧）：

- 顶部时间区 **72 px**：24H 使用 28 px Inter tabular，12H 使用 24 px Inter tabular（无前导零 + AM/PM）；时间标签固定占满时间区并居中；
- 中部菜单区：图标按钮 72×72，图标 40 px，垂直居中；
- 底部电池区 **72 px**：只显示 iOS 设备电量/充电状态图标；不再显示百分比、phone 或电源图标；
- 顺序固定为：Now Playing、Albums、Artists、Songs、Settings；
- Now Playing 图标与实际播放状态对应：播放中显示播放三角，暂停/停止显示暂停图标；
- 其它图标：Albums = `disc`，Artists = `person`，Songs = `music-note-list`，Settings = `gear`；
- 当前项：背景 `#262626` + 朝内容区的 3 px 浅灰边条，图标变白；
- 非当前项：透明背景 + 灰色图标；
- 图标本身也是触摸目标，直接点按可切页；键盘 1–5 仍然直接切页；Page Down 下一分区，Page Up 上一分区；在 Now Playing 按 Page Up 切换 Up Next；
- **menubar 在左还是右由用户选择**，默认左侧（驾驶员在左，靠左更好够到），Settings 里有 `Menubar: Left / Right`；
- 这是开源程序，不把左/右写死；位置设置后续写入 NVS。

**迷你 Now Playing 条**（只在非 Now Playing 页面出现）：

- 高度固定为 **MENUBAR_TIME_H = 72 px**，与 menubar 顶部时间区同高；
- 四条边框全部取消，与上下区域只用底色区分；menubar 与内容区之间也不画竖向分隔线；
- 左侧：56×56 小封面，距迷你播放器左边缘 16 px；
- 中间两行文字：第 1 行歌名（24 px），第 2 行 `artist  |  album`（18 px），单行省略；
- 右侧仅显示 `已播放时间 / 曲目长度`（如 `1:13 / 4:58`），不显示队列位置或总数；使用 18 px Inter tabular，右对齐并垂直居中；
- 最右侧：shuffle、repeat、star 状态图标；开启/收藏时高亮，关闭/未收藏时变暗，star 收藏时显示实心图标；
- 点击整条区域回到 Now Playing；
- Now Playing 页面隐藏这条栏，内容区恢复为 1172×720。

**内容区**：

- 主内容区背景使用 `COL_MAIN_BG (#050505)`，页面面板使用 `#141414`；页面容器不设外边距，也不画边框；
- Now Playing：1172 × 720；
- 其它页面：顶部 72 px 迷你 Now Playing 条 + 下方 1172 × 648 页面内容区；
- 页面容器无外边距；页面面板内部使用 18–20 px padding。

**不设底部提示栏。** 按键语义写在硬件丝印、产品说明和调试文档里，把屏幕高度留给内容。触摸开关状态只在 Settings 的 Touch input 行显示 On/Off。

### 5.2 Now Playing + Queue

左右两栏；两栏之间没有间隔，直接贴在一起，用底色区分：左栏 `#141414`，右栏 `#1E1E1E`。Up Next 默认隐藏，在 Now Playing 按 Page Up 显示，再按 Page Up 隐藏；面板打开时按 Back 也可以关闭。隐藏时左侧 Now Playing 面板占满内容区，并保持横向布局；显示时左栏切换为上下布局。

Queue 显示时的左栏：

```text
┌────────────────────────────────────────┬──────────────────────────────┐
│           ┌──────────────┐             │  Up Next                    │
│           │   Cover      │             │    Next track               │
│           │   360×360    │             │    Following track          │
│           └──────────────┘             │    ...                      │
│  Track Title                       ☆   │    ...                      │
│  Artist - Album                        │    ...                      │
│  1:12  ──────────●──────── 4:35        │    ...                      │
│  🔀       [⏮]  [▶/⏸]  [⏭]        🔁   │                              │
└────────────────────────────────────────┴──────────────────────────────┘
```

- Queue 隐藏时（默认）：内容区左右各留 72 px，360×360 封面距顶部 72 px；信息列位于封面右侧 24 px，宽 644 px。歌名（40 px）、艺术家（32 px）、专辑（32 px）各占一行，到下一行/星标的间距按黄金比例递减（26:16:10）；整组底部对齐，收藏星标的可见下沿距封面底部 24 px，并与文字左对齐。进度行位于封面下方 64 px，时间标签分列两侧；进度行与控制行宽均为 1028 px（左侧 menubar 时，右边缘为屏幕 x=1208）。五个等尺寸控制按钮（shuffle / prev / play / next / repeat，96×84 px）水平居中，点击区域下沿距屏幕底部 48 px；
- Queue 显示时：左栏宽度 620 px，改为上下布局，字号整体比默认横向布局小一档：
  - 四边统一留 36 px；封面水平居中，360×360，距顶部 36 px；
  - 标题行、`Artist - Album`、进度条、控制行统一使用 548 px 宽并居中，左右边缘对齐到 36 px 内边距；
  - 标题 32 px，收藏星标固定在标题行最右侧；
  - 下一行：`Artist - Album`，28 px；
  - 进度条一行，播放时间 18 px，分列左右两侧；
  - 控制行：shuffle 在最左，prev / play / next 居中，repeat 在最右；传输按钮使用标准 84 / 108 / 84 px 和 48 px 图标；
  - 控制行下沿距屏幕底部 36 px；进度条与控制行之间使用弹性间隔，封面与文字组的间距固定；
- 进度条在封面区域下方；普通布局时间标签使用 24 px Inter tabular（88 px 宽），队列布局使用 18 px（72 px 宽），避免走时造成进度条左右抖动；
- shuffle / repeat 在普通布局使用与传输按钮一致的 48 px 图标，队列布局使用 32 px；启用时显示 `#303030` 直角底色；star 普通布局约 30 px（48 ÷ φ，保留 56×56 px 点击区域）、队列布局 32 px，收藏后显示实心星；
- 传输按钮无背景、48 px 图标，只支持触摸点击，不进入焦点链；
- 界面不显示 Playing/Paused 文本，播放状态由 menubar 图标表示；
- 右侧：Up Next 队列占满左栏之外的剩余宽度，内容区右侧留 16 px；标题为 `Up Next`，不显示队列位置/总数计数器；
- 仅显示当前播放项之后的歌曲，当前播放项不在列表中；不显示行号，焦点独立于设备队列下标；
- 队列行：行高 56 px，仅显示歌名（FONT_H2，超长淡出）和艺术家（FONT_H2，靠右 200 px 列，超长直接切断）；不显示序号或时长，移除的序号、时长列及间距全部让给歌名；无行间距；固定 11 行，列表高 616 px；
- Now Playing 背景有三种模式，由 Settings → Now Playing Background 控制：
  - **Theme**：沿用当前面板主题灰（`#141414`）；
  - **Blurred cover**：当前封面按 16:9 填充整个内容区，做高斯模糊，再叠 60% 黑色遮罩；封面本体仍保持清晰；
  - **Cover gradient**（默认）：把封面缩到 32×32，过滤过暗/过亮像素后用 K-Means 提取 5 个主色，再映射到 3×3 控制网格，用二次 Bezier patch 生成 592×360 静态渐变图（RGB565 约 426 KB，真机放 PSRAM），最后拉伸到 1184×720 并叠 60% 黑色遮罩；生成结果按封面缓存，不做动画；
- 长按 Enter（650 ms）打开当前曲目的上下文菜单；菜单为居中弹层，带半透明遮罩：
  - `GoTo`：Go to album、Go to artist；
  - `Star`：Star / Unstar，根据当前收藏状态切换文案和实心/空心星标；
  - `Playback`：Shuffle（Off/On）、Repeat（Off/All/One），右侧显示当前状态；
  - 切换 Star / Shuffle / Repeat 后菜单保持打开并即时刷新，方便连续调整；GoTo 项执行后跳转并关闭菜单；
  - Up/Down 在菜单项间移动，Enter 执行，Back 关闭，点击菜单外部关闭；
  - 菜单使用独立焦点索引，不覆盖或清除底层 Now Playing / Queue 的焦点；
- 焦点顺序：只有队列行（队列可见时）；
- Enter 在队列行上 = 跳到该曲；
- key7 全局播放/暂停，不依赖焦点；
- 封面来自 mock 封面 PNG；缺失时显示灰色占位块，不在封面上绘制文字。

**为什么队列用 Page Up 呼出？** Now Playing 默认把空间留给封面和曲目信息；需要查看/跳转队列时按 Page Up 显示，再按一次隐藏。

### 5.2.1 播放动作与队列

所有“从这里开始播放”的动作都先按界面显示的顺序生成一个有序列表，再从选中项开始播放，其后的项目进入队列（Up Next）：

| 入口 | 动作 | 队列内容 |
|---|---|---|
| Albums 左栏 / 详情 Play | 播放整张专辑 | 专辑全部曲目 |
| Albums 详情 Shuffle | 随机播放本专辑 | 本专辑全部曲目，随机顺序 |
| Albums 详情某一曲目 | 播放该曲 | 该曲及其后的曲目（不回绕） |
| Artists 详情 Play | 播放本艺术家 | 按详情显示顺序（专辑分组）的全部歌曲 |
| Artists 详情 Shuffle | 随机播放本艺术家 | 本艺术家全部歌曲，随机顺序 |
| Artists 详情某一曲目 | 播放该曲 | 该曲到列表末尾，再从列表头回绕到该曲之前 |
| Songs 某一曲目 | 播放该曲 | 该曲到库末尾，再从库头回绕到该曲之前 |
| Playlists 详情 Play / Shuffle | 播放整个列表（随机） | 列表全部曲目 |
| Playlists 详情某一曲目 | 播放该曲 | 该曲及其后的曲目（不回绕） |

- Albums 左栏顶部有一个 `Shuffle All`（不显示在默认位置，向上滚动一行即可看到）：随机播放整个媒体库；真机用 `0x4C09` 从随机曲目开始并打开 iPod 随机模式。
- Shuffle 不再只是切换全局随机标志：按钮直接以随机顺序播放本专辑 / 本艺术家 / 本列表，并打开随机模式。
- 模拟器：队列就是本地列表；随机时对整个列表做一次洗牌，首项即当前播放。
- 真机（iAP2）：Albums、Artists、Playlists 把有序 PID 列表用 `0x4C07 PlayMediaLibraryItems` 发给 iPod，单次最多约 500 首（一帧 2048 字节，每首 8 字节）；随机时打开 iPod 自身的随机模式。
- Songs 真机：库可能超过 2 万首，不发送整张 PID 列表，而是用 `0x4C09 PlayMediaLibrarySpecial`（媒体库 UUID、AllSongs、起始曲目 PID），由 iPod 自己建立队列；顺序是 iPod 的全库顺序，不是界面排序。
- Up Next 列表最多显示当前项起后面的 100 项；队列总数（如 `1/562`）是完整队列长度，超出 100 项的部分仍会播放，但不在列表中显示。
- 真机的 Up Next 读取 iPod 的队列（`0x0003` / `0x0006` 文件传输）；iPod 尚未发送队列时显示我们发出的列表。队列内容变化（即使条数相同）都会刷新。
- 真机的 Up Next 旧说明：，仍显示配件侧状态（见 README 的已知问题）。

### 5.3 Albums

两栏：专辑列表 + 专辑详情。

```text
┌──────────────────────────────────────┬──────────────────────────────┐
│  Albums                     1/128    │  ┌────────┐                  │
│  ┌────┐ Northern Lines               │  │ Cover  │  Northern Lines  │
│  │cov │ Clara Voss · 9 tracks      │  └────────┘  Clara Voss     │
│  └────┘                              │              2019 · 9 tracks│
│  ┌────┐ Slow Meridian                │  01  Prelude            3:12 │
│  │cov │ Clara Voss · 8 tracks       │  02  Northern Lines     4:01 │
│  └────┘                              │  03  Interlude I        2:48 │
│  ...                                 │  ...                         │
└──────────────────────────────────────┴──────────────────────────────┘
```

- 左栏宽度 480 px，行高 70 px，带 54×54 封面缩略图；8 行 × 70 px = 560 px 固定列表高度，触摸滚动时允许边缘出现部分行；
- Albums 左栏复用 10 个行控件：滚动期间每 16 ms 合并更新文字和已缓存封面；停止滚动 140 ms 后，仅补齐当前视口的封面，每批最多 2 张，单次读取后超过 4 ms 则让出 UI。保留的行也重新判断可见性，不依赖 Enter 重建页面；临时读取失败间隔 250 ms 重试、最多 3 次，缺失封面不反复读取；离开页面取消待处理任务。
- 标题右侧显示 `当前选中序号/总数`（如 `1/100`），使用 Inter tabular 18 px，选中项变化时实时更新；
- 左右两栏无间隔，左栏 `#141414`、右栏 `#1E1E1E`，用底色区分；
- 右侧详情始终显示左栏当前选中的专辑，不再显示占位提示文字；
- 两侧面板不再加遮罩；选中专辑行保留 `#262626` 选中底色，浅色条只在旋钮焦点位于左栏时显示；上下移动左栏选中项时右侧内容实时跟随；
- Enter 打开右侧详情并把焦点移到 `Play` 图标；左侧选中行保留选中底色但不显示浅色条；两侧内容始终可以直接触摸操作；左栏滚动位置保持不变；
- 从第一张专辑向上一次即显示并选中 `Shuffle All`；滚动导致行复用刷新时，也同步更新该独立操作行的焦点。负索引操作项（如 `-1`）只在本次访问中保留，切换页面再返回 Albums 时选中第 1 张专辑并隐藏操作行；普通专辑则保留选择和滚动位置。
- 右侧详情曲目列表固定 9 行：9 × 56 px = 504 px，行间无间隔；顶部 108 px 用于封面、标题和 Shuffle/Play 图标，合计 612 px，正好填满右栏可视高度；
- 未打开详情时右侧预览最多 15 首曲目；Enter 打开详情后显示整张专辑的全部曲目，列表可滚动；
- 曲目列表触摸滚动结束时按 56 px 行高吸附（`LV_EVENT_SCROLL_END` 自定义吸附），**不要**使用 `LV_SCROLL_SNAP_START`，否则旋钮焦点会被强制贴在列表顶部；
- 右侧详情顶部的封面/标题右侧是 `Shuffle` 和 `Play` 两个纯图标按钮，与封面垂直居中对齐；Backspace 返回专辑列表并恢复左栏焦点；
- 在曲目列表 Enter = 播放该曲；Back = 回专辑列表；
- 在专辑列表长按 Enter = 上下文菜单（播放整张、加入队列等）。

### 5.4 Artists

两栏：艺术家列表 + 该艺术家的专辑分组。

```text
┌──────────────────────────────────────┬──────────────────────────────┐
│  Artists                     1/24    │  Clara Voss                  │
│  A  Clara Voss            30 songs   │  30 songs  |  5 albums       │
│  B  Harbour Lights        23 songs   │  ← Back                      │
│  C  Ivor Mallory          24 songs   │  [cov] Northern Sessions  ▶  │
│  ...                                 │        2024  |  3 tracks     │
│                                      │    01  Night Passage   4:09  │
│                                      │    02  Paper Weather   3:22  │
│                                      │  [cov] Faded Meridian    ▶   │
│                                      │        1993  |  6 tracks     │
└──────────────────────────────────────┴──────────────────────────────┘
```

- 标题右侧显示 `当前选中序号/总数`（如 `1/24`），使用 Inter tabular 18 px；
- 右侧始终显示左侧当前选中的艺术家；未打开详情时预览最多 15 首曲目，Enter 打开详情后显示该艺术家的全部专辑和曲目，不再显示占位提示文字；
- 顶部压缩为 52 px：艺术家名和 `songs | albums` 统计同一行，右侧是 48 px 的纯图标 Shuffle / Play 按钮；Play 播放该艺术家的全部歌曲；
- 下方列表 10 × 56 px = 560 px，整数行吸附；专辑和曲目共用这个滚动区域；
- 每张专辑是单行 56 px：34×34 封面、专辑名、年份 + 曲目数、右侧播放图标；
- 专辑 header 本身可聚焦，Enter / 触摸 = 播放整张专辑；
- 曲目行 56 px，Enter / 触摸 = 播放该曲；
- Enter 在艺术家上 = 打开详情并把焦点放到 Play 图标；Backspace 返回艺术家列表并保留左栏滚动位置；
- 左右两栏无间隔，左栏 `#141414`、右栏 `#1E1E1E`，用底色区分；选中艺术家行保留 `#262626` 选中底色。

### 5.5 Songs

单栏列表，信息密度高：

```text
┌─────────────────────────────────────────────────────────────────────┐
│  Songs                           1/555                              │
│  ─────────────────────────────────────────────────────────────────  │
│  ♪ 01  Ballad for the Eastern Shore   The Meridian Quartet   4:35   │
│  ▸ 02  Meridian Line                  The Meridian Quartet   3:42   │
│    03  Crosscurrent                   The Meridian Quartet   4:35   │
│    ...                                                              │
└─────────────────────────────────────────────────────────────────────┘
```

- 行高固定 56 px；
- 列表高度固定为 10 行：10 × 56 = 560 px，正好是滚动区域的整数倍；
- 标题右侧显示 `当前选中序号/总数`（如 `1/555`），使用 Inter tabular 18 px；
- 列：播放标记、序号、标题、艺术家、专辑、时长；
- 1280 宽度下标题可以占较大比例；
- Enter 播放当前歌曲，并把当前列表设为播放队列；
- 长按 Enter 打开上下文菜单（播放下一首、加入队列等）；
- 已用 14 行复用池 + 固定高度 spacer 实现虚拟列表；真实媒体库 2 万首时同样只维护可见行。

### 5.5.1 设备可用性与空状态

- **iAP2 模式且没有检测到 iPod**（未识别）：
  - menubar 中除 Now Playing 与 Settings 外的图标显示为暗灰（`#686868`）；
  - Now Playing 菜单图标换成 `bi phone`；
  - Now Playing 页面只显示插头图片，在内容区水平居中、贴底对齐（下边距 24 px），不显示播放器 UI。图片只有一份：`assets/ui/plug-in.png`（52×343），由 `tools/gen_plug_art.py` 转成 LVGL 资产并编译进固件；不从 SD 卡读取任何覆盖文件。
- **SD 卡缺失或挂载失败**：
  - 同样的菜单项置灰；
  - Now Playing 菜单图标换成 `bi sd-card`；
  - Now Playing 页面只显示 `bi sd-card` 加问号图标；
  - 若开机时无卡、之后插入并挂载成功，空状态保持不变并在图标下显示一行 `Press Power to Reboot`，因为媒体库和配置是在无卡状态下加载的，必须重新上电。
- 设备状态每秒从固件推送到 UI；状态变化时 menubar 立即刷新，当前在 Now Playing 时会重建该页面。
- 如果掉线（或 SD 失效）发生在 Albums、Artists、Songs、Playlists 页面，界面立即切到 Now Playing 并显示对应空状态；Settings（含 Bluetooth 子页）保持不动。设备恢复后停留在 Now Playing，不自动跳回。
- 模拟器可用 `SIM_NO_IPOD=1`、`SIM_NO_SD=1`、`SIM_SD_BACK=1` 复现这三种状态。

### 5.5.2 Web 控制台

- 单页控制台，视觉沿用 `../iap2mediactrl`：等宽字体、深色分层背景、1 px 分隔线、直角、小号大写按钮。
- 分区：Player（当前曲目、进度、star/shuffle/repeat/上一首/播放/下一首）、Device（iAP2/usbmux/lockdown/AFC/WiFi/SD/heap/PSRAM/复位原因）、Library（上传 library.bin、复制 iPod 数据库、上传配对 plist）、Power（INA226 读数、充电开关）、System（音量/亮度滑条、声音、调试日志、iAP2 模式、USB 模式、屏幕方向、WiFi 模式与 AP SSID、同步时间、重启）、Firmware（OTA 上传与进度）、Logs（最近日志、实时调试页、下载 SD 日志）。
- 每 2 秒轮询 `/api/status`、`/api/config`、`/api/power`；顶部状态灯显示 iAP2 / SD / WiFi。
- 源码在 `assets/web/index.html`；`tools/gen_web_ui.py` 生成 `firmware/main/web_index.h` 并编译进固件。

### 5.6 Settings

左右两栏。左栏是设置项列表：标题栏 56 px、16 px 内边距，右上角为 `当前序号/总数` 计数器（与其它列表页一致）；设置项行高 64 px，文字 FONT_H2。右栏是当前设置项的内容，没有标题，内容从顶部开始排列。

设置项顺序：iOS Device、Bluetooth、Theme、Now Playing Background、USB Options、Boot Screen、About、Clock。

- **iOS Device**：iAP2/AFC 模式切换、缓存统计和 iAP2 状态；`Battery` 行放在详情最下方（`iAP2 Status` 之后），显示 `CHG` / `NOT CHG` 和电量百分比（iPod 不提供电量时显示 `--%`）。

```text
┌──────────────────────────────────────┬──────────────────────────────┐
│  Settings                    3/8     │  Paired Devices              │
│  AFC & iAP2                          │  …                           │
│  Bluetooth                        ◀  │  Search Devices   PRESS: PAIR│
│  Theme                               │  …                           │
│  Now Playing Background              │                              │
│  USB Options                         │                              │
│  Boot Screen                         │                              │
│  About                               │                              │
│  Clock                               │                              │
└──────────────────────────────────────┴──────────────────────────────┘
```

- 菜单项统一规格：所有可选、只读、带右侧值的菜单项都是 64 px 高、16 px 左内边距、FONT_H2 字号；颜色按语义保留（On 为绿色，Off 与次要信息为灰色，强调项为 accent 色）。
- 说明性文字不随菜单字号：分组标题（如 `Paired Devices`、`Search Devices`）及其右侧提示、`Reboot to apply`、`No paired devices`、`Searching...` 保持原小字号；说明文字与菜单标签左对齐（16 px 内边距）。
- **Bluetooth**：第一行是 Bluetooth On/Off。下方是 Paired Devices（Connected / Disconnected，长按 Enter 忘记设备）。页面底部是 **Search Devices** 分组（`PRESS: PAIR`）：打开页面即开始扫描，新发现的设备出现在该分组下方，选中即配对。已移除顶部的 Search 行。
- **Key Feedback**：`Volume` 行（Enter 进入调节，Up/Down ±5%，Enter 确认）；中间留 14 px inset；下方 `Play sound on` 提供 `Touch`、`Touch + Key`、`None` 三个带勾选项，默认 `Touch + Key`。
  - 触屏点击控件以及键盘按钮操作有声音：左右切换标签页、Back、播放/暂停、上一首/下一首、Enter（旋钮按下）、菜单与收藏。统一在 key dispatch 入口触发，覆盖列表、菜单和编辑模式；同一次按键最多一个提示音。长按上一首/下一首在开始快退/快进时响一次，松开不重复。
  - 列表滚动（触摸拖动、滚轮/上下键移动）不发声。
  - 保存到 `sound_mode`（0 none / 1 touch / 2 touch+key），同时同步旧的 `sound_enabled` 字段。
  - **此前状态（修复前）**：设备上仍无可听见的提示音。已确认的部分：UI 事件 → `feedback_play` → 队列 → `esp_codec_dev_write` 全链路返回成功；I2S 以 16 kHz 单声道打开；ES8388 `DACPOWER=0x3c`、`DACCONTROL21=0x80`、LOUT1/ROUT1 音量 `0x1e` 正常；扬声器功放使能脚（扩展器 0，第 1 位）输出寄存器为 1、方向为输出。已修复：DAC 静音位（`DACCONTROL3` `0x19` bit 2）在诊断时为 1，`feedback.c` 每次写入前调用 `esp_codec_dev_set_out_mute(false)`，寄存器读回变为 `0x00`，但仍未听到声音。
  - 注意：扩展器输入寄存器读回不可靠（输出为 1 的引脚如 `BSP_TOUCH_EN` 也读回 0），不能用来判断功放使能状态。
  - **2026-11-08 代码检查**：发现 `usb_mode=2` 把 USB Serial/JTAG 路由到 PHY1，却未关闭其 pads；PHY1 的 GPIO26/27 正是 ES8388 的 I2S DOUT/BCLK。`usb_host_core_start()` 已改为先关闭 Serial/JTAG pads，再把 USB_WRAP 切到 PHY0，保留 USB-A + USB-C 双 host。已通过 OTA 验证，用户确认提示音恢复。首次本机构建还暴露了 MFi 私钥未带 public point 时解析缺少 RNG 的问题，补齐 RNG 后设备日志确认 iAP2 authentication succeeded。已将 D100H 所需的 interrupt OUT 支持纳入本地 `usb_host_hid` 组件；最终 OTA 后用户确认 iPod、键盘、旋钮及提示音同时工作。
- **USB Options**：`USB-A HighSpeed Only`（`usb_mode 0`，默认）和 `USB-A HS & USB-C OTG`（`usb_mode 2`）；启动生效，需重启，详见 [`usb-modes.md`](usb-modes.md)。
- **Boot Screen**：`OFF`（默认）和 `KCN4M1`；选中后保存到 `boot_screen`，下次启动生效。KCN4M1 是启动动画，设备上播放约 5 秒，模拟器需 `--boot` 才会播放。
- **Now Playing Background**：Theme / Blurred cover / Cover gradient，默认 Cover gradient，切换后返回 Now Playing 生效。
- **Clock**：
  - 第一行 `FORMAT 12H/24H`：Enter 或触摸切换，写入配置；
  - 下方为时间编辑器，字段依次为 YEAR、MONTH、DAY（日期行）和 HOUR、MINUTE、AM/PM（时间行）；六个字段宽度相同，两行列对齐；
  - 12H 时 HOUR 显示 1–12，AM/PM 字段可见；HOUR 在同一半天内循环，AM/PM 字段 Enter 或方向键切换；24H 时 HOUR 显示 0–23，AM/PM 字段隐藏，留出空位保持列对齐；
  - Enter 开始编辑当前字段，编辑中 Up/Down 调整数值，再次 Enter 确认；未编辑时 Up/Down 在字段间移动，Enter 只作用于当前字段；
  - 上方两个 64×56 触摸按钮：非编辑状态用于上下移动焦点，编辑状态用于调整当前字段；
  - 编辑框与菜单同宽，时间数字使用 36 px Inter tabular；选中字段以 250 ms 闪烁，非编辑时保持静态高亮；
  - 确认后写入系统时间和 RX8130CE RTC；模拟器仍使用内存 offset。
- 触摸两侧都可操作。进入标签页时光标总是回到左栏，并回到该页之前选中的行。
- Back 的顺序：右栏 → 左栏；已在左栏且不在第 1 行时 Back 回到第 1 行；已在第 1 行时 Back 返回 Now Playing。
- Albums 的 Back 不会停在 `Shuffle All`：从任意专辑 Back 会选中第 1 张专辑并把 `Shuffle All` 滚出视野；已在第 1 张或 `Shuffle All` 时 Back 直接返回 Now Playing。

---

## 6. Mock 后端设计

### 6.1 目标

让 UI 在没有 USB/iAP2 的情况下完整可用，并且接口与真实后端保持一致。Mock 不是“随便写死几个字符串”，而是模拟真实状态变化，方便调焦点、滚动、播放和队列。

### 6.2 数据规模

第一版 mock 由 Python 脚本生成，规模和内容如下：

- 24 位艺术家；
- **100 张专辑**；
- **555 首歌曲**（每张 3–8 首）；
- 100 张 256×256 彩色科幻抽象封面，全部无文字；
- 队列按所选专辑/艺术家生成，Now Playing 显示当前项 + 后续项。

曲目量已经接近真实小库。Songs 页面使用固定行高的虚拟列表：只保留 14 个复用行控件，滚动时更新索引和文字，不再为 555 首歌创建 555 个行对象。

### 6.3 状态模型

```c
typedef enum {
    PLAYBACK_STOPPED,
    PLAYBACK_PLAYING,
    PLAYBACK_PAUSED,
    PLAYBACK_SEEKING_FORWARD,
    PLAYBACK_SEEKING_BACKWARD,
} playback_state_t;

typedef struct {
    uint64_t id;
    char title[128];
    char artist[128];
    char album[128];
    char album_artist[128];
    uint32_t duration_ms;
    uint16_t track_no;
    uint16_t disc_no;
    uint16_t year;
} track_info_t;

typedef struct {
    playback_state_t status;
    track_info_t track;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    int volume;              /* 0-100 */
    bool muted;
    bool starred;
    bool shuffle;
    int repeat;              /* 0=off, 1=all, 2=one */
    bool connected;
    char device_name[64];
    int battery;             /* -1 = unknown */
    bool charging;
} player_snapshot_t;
```

队列：

```c
typedef struct {
    int index;               /* 当前曲目在队列中的位置 */
    int total;
    int count;               /* 本次快照包含的条目数 */
    track_info_t items[32];  /* 当前项 + 后续项 */
} queue_snapshot_t;
```

媒体库查询：

```c
int  player_album_count(void);
bool player_album_at(int index, album_info_t *out);
int  player_artist_count(void);
bool player_artist_at(int index, artist_info_t *out);
int  player_song_count(void);
bool player_song_at(int index, track_info_t *out);
int  player_album_tracks(uint64_t album_id, track_info_t *out, int max);
int  player_artist_tracks(uint64_t artist_id, track_info_t *out, int max);
```

真实后端接入后，这些函数改成从 SQLite/内存镜像分页读取；UI 层不关心数据来自 mock 还是 iAP2。

### 6.4 命令

```c
void player_play_pause(void);
void player_next(void);
void player_previous(void);
void player_stop(void);
void player_seek_ms(uint32_t elapsed_ms);
void player_set_volume(int volume);
void player_mute_toggle(void);
void player_toggle_star(void);
void player_set_shuffle(bool on);
void player_set_repeat(int mode);

void player_play_track(uint64_t track_id);
void player_play_album(uint64_t album_id);
void player_play_artist(uint64_t artist_id);
void player_queue_jump(int index);
void player_queue_add(uint64_t track_id);
void player_queue_clear(void);
```

Mock 收到命令后立刻更新状态；真实后端把命令投递到 iAP2 任务队列，不阻塞 UI。

### 6.5 状态刷新

- UI 以 500 ms 周期刷新时间、进度和 mini 条；
- menubar 的播放/暂停图标不参与轮询：后端 `bump_revision()` 时通过 `player_set_change_callback()` 立即通知 UI，只更新这一个图标；
- 后端维护 `revision`，其它标签/列表仍可按 revision 变化刷新；
- 播放进度每秒变化一次即可，不需要 60 Hz；
- 列表数据变化时重建当前列表，保持选中项稳定。

真实后端如果从其它任务触发回调，需要在回调里把更新投递到 UI 任务。

### 6.6 Mock 数据生成与导入

Mock 数据不再写在 C 表里，而是用 Python 生成 JSON + PNG，C 只负责读取：

```text
tools/gen_mock_library.py     # 生成器
assets/mock/
  library.json                # 艺术家 / 专辑 / 曲目
  covers/000.png ... 099.png  # 256×256 彩色科幻抽象封面，无文字
```

生成命令：

```bash
python3 tools/gen_mock_library.py
# 可选：--albums 100 --seed 20261104
```

- Python 使用固定 seed，所以每次生成结果可复现；改 seed 就得到另一套随机数据；
- 封面使用 PIL 生成彩色科幻构图（行星、轨道、霓虹网格、光束、碎片），不带任何文字；
- C 端用 cJSON 读取 `assets/mock/library.json`（仅模拟器）；
- 封面通过 LVGL LodePNG + stdio 文件系统（`A:`）加载；
- 模拟器编译时通过 `MOCK_ASSET_DIR` 指向 `assets/mock`；
- 真机固件不打包 `assets/mock`、不加载 mock 库、不执行开机演示；真机库来自
  `/sdcard/lib/<short8>/library.bin`，播放/封面来自 iAP2；
- 修改 mock 数据不需要重新编译 C，只要重新生成 JSON/封面。

---

## 7. 代码结构建议

在当前仓库上逐步演进：

```text
app/
  backend/
    player.h              # 统一后端接口（C）
    mock_player.c/.h      # cJSON 读取 assets/mock/library.json
    iap2_player.c/.h      # future: 接 ../iap2mediactrl
  input/
    input_event.h         # INPUT_PREV / NEXT / ACTIVATE / KEY_1..7
    input_router.c        # 逻辑按键 → 全局动作/当前页面
  ui/
    theme.c/.h            # 颜色、字号、间距
    ui_root.c/.h          # menubar、页面容器
    focus.c/.h            # 焦点列表、滚动、值控件
    widgets/
      list_row.c/.h
      queue_list.c/.h
      artwork.c/.h
      status_bar.c/.h
    screens/
      now_playing.c/.h
      albums.c/.h
      artists.c/.h
      songs.c/.h
      settings.c/.h
sim/
  main.c                  # SDL 窗口、主循环
  input_sdl.c             # PC 键盘 → 逻辑输入
  screenshot.c
```

当前 `app/ui/ui.c` 里的占位 Dashboard 会在第一阶段替换成 `ui_root` + 5 个页面；模拟器窗口和截图能力保留。

### 7.1 输入分发

```text
HID/键盘
  → input_event_t
  → input_router
      ├─ 全局键：1-5 切屏、6 返回、7 播放/暂停
      └─ 旋钮：交给当前 screen 的 focus handler
  → screen_focus_move() / screen_activate() / screen_back()
```

建议不要让 LVGL 默认 group 直接接管所有按键。原因：

- 数字键需要在任何焦点下都生效；
- Now Playing 有“传输按钮 → 随机/循环 → 队列”三个区域；
- 列表详情需要保存/恢复焦点；
- 值控件需要“旋钮调值而不是移动焦点”。

用一个小型自定义焦点引擎更可控；LVGL 只负责画控件和滚动。

### 7.2 焦点 API（建议）

```c
typedef struct focus_item {
    lv_obj_t *obj;
    void (*on_activate)(void *user);
    void (*on_adjust)(int delta, void *user);  /* NULL = 普通列表项 */
    void *user;
} focus_item_t;

void focus_set_items(const focus_item_t *items, int count, int selected);
void focus_move(int delta);
void focus_activate(void);
void focus_back(void);
void focus_restore(void);
```

每个页面在创建时注册自己的 focus items；页面切换时保存 selected index。

---

## 8. 视觉规范

### 8.1 颜色

开发阶段使用全灰阶，避免高饱和蓝色在车内夜间刺眼；后续车内主题再单独调。颜色集中到 `theme.c`：

| 用途 | 颜色 |
| --- | --- |
| 背景 | `#050505` |
| 面板 | `#141414` / `#111111` |
| 边框 | `#2A2A2A` |
| 主文字 | `#EDEDED` |
| 次要文字 | `#9A9A9A` |
| 强调色 / 焦点 | `#C8C8C8` |
| 选中背景 | `#303030` |
| 当前播放 | `#C8C8C8` |
| 错误/断连 | `#E6E6E6`（灰阶阶段） |

四个分区的灰阶初版：

| 区域 | 背景 | 前景 |
| --- | --- | --- |
| menubar 顶部时间区（72 px） | `#2A2A2A` | `#FFFFFF` |
| menubar 菜单区 | `#0A0A0A` | `#C8C8C8`（当前项 `#FFFFFF`） |
| menubar 电池区（72 px） | `#202020` | `#C8C8C8` |
| 右侧顶部迷你 Now Playing（72 px） | `#333333` | `#FFFFFF`（次级 `#D0D0D0` / `#A0A0A0`） |
| 右侧主内容区 | `#050505` | `#F0F0F0`（面板 `#141414`） |
| 双栏页面的右侧队列/详情面板 | `#1E1E1E` | `#F0F0F0`（与左栏无间隔相接） |

Mock 封面由 Python 生成彩色科幻抽象 PNG，不在封面上绘制文字；UI 主题仍保持灰阶（Now Playing 可选用封面模糊背景引入封面色）。最终车内主题可以在 `theme.c` 里单独调整。

**界面不使用圆角。** 面板、卡片、按钮、列表行、专辑封面、进度条、滑杆和滚动条全部为直角；焦点用左侧色条 + 背景高亮表达，不做胶囊形或圆角矩形。

### 8.2 字号（5 英寸 1280×720）

| 用途 | 建议字号 |
| --- | --- |
| menubar 图标 | 40 px |
| menubar 时间 | 28 px Inter tabular |
| menubar 电量百分比 | 16–18 px |
| 列表主文字 | 22–24 px |
| 列表次要文字 | 18–20 px |
| 页面标题 | 32–36 px |
| Now Playing 标题 | 40 px |
| Now Playing 艺术家 | 32 px |
| Now Playing 专辑 | 32 px（与艺术家相同） |
| Now Playing 上下文菜单项 | 24 px |
| 上下文菜单分组标题 | 20 px |
| 行高 | 56–72 px |
| 最小触摸目标 | 56 px |

当前模拟器里的 14 px 辅助文字在 5 英寸屏上偏小，正式页面会整体放大。

### 8.3 字体

- 常规文字/数字：Inter（14/16/18/20/22/24/28/32/36/40），由 `tools/gen_inter_fonts.sh` 从 `assets/fonts/Inter-Variable.ttf` 生成；Inter 使用 SIL OFL 1.1，许可证见 `assets/fonts/LICENSE.inter`；
- menubar 时间：Inter tabular 28 px（启用 `tnum`）；标签固定占满 120 px 时间区并居中；
- Now Playing 进度时间：Inter tabular 20 px（启用 `tnum`）；左右标签固定 72 px 宽，左/右对齐，避免走时导致进度条抖动；
- 中文：媒体库标题可能是中文，不能只做固定文案子集；
  - 方案 A：FreeType 运行时加载中文 TTF（放 Flash 或 SD 卡），带字形缓存；
  - 方案 B：预生成 GB2312/常用字子集，16–20 px；体积较大但实现简单；
  - 方案 C：第一版 UI 文案用英文，中文媒体元数据后续解决。
- 第一版 mock 可以先用英文数据，把中文方案留到真机阶段验证。这个需要在开工前确认。

### 8.4 图标：统一使用 Bootstrap Icons

不再混用 `LV_SYMBOL_*`。所有界面图标统一从 Bootstrap Icons 字体取字形。

当前已经落地的文件：

```text
assets/fonts/
  bootstrap-icons.ttf             # 原始字体，MIT
  LICENSE.bootstrap-icons
  bootstrap-icons-codepoints.txt  # 当前用到的图标和码点
app/ui/
  icons.h                         # ICON_* 宏 + LV_FONT_DECLARE
  fonts/bootstrap_icons_24.c      # 24 px LVGL 子集（普通图标）
  fonts/bootstrap_icons_32.c      # 32 px LVGL 子集（备用）
  fonts/bootstrap_icons_40.c      # 40 px LVGL 子集（menubar）
tools/
  gen_icons.sh                    # 重新生成 24/32/40 px 图标子集
```

当前子集包含：

- menubar 电池：`battery-full`、`battery-half`、`battery-low`、`battery-charging`、`battery`；
- 播放：`play-fill`、`pause-fill`、`skip-start-fill`、`skip-end-fill`、`stop-fill`；
- 模式：`shuffle`、`repeat`、`repeat-1`、`star`、`star-fill`；
- 浏览：`music-note-list`、`disc`、`person`、`gear`、`list`、`house`、`arrow-left`、`chevron-up`、`chevron-down`；
- 其它：`check-lg`、`x-lg`、`wifi`、`wifi-off`、`eye`、`display`、`collection-play-fill`、`brightness-high`、`clock`、`music-note-beamed`、`circle-fill`。

使用约定：

- 普通图标使用 `bootstrap_icons_24`，上下文菜单使用 `bootstrap_icons_32`，menubar 使用 `bootstrap_icons_40`，Now Playing 传输按钮使用 `bootstrap_icons_48`；
- 文本 label 继续使用 Inter；
- 不要在同一个 label 里混排图标和文字，图标和文字分别建 label；
- 图标码点通过 `ICON_PHONE`、`ICON_PLAY` 这类宏引用，不直接写 `\xEF...`；
- 需要新图标时：
  1. 从 Bootstrap Icons CSS 查码点；
  2. 加入 `tools/gen_icons.sh` 的 `RANGES`；
  3. 在 `app/ui/icons.h` 增加宏；
  4. 执行 `./tools/gen_icons.sh`。
- 24 / 32 / 40 / 48 px 都从同一份 TTF 生成；如果还需要其它尺寸，在 `gen_icons.sh` 里增加字号，不要缩放位图字形。

menubar 电量映射：

| 状态 | 图标 | 颜色 |
| --- | --- | --- |
| 充电中 | `battery-charging` | 浅灰 |
| 76%–100% | `battery-full` | 浅灰 |
| 41%–75% | `battery-half` | 浅灰 |
| 16%–40% | `battery-low` | 浅灰 |
| 0%–15% | `battery` | 浅灰 |
| 未知 | `battery` + `--%` | 深灰 |

充电状态优先于电量：只要设备返回“充电中”，无论剩余多少都显示 `battery-charging`。Bootstrap Icons 1.13.2 才加入 `battery-low`，项目用的是 1.13.2 的字体。

时间更新：

- 用 `time()` + `localtime_r()` 取本地时间，`strftime("%H:%M")`；
- 每秒检查一次即可，只在分钟变化时刷新 label；
- 真机使用 Tab5 的 RX8130CE RTC；上电时读取 RTC，掉电/时间不可信时用构建时间兜底，之后可用网页 “Sync browser time” 或 Settings → Clock 校正。


---

## 9. 状态与错误处理

### 9.1 应用状态

```text
BOOT
  → NO_DEVICE（显示“等待 iPod/iPhone”与 Mock 入口）
  → CONNECTING
  → READY
  → ERROR / DISCONNECTED
```

- UI 在 `NO_DEVICE` 时仍然可以进入 Mock 库，方便演示；
- `DISCONNECTED` 时保留最后一份 Now Playing 快照，灰显控制按钮；
- 错误信息不弹模态框，放在 Settings/Status bar 或顶部小提示，避免打断旋钮操作。

### 9.2 连接丢失

参考 headunit：覆盖层显示“Connection lost”、重试次数、最后错误；按 Back 可关掉覆盖层继续浏览缓存库。

### 9.3 时间与进度

- 进度条每秒更新；
- seek 只在后端声明支持时启用；
- 时长未知显示 `--:--`；
- 队列位置未知时隐藏 “02/07”。

---

## 10. 实施计划

### M0：对齐（本次）

- 确认 7 个按键的最终映射；
- 确认界面深度：Albums/Artists 是否要右侧详情；
- 确认中文/英文和字体方案；
- 确认输入设备与 Tab5 的物理连接方式。

### M1：UI 骨架 + Mock

- 重构 `app/` 目录；
- 实现 `theme`、`ui_root`、焦点引擎；
- 接入 mock 后端；
- 做出 5 个页面的静态布局；
- 模拟器键盘映射：Up/Down/Enter/1–7。

### M2：交互闭环

- Now Playing 播放/暂停、上一首/下一首、进度；
- Queue 选择与跳转；
- Albums/Artists/Songs 列表滚动、选中、播放；
- Page Down 切换 menubar；Now Playing 的 Page Up 切换 Up Next；Settings 触摸开关、menubar 左右位置、亮度、超时；
- Settings 的 Touch input 行随触摸开关显示 On/Off。

### M3：视觉与可用性

- 5 英寸可读性调整；
- 焦点/正在播放/选中三种状态区分；
- 长列表性能（虚拟列表/分页）；
- 截图回归和键盘冒烟测试。

### M4：真实后端适配

- 把 `player.h` 接口接到 `../iap2mediactrl` 的数据模型；
- 先做只读快照和命令队列；
- 再做 USB HID/iAP2 的实际连接；
- Mock 保留为无设备时的演示模式。

### M5：Tab5 真机

- ESP-IDF + esp-bsp `m5stack_tab5` + esp_lvgl_port；
- USB HID 输入设备；
- iAP2 USB Host；
- PSRAM 绘图缓冲、封面缓存；
- 中文字体、NVS 设置持久化。

---

## 11. 验收标准（第一版）

- [ ] 只按 Up/Down/Enter/1–7，可以完成：切屏、列表滚动、打开专辑、播放歌曲、跳转队列、开关触摸；
- [ ] 关闭触摸后，触摸完全无响应，旋钮和按键仍正常；
- [ ] menubar 顶部时间正确显示并每分钟刷新；
- [ ] menubar 底部电量图标随电量和充电状态变化；
- [ ] menubar 图标顺序正确，Now Playing 图标随播放状态切换；
- [ ] Settings 可以把 menubar 在左/右之间切换，布局立即生效；
- [ ] 在 Now Playing 按 Page Up 可以显示/隐藏 Up Next 面板；
- [ ] Now Playing 的传输/随机/循环按钮不进入焦点链，触摸点击仍可用；
- [ ] Now Playing 走时不会导致进度条左右抖动；
- [ ] star 按钮可以切换当前曲目的收藏状态；
- [ ] 非 Now Playing 页面顶部显示 72 px 迷你 Now Playing 条，Now Playing 页面隐藏；
- [ ] 迷你条显示小封面、两行文字（歌名 / artist + album）、队列位置/大小、播放时间/曲长和 shuffle/repeat/star 状态；
- [ ] 所有图标来自 Bootstrap Icons 子集，不混用 `LV_SYMBOL_*`；
- [ ] 5 个页面在 1280×720 下无溢出、无文字截断；
- [ ] 焦点始终可见，列表自动滚动到焦点；
- [ ] 100 张专辑、555 首歌曲的 mock 数据从 JSON 正确加载；
- [ ] 所有专辑封面均显示，封面无文字；
- [ ] 长列表（1000 行以上）滚动不卡顿；
- [ ] 当前播放项、焦点项、普通项三种状态可区分；
- [ ] 模拟器可一键截图，可用于每次 UI 变更对比；
- [ ] Mock 与真实后端共用同一套 UI 接口，切换后端不改页面代码。

---

## 12. 待确认问题

1. **7 个按键的最终功能**：是否就是 1–5 切屏、6 返回、7 播放/暂停？硬件上有没有丝印？
2. **旋钮是否有段落感/每圈脉冲数**：决定列表滚动速度；是否需要加速度？
3. **输入设备怎么接 Tab5**：USB HID 键盘是插在 Tab5 的 USB-A 上吗？iPod 也走 USB 时是否需要 Hub 或双 USB？
4. **Albums / Artists 是否要右侧详情**：第一版做两栏，还是列表 Enter 直接播放？
5. **Queue 是否可编辑**：只显示设备队列，还是也要做 accessory 侧“加入队列”？
6. **中文媒体库**：歌曲名/艺术家可能是中文，字体方案选 A/B/C？
7. **长按 Enter 是否可用**：固件能否区分短按/长按？
8. **Settings 第一版要哪些项**：触摸、亮度、超时、后端状态，是否还要 shuffle/repeat？
9. **Now Playing 是否要独立 Queue 页面**：还是像现在设计一样常驻右侧？
10. **主题色**：开发阶段已改为全灰阶；最终车内主题是否恢复低饱和强调色，还是保持灰阶？
11. **menubar 时间制式**：默认 24 小时还是 12 小时？RTC 掉电时用构建时间兜底还是显示 `--:--`？
12. **设备状态**：phone 图标已去掉；未连接状态只在 Settings 的 Device 行显示？
13. **电池位置**：当前放在 menubar 底部；是否还需要百分比文字，还是只留图标？
14. **电量阈值/颜色**：按 15/40/75 分档是否合适？
15. **音量控制**：Now Playing 已去掉滑动条；后续用硬件按键、设置项，还是不做 UI？
16. **menubar 位置持久化**：当前只做运行时切换，真机需要写入 NVS；默认左侧还是记住上次选择？
17. **已知问题：列表底部半行**：Albums / Artists 以及右侧专辑列表当前会被底部边框裁出半行；等底部边框和内容高度样式调整后统一修复。

---

## 13. 参考

- `../iap2mediactrl/README.md`、`web_app.py`、`docs/PORTING_CPP.md`
- `rGnng/headunit`：`AGENT.md`、`ui/pages.py`、`ui/widgets.py`、`ui/theme.py`
- 本地 `../headunit`（QML 版本）与 `../headunit-lvgl`（LVGL 8 草稿）
- [Bootstrap Icons](https://icons.getbootstrap.com/)（MIT）
- 当前模拟器：`sim/main.c`、`app/ui/ui.c`、`app/ui/icons.h`、`README.md`

### 5.x 长文本显示

- 列表和详情里的文字一律不换行，超出部分不显示 `...`；
- 超出右边缘时，最右 16 px 渐变淡出到所在行的背景色；
- 选中项（Albums、Artists、Playlists、Songs 的选中行，以及详情页标题）停止操作约 1.2 s 后，超长文字从右向左匀速滚动（约 36 px/s），显示完最后一个字符后停约 1 s，再回到起始位置；选中项变化时重新计时；
- Songs 列表只有歌名滚动和渐变，歌手、专辑等其它列超长直接切断；
- Songs 列表字号与其它列表保持一致。
