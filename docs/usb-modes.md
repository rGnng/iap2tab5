# USB 角色切换：USB-A HS / Both A+C

本文说明 Tab5 的 USB host 角色如何配置、什么时候生效、哪些东西必须在编译期决定，以及之后把 USB 切换加到 Settings UI 时需要注意的代码位置。

## 结论

- **USB-A HS / Both A+C 是运行时配置，开机生效，不需要重新编译。** Settings → USB Options 中对应的两个选项为 `USB-A HighSpeed Only`（`usb_mode 0`）和 `USB-A HS & USB-C OTG`（`usb_mode 2`）。
- **不能在运行中热切换。** USB host 栈和 PHY 只在启动时初始化一次；修改 `usb_mode` 后必须重启。
- 编译期只需要决定：
  - dual-host 是否允许（`ENABLE_USB_DUAL_HOST`，当前为 `1`）
  - USB Host 栈 / Hub 支持的 Kconfig
  - USB-C 的 device/console 方案（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`）
  - 如果要通用 USB device（TinyUSB HID/MSC 等），device 栈代码必须编译进固件
- 板级 USB-C 是 sink（CC 5.1k 下拉、VBUS 输入）；USB-C host 需要 powered hub/dock 向上游回供 5V。
- **不要一边把 USB-C 直连 PC，一边使用 `usb_mode=2`。** PC 和 Tab5 会同时作为 host/VBUS 源，容易出现启动异常；接 PC 调试串口时用 `usb_mode=0`（USB-C device console）。

## 配置项

`/sdcard/config.json`：

```json
{
  "usb_mode": 2
}
```

| 值 | 启动后的 USB 角色 |
| --- | --- |
| `0` | USB-A HS host；USB-C 作为 device / USB Serial-JTAG console（默认） |
| `2` | Both A+C：USB-A HS + USB-C FS 同时 host；Serial/JTAG pads 关闭，避免占用 I2S |

补充：

- `usb_mode` 由 `app_config_init()` 在启动早期从 SD 卡读取，然后 `app_main()` 在 `usb_host_install()` 之前应用。
- SD 卡缺失或 `config.json` 不存在时默认 `0`。
- 目前 `usb_mode` **只存在 SD 卡**，没有像 `iap2_mode` 那样存 NVS。如果希望 SD 卡缺失时仍保留上次选择，可以后续把 `usb_mode` 也写入 NVS。
- 修改配置后需要重启；Web 的 `POST /api/usbmode?mode=0|2` 已经做了“保存 + 重启”。
- 旧的 `usb_mode=1`（仅 USB-C FS）已移除；读到 `1` 时按 `0`（USB-A HS）处理。

## 启动流程与代码位置

```text
app_config_init()                        读取 /sdcard/config.json 的 usb_mode
        ↓
app_main()
        ↓
usb_peripheral_map_for_mode(usb_mode)    0→BIT0, 2→BIT0|BIT1
usb_host_core_set_peripheral_map(map)
        ↓
usb_host_core_start()
  ├─ BIT0: bsp_feature_enable(BSP_FEATURE_USB, true)   打开 USB-A VBUS
  ├─ BIT1: usb_wrap_ll_phy_select(&USB_WRAP, 0)        USB-C 路由到 FSLS PHY0
  └─ usb_host_install(peripheral_map=map)              只初始化一次
```

关键文件：

| 文件 | 作用 |
| --- | --- |
| `firmware/main/app_config.[ch]` | `usb_mode`、`charge_enabled` 的读写、默认值、SD 配置 |
| `firmware/main/main.c` | `usb_peripheral_map_for_mode()`、启动时应用、充电使能 |
| `firmware/main/usb_host_core.c` | `usb_host_install()`、PHY0 路由、USB-A VBUS |
| `firmware/main/charge.[ch]` | IP2326 `CHG_EN`（E2.P7）控制 |
| `firmware/main/web_server.c` | `/api/usbmode`、`/api/charge`、`/api/status`、`/api/config` |
| `app/ui/ui.c` | `g_usb_mode`、`ui_system_config_t.usb_mode`、Settings UI |

## 硬件与控制器映射

| `peripheral_map` bit | P4 控制器 | 板级端口 | PHY |
| --- | --- | --- | --- |
| `BIT0` (`1<<0`) | USB 2.0 OTG HS | USB-A (J10) | UTMI |
| `BIT1` (`1<<1`) | USB 1.1 OTG FS | USB-C (J8) | FSLS PHY0 (GPIO24/25) |
| `BIT0\|BIT1` | 两个 root port | 同时 | UTMI + FSLS PHY0 |

- USB-C 数据线在 Tab5 上接 **FSLS PHY0**；ESP-IDF 默认把 `USB_WRAP` 映射到 PHY1，所以固件必须在 `usb_host_install()` 前调用：

  ```c
  usb_serial_jtag_ll_phy_enable_pad(false);  // GPIO26/27 留给 I2S
  usb_wrap_ll_phy_select(&USB_WRAP, 0);
  ```

- USB-C host 模式下，USB Serial/JTAG 会被路由到 PHY1（GPIO26/27）。这两个脚实际连接 ES8388 的 I2S DOUT/BCLK，所以必须在切换 PHY 前调用 `usb_serial_jtag_ll_phy_enable_pad(false)` 释放引脚，否则 codec 写入成功也没有声音。USB_WRAP 的 PHY0 pads 保持启用；远程调试使用 WiFi 的 `/debug`、`/api/log`、`/api/log/stream`。
- OTA / USB mode 切换后的重启使用 `esp_rom_software_reset_system()`，不再调用 `esp_restart()`：USB host 栈处于活动状态时 `esp_restart()` 可能卡住并触发 WDT，表现为“重启时死机几秒后自动重启”。
- USB-C 的 CC1/CC2 是 5.1k 下拉，VBUS 是 `USB_5VIN` 输入；它不是能对外供电的 host 口。需要通过 powered hub/dock 向上游供 5V，Tab5 才能同时作为 USB-C host 工作。
- USB-A 的 VBUS 由 BSP `BSP_FEATURE_USB` 开关控制；双 host 时 USB-A 和 USB-C 下游设备的电流都来自系统 5V/电池，供电余量要留够。

### Hub / TT 限制

ESP-IDF 5.5.6 的 external hub driver **仍不支持 Transaction Translator (TT)**：

- USB-A HS + HS hub + FS/LS 设备：不行
- USB-A HS 直连 FS/LS 设备：可以（HS 端口直接降速）
- USB-C FS + FS hub + FS 设备：可以（整条总线 FS，不需要 TT）
- 相关代码：`espressif__usb/src/ext_hub.c` 的 `IDF-10059` TODO、`hub.c` 的 `TT is not supported`

## “OTG” 的两种含义

- **作为 host**：本文的 `usb_mode=2`，使用 P4 的 OTG 控制器 + USB Host 栈。
- **作为 device**：当前固件的 USB-C device 功能是 USB Serial/JTAG console，由编译期 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 决定。如果要通用 USB device（TinyUSB HID/MSC 等）：
  - device 栈代码必须在编译期加入
  - 可以在启动时选择 host / device，但同一个 FSLS 控制器不能同时跑 host 和 device
  - 不能和 `usb_mode=2` 的 USB-C host 同时使用

## Web / 串口接口

| 接口 | 说明 |
| --- | --- |
| `POST /api/usbmode?mode=0\|2` | 保存 `usb_mode` 并重启；其它值返回错误 |
| `GET /api/status` | 包含 `usb_mode`、`reset_reason`、`charge_enabled` |
| `GET /api/config` | 包含 `usb_mode`、`charge_enabled` |
| `POST /api/config` | 可写 `usb_mode` / `charge_enabled`，但 `usb_mode` **不会重启**；下次启动才生效 |
| `GET /api/power` | `bus_mv`、`current_ma`、`usb5v_en`、`chg_en`、`chg_stat` |
| `POST /api/charge?en=0\|1` | 立即设置/清除 `CHG_EN` 并保存 `charge_enabled` |
| 串口 `(` / `)` | 选择 USB-A HighSpeed Only / USB-A HS & USB-C OTG 并重启 |
| `/debug` 页面 | USB-A HS / Both (A+C) 按钮 + 重启 + OTA |

## 充电控制

`/sdcard/config.json` 里新增：

```json
{
  "charge_enabled": true,
  "qc_high": false
}
```

- 开机时 `app_main()` 调用 `charge_set_enabled(g_app_config.charge_enabled)`，把 IO expander `PI4IOE5V6408 #2` 的 `E2.P7`（IP2326 `CHG_EN`）拉高/拉低
- `qc_high` 控制 `E2.P5`（`nCHG_QC_EN`）的原始电平，默认 `false`；这是调试/兼容项，会影响 IP2326 的 QC/NTC 相关电路
- 调用位置在显示/背光初始化之后、USB host 启动之前
- Web 根页面的 **Power / charging** 卡片可以实时查看并切换
- `POST /api/charge?en=0|1&qc=0|1` 立即生效并保存到 `config.json`
- `GET /api/power` 返回：
  - `bus_mv` / `current_ma`：INA226 电池总线电压/电流
  - `shunt_raw` / `ina226_config`：INA226 原始分流电压和配置寄存器，用于区分“真的没电流”和“测量没配置”
  - `usb5v_en`：USB-A 5V 开关
  - `chg_en`：当前 `CHG_EN` 输出
  - `nchg_qc_en`：当前 `E2.P5` 原始电平
  - `chg_stat`：`E2.P6` / IP2326 `BAT_STAT` 原始电平。按数据手册，涓流充电时输出低电平，进入恒流/恒压后输出高电平；它不是简单的“充电/未充电”标志
- 充电需要 USB-C 口有 5V 输入；`usb_mode=2` 时 USB-C 是 host，需要 powered hub/dock 向上游供 5V
- 不装电池时切换这个选项没有实际影响

## 把 USB 切换加到 Settings UI

当前 Settings → USB-C 页面是只读展示（`app/ui/ui.c` 的 `case SETTING_USB`）。要改成可切换，建议按下面做。

### 已有数据结构

- `app/ui/ui.h` 的 `ui_system_config_t.usb_mode`：`0` USB-A HS，`2` Both A+C
- `app/ui/ui.c` 的 `usb_mode_t`：`USB_MODE_HOST` / `USB_MODE_BOTH`
- `ui_apply_saved_settings()` 已把 `cfg->usb_mode` 映射到 `g_usb_mode`
- `settings_system_changed()` 已把 `g_usb_mode` 写回 `g_system_config.usb_mode`
- `main.c` 的 `ui_system_config_changed()` 已把 `cfg->usb_mode` 写入 `g_app_config.usb_mode`

### 当前 UI 交互

`SETTING_USB` 现在直接显示两个带勾选的行：

- `USB-A HS` → `USB_MODE_HOST` / `usb_mode=0`
- `Both (A+C)` → `USB_MODE_BOTH` / `usb_mode=2`

点击后只更新 `g_system_config.usb_mode` 并提示 `Reboot to apply`，不会尝试热切换。
Web 的 `POST /api/usbmode?mode=0|2` 会保存并立即重启。

### 重启应用

**不要**在 `apply_runtime_config()` 里尝试热切换 USB host。建议二选一：

1. **UI 动作后重启**（推荐，和 Web `/api/usbmode` 一致）
   - 在 `main.c` 的 `ui_system_config_changed()` 里检测 `cfg->usb_mode` 是否变化：
     ```c
     bool usb_changed = g_app_config.usb_mode != cfg->usb_mode;
     g_app_config.usb_mode = (uint8_t)cfg->usb_mode;
     if(usb_changed) {
         app_config_save();           // 先落盘
         ESP_LOGW(TAG, "USB mode -> %u, rebooting", cfg->usb_mode);
         vTaskDelay(pdMS_TO_TICKS(500));
         esp_restart();
     }
     ```
   - 注意 `ui_system_config_changed()` 在每次设置变化时都会被调用，只有 `usb_mode` 变化才重启。
2. **只保存，提示用户手动重启**
   - 在 Settings 页面显示 “Reboot to apply”
   - 用户在 Web `/debug` 或电源键重启

如果 UI 代码同时要跑桌面模拟器，`esp_restart()` 要放在 `#if defined(ESP_PLATFORM)` 里，或者通过 `main.c` 的回调执行。

### 其它注意

- `settings_initial_focus(SETTING_USB)` 返回 `0`（USB-A HS）或 `1`（Both A+C）。
- `g_system_config.usb_mode` 的类型是 `int`，`g_app_config.usb_mode` 是 `uint8_t`，赋值时只使用 `0` 或 `2`。
- Web `/api/config` 的 `usb_mode` 只保存不重启；如果希望“保存即重启”，可以复用 `/api/usbmode`。
- `usb_mode=2` 时 USB-C console 不可用；OTA/调试都走 WiFi。切换前确保 WiFi 正常。

## 验证记录

2026-11-07 在 Tab5 上验证通过：

```text
usb_core: USB host peripheral_map=0x3 (USB-A HS + USB-C OTG FS)
usb_core: USB-C FS host routed to FSLS PHY0
usb_core: USB host installed
usb_core: iPod 05ac:12aa -> USB config 2 (iAP2 HID)
usb_hid: HID connected: fff1:00de ...
usb_hid: Ulanzi D100H vendor HID iface=3 addr=3 ready
```

- USB-A：iPod，iAP2 认证和 Now Playing 正常
- USB-C：powered hub + Ulanzi D100H，HID 正常
- 充电：`CHG_EN=1`，`/api/power` 可读到 `chg_en=true`；`/api/charge` 可切换
- 之前的 reset + 喇叭噪声是电池/供电问题，不是 dual-host 初始化问题
