# IAP2 · M5Stack Tab5 UI

**开发板还没到，也可以先做界面。** 这个仓库是一个面向 M5Stack Tab5 的 LVGL 9.6 桌面模拟器，以及一版可运行的 1280×720 UI mock。

[![UI preview](docs/screenshots/overview.png)](docs/screenshots/overview.png)

模拟器直接在 PC 上跑 SDL2 窗口，鼠标就是触摸屏。UI 代码放在 `app/ui/`，不包含任何桌面专用接口，之后可以原样放进 ESP-IDF 工程。

键盘：`Up/Down` 移动焦点，`Enter` 确认（长按 650 ms 打开上下文菜单，也可以按 `C`），`7` 播放/暂停，`Backspace` 或 `6` 返回，`Page Up/Page Down` 在 menubar 中上下切换；数字键 `1–5` 直接切页。在 Now Playing 按 `Page Up` 显示/隐藏 Up Next 面板。

## 项目方向

目标是做一个 iPod / iPhone 音乐控制端：旋钮 + 7 个按键为主输入，触摸仅作辅助。功能范围参考 `../iap2mediactrl`，交互和视觉参考 `rGnng/headunit`。

完整的需求、按键映射、焦点模型、页面布局和实施计划见 **[docs/design.md](docs/design.md)**。

当前第一版 mock 已实现：

- 5 个页面：Now Playing + Queue、Albums、Artists、Songs、Settings
- 左侧/右侧竖排图标 menubar，图标 72×72 按钮 + 40 px 图标，Settings 里可切换；Now Playing 图标随播放状态变 play/pause
- 旋钮 + 7 键的焦点导航，带 mock 播放器和媒体库
- menubar 顶部 72 px 时间区：24H 用 28 px、12H 用 24 px Inter tabular 时间；底部 72 px 电池区：iOS 设备电量/充电状态图标
- 非 Now Playing 页面顶部有 72 px 迷你 Now Playing 条：小封面、两行文字（歌名 / 艺术家 + 专辑）、队列位置/大小、播放时间/曲长、shuffle/repeat/star 状态
- Now Playing 使用 360 px 封面；标题 40 px，艺术家/专辑 32 px 合并为一行（`Artist - Album`），进度时间 20 px tabular；信息列宽 582 px（封面宽度的 1.618 倍）；标题与元信息作为一组下移到信息列的黄金分割位置，组内间距 12 px，外层主间距和上下留白也按黄金比例分配；传输按钮使用 48 px 图标；进度条走时不抖动；shuffle/repeat/star 在进度条下方居中
- Queue 展开时左栏切换为上下布局并整体降一档字号：封面居中；标题行、`Artist - Album`、进度条、控制行统一为 582 px 宽并居中，比封面宽但左右留出边距；标题 32 px，star 在标题行最右侧，控制行左侧 shuffle、中间三枚传输按钮、右侧 repeat
- Now Playing 背景支持 Theme / Blurred cover / Cover gradient（默认）：Blurred cover 用当前封面填充并高斯模糊；Cover gradient 从封面提取 5 个主色并生成静态 3×3 网格渐变，再叠 60% 黑色遮罩保证文字可读
- Now Playing 长按 Enter 打开分组上下文菜单：GoTo（Go to album / Go to artist）、Star（Star / Unstar，星标随状态切换）、Playback（Shuffle / Repeat，显示当前状态）；菜单是居中弹层，带遮罩、分组标题和独立焦点，不破坏底层页面焦点
- Albums 两栏：右侧始终显示左侧选中专辑；选中行保留底色，浅色条只提示旋钮焦点，触摸两侧都可操作
- Artists 两栏：右侧按专辑分组列出该艺术家的全部专辑；48 px 专辑 header 可直接播放整张专辑
- Settings 两栏：左侧 General / Theme / Now Playing Background / Device / USB-C / Backend / About 七组；General 内含 Time format（12H/24H）、Set time、Menubar、Touch input、Brightness；Now Playing Background 可选 Theme / Blurred cover / Cover gradient
- Songs 行高 56 px，列表固定 10 行（560 px），滚动区域为行高整数倍
- Bootstrap Icons 图标子集，生成脚本是 `tools/gen_icons.sh`
- Inter 字体子集（SF Pro 风格），生成脚本是 `tools/gen_inter_fonts.sh`；时间使用 tabular 数字
- Bootup sequence 默认使用 Inter tabular（与主界面数字一致）；VT323 像素字体保留为可选方案，生成脚本是 `tools/gen_boot_font.sh`
- 开发阶段 UI 控件全灰阶、全直角、无底部提示栏；各区使用不同深浅的灰阶（menubar 时间区 / menubar 主体 / 电池区 / 迷你 Now Playing / 主内容区），Now Playing 可选用封面模糊背景
- Songs 页面使用 14 行复用池的虚拟列表，555 首歌曲不再一次性创建 555 行
- 启动画面：3 段式 Bootup sequence（POST 自检 → Initialize App → DISPLAY CHECKING / FPS），黑底 Inter 字体、黄色标题、绿色 `OK`、白色 `---` 和 `CHECKING....` 四点动画；默认启动时播放，任意键跳过

更多截图：[Boot sequence](docs/screenshots/boot-sequence.png) · [Albums](docs/screenshots/albums.png) · [Album detail](docs/screenshots/album-detail.png) · [Artists](docs/screenshots/artists.png) · [Artist detail](docs/screenshots/artist-detail.png) · [Songs](docs/screenshots/songs.png) · [Settings](docs/screenshots/settings.png) · [Settings time](docs/screenshots/settings-time.png) · [Now Playing Background](docs/screenshots/settings-now-playing-background.png) · [Now Playing + queue](docs/screenshots/now-playing-queue.png) · [Now Playing menu](docs/screenshots/now-playing-menu.png) · [Menubar right](docs/screenshots/menubar-right.png)


## 快速开始

Debian / Ubuntu：

```bash
sudo apt install build-essential cmake ninja-build libsdl2-dev
```

构建并运行：

```bash
cmake -S . -B build -G Ninja
cmake --build build -j
./build/tab5_sim
```

第一次配置会下载 LVGL v9.6.0 到 `build/_deps`。如果 GitHub 访问慢，可以先用任意方式准备一份 LVGL 源码，然后指定本地目录：

```bash
cmake -S . -B build -G Ninja -DLVGL_SOURCE_DIR=/path/to/lvgl
```

交互方式：

- 鼠标 = 电容触摸
- `--zoom 0.75`：窗口缩放，适合小屏幕笔记本
- 关闭窗口退出

### 启动画面

启动时默认先播放 Bootup sequence，约 5.8 秒后自动进入主界面；任意按键跳过。

- 第一屏：POST 自检（USB / SD / TOUCH / DISPLAY / PSRAM / FLASH / USB HOST / iAP2 证书 / Wi-Fi C6 / NETWORK），停留 3500ms；底部 `ELAPSED TIME` 按 60fps 显示帧数（3500ms 对应 210 帧）
- 第二屏：`Initialize App`
- 第三屏：`DISPLAY CHECKING` 与 `FPS = 59.9990~60.0000`（模拟器里末四位快速变化）

调试时可以冻结在任意时间点：

```bash
./build/tab5_sim                                      # 完整启动画面
./build/tab5_sim --no-boot                            # 直接进主界面
./build/tab5_sim --boot-freeze 1200 --screenshot build/screenshots/boot-post.png --wait 300
./build/tab5_sim --boot-freeze 3900 --screenshot build/screenshots/boot-app.png --wait 300
./build/tab5_sim --boot-freeze 5000 --screenshot build/screenshots/boot-display.png --wait 300
```

`--boot-freeze <ms>` 只用于截图/调试：画面停在该时间点，不再自动结束。

### 无显示器 / SSH 环境

没有 X11 或 Wayland 时，模拟器会自动切到 SDL 的 offscreen 驱动，可以直接输出预览图：

```bash
mkdir -p build/screenshots
./build/tab5_sim --no-boot --screenshot build/screenshots/ui.png --wait 1500
```

也可以显式加 `--headless`。`--wait` 用来让启动动画和图表定时器先跑一会儿。

## Mock 数据

> 仅用于桌面模拟器。真机固件不打包 `assets/mock`，不加载 mock 库，也不执行开机演示播放；
> 真机的媒体库来自 SD 卡上的 `library.bin`，播放状态和 Now Playing 封面来自 iAP2。

Mock 库由 Python 生成，C 端只读 JSON：

```text
assets/mock/
  library.json                 # 24 artists / 100 albums / 555 tracks
  covers/000.png ... 099.png   # 256×256 彩色科幻抽象封面，无文字
tools/gen_mock_library.py      # 生成器
```

重新生成：

```bash
python3 tools/gen_mock_library.py
# 可选：python3 tools/gen_mock_library.py --albums 100 --seed 20261104
```

封面不带文字，使用彩色科幻构图（行星、轨道、霓虹网格、光束、碎片）；`library.json` 由 cJSON 在启动时加载。修改 mock 数据不需要重新编译 C。

## 目录结构

```text
.
├── app/
│   ├── backend/
│   │   ├── player.h          # 后端接口
│   │   └── mock_player.c     # 共享后端：模拟器读 mock，真机读 SD + iAP2
│   └── ui/
│       ├── boot.c            # Bootup sequence（POST / Initialize App / FPS）
│       ├── boot.h
│       ├── ui.c              # 页面、焦点、状态栏、menubar
│       ├── ui.h
│       ├── icons.h
│       └── fonts/            # Bootstrap Icons + Inter（含 tabular）+ VT323 备用
├── assets/
│   ├── fonts/                # Bootstrap Icons / Inter-Variable.ttf / VT323
│   └── mock/                 # library.json + 100 张封面 PNG
├── sim/
│   ├── main.c                # SDL2 显示 + 键盘映射
│   └── screenshot.c          # 离屏 PNG 截图
├── tools/
│   ├── gen_boot_font.sh
│   ├── gen_icons.sh
│   ├── gen_inter_fonts.sh
│   └── gen_mock_library.py
├── lv_conf.h
├── third_party/              # stb, cJSON
└── CMakeLists.txt
```

`app/ui/` 只依赖 LVGL 和 `player.h`。模拟器专属的 SDL、截图、命令行都在 `sim/`，所以移植时不需要改页面代码。

## 和真实 Tab5 的差异

模拟器对的是 Tab5 的屏幕规格，但不模拟硬件性能：

| 项目 | Tab5 | 模拟器 |
| --- | --- | --- |
| 分辨率 | 1280×720，5" IPS，MIPI-DSI | 1280×720 SDL 窗口 |
| 触摸 | GT911，多点电容触摸 | 鼠标单点 |
| 主控 | ESP32-P4，双核 RISC-V 400 MHz | 桌面 CPU |
| 内存 | 16 MB Flash / 32 MB PSRAM | 桌面内存 |
| Wi-Fi | ESP32-C6，Wi-Fi 6 | 无（界面里是假数据） |
| 渲染 | RGB565 / 双全屏 PSRAM 缓冲 / PPA | LVGL 软件渲染 |

所以模拟器里流畅不代表板子上也流畅。1280×720 全屏 RGB565 一帧约 1.8 MB，真机上要注意 PSRAM 带宽、双缓冲大小和刷新区域。

当前界面里的数据（CPU、内存、Wi-Fi、吞吐）都是占位内容；确定实际产品界面后直接替换 `app/ui/ui.c` 即可。

## 之后怎么放到 Tab5 上

仓库里的 `firmware/` 是一版可烧录的 **Tab5 真机预览固件**：ESP-IDF 5.4.2 + esp-bsp `m5stack_tab5` + esp_lvgl_port + 共享的 `app/ui`。真机媒体库来自 SD 卡上的 `library.bin`，播放状态和封面来自 iAP2；`assets/mock` 只用于桌面模拟器。

```bash
export PATH="/home/ops/.local/share/uv/python/cpython-3.12-linux-aarch64-gnu/bin:$PATH"
. /home/ops/esp/esp-idf/export.sh
cd firmware
python3 ../tools/gen_iap2_identity.py   # MFi 私钥不提交，需本地生成
idf.py build
idf.py -p /dev/ttyACM0 flash
```

细节和真机相关的坑（P4 v1.x 修订、PSRAM BSS、LVGL 任务栈、USB 组件 override）见 [`firmware/README.md`](firmware/README.md)。

下面的路线是后续接入真实 iAP2 / 量产工程时的参考。

1. 安装 ESP-IDF v5.4 或更新版本。
2. 新建 ESP-IDF 工程，添加组件：
   - `espressif/esp-bsp`（选择 `m5stack_tab5`）
   - `espressif/esp_lvgl_port`
3. 在 `app_main()` 里启动 BSP 显示，然后调用 `ui_init()`：

```c
#include "bsp/esp-bsp.h"
#include "ui.h"

void app_main(void)
{
    lv_display_t *disp = bsp_display_start();
    (void)disp;

    bsp_display_lock(0);
    ui_init();
    bsp_display_unlock();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

4. 把 `app/ui/ui.c` 加进工程的源文件列表。`lv_conf.h` 用 ESP-IDF 工程自己的那份，不要直接复用模拟器这份。
5. 在 `menuconfig` 里打开 PSRAM，并把 LVGL 的绘制缓冲区放到 PSRAM。

官方参考：

- [M5Stack Tab5 官方 UserDemo](https://github.com/m5stack/M5Tab5-UserDemo)（自带 Desktop Build，也是一个可参考的模拟器）
- [esp-bsp / m5stack_tab5](https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_tab5)
- [LVGL PC simulator](https://docs.lvgl.io/master/details/integration/simulator/index.html)

## 中文界面与字体

LVGL 自带字体只覆盖 ASCII 和图标，中文需要单独准备。Tab5 只有 16 MB Flash，**不要把完整中文字体整个塞进去**。

常用做法是按界面实际用到的字符做子集：

```bash
npx lv_font_conv \
  --font NotoSansSC-Regular.otf \
  --size 20 --bpp 4 --format lvgl \
  --symbols "系统概览温度湿度设置返回确定取消" \
  --range 0x20-0x7F \
  -o app/ui/fonts/ui_zh_20.c
```

然后把生成的字体声明进 UI：

```c
LV_FONT_DECLARE(ui_zh_20);
lv_obj_set_style_text_font(label, &ui_zh_20, 0);
```

如果只是临时验证，也可以打开 `lv_conf.h` 里的 `LV_FONT_SOURCE_HAN_SANS_SC_16_CJK`，但它是 16 px 固定字号，而且体积约 1 MB 级，只适合做原型。

桌面模拟器还支持 FreeType，可以直接加载系统 TTF；真实固件更推荐上面的子集方案，内存和 Flash 都更可控。

## 下一步

现在这版是 **第一版 UI mock**，后端仍是 mock 数据。接下来继续做：

- 接入 `../iap2mediactrl` 的真实 iAP2 数据模型和命令队列
- 中文媒体库字体/字形缓存
- 把上下文菜单扩展到 Albums / Artists / Songs，增加更多队列操作
- Genres / Recently Added / Playlists 页面
- Tab5 真机：ESP-IDF、USB HID 输入、USB Host、PSRAM 绘图缓冲、menubar 位置持久化到 NVS

下一步先从真实后端接口和把上下文菜单扩展到其它页面开始，再按实际硬件按键丝印调整映射。
