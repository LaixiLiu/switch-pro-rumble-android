# Switch Pro Controller rumble for Android (Magisk)

让 Android 手机支持 **Nintendo Switch Pro Controller** 的震动，震动强度由游戏实时驱动。

适用于内核没有 `hid-nintendo` / `FF` 支持的手机（本项目在魅族 21 / Flyme 12.6 / Android 16 / 内核 6.1.25 上开发验证）。

## 原理

```
游戏 (InputDevice.getVibrator, 强度 0-65535)
  → Android 框架 → uinput 虚拟手柄 (FF_RUMBLE)
  → 本项目的守护进程 (switch_rumble_bridge)
  → Nintendo HID 输出报文 0x10 → hidraw → 蓝牙 → 手柄马达
```

守护进程做三件事：

1. 创建一个与真实手柄同名同能力的 uinput 虚拟手柄，并注册 `FF_RUMBLE`
2. `EVIOCGRAB` 真实手柄的 evdev 节点，把输入事件转发给虚拟手柄
3. 接收框架下发的 FF 效果（幅度/时长），用 SDL3 的幅度表编码成 Nintendo 0x10 报文写入 hidraw

手柄断线（休眠/蓝牙掉线）时保留虚拟手柄并每 2 秒探测，手柄回来后自动重新接管。

## 模块

| 目录 | 模块 | 说明 |
|---|---|---|
| `bridge/` | Switch Pro Controller Rumble Bridge | 主模块。Magisk 卡片 Action 手动启动/停止，无常驻后台 |
| `probe/` | Switch Controller HID Access Probe | 早期的探测模块，只用于验证 Magisk 域能否写 hidraw；现在仅作参考 |
| `tools/` | — | 手动震动测试脚本（单发、强度阶梯、频率扫描、A/B 对照、按键码探针） |

## 构建

需要 Android NDK（r27c 验证过），修改 `bridge/build.sh` 里的 `NDK` 路径后：

```sh
sh bridge/build.sh
```

产物为 `bridge/bin/switch_rumble_bridge`（arm64，动态链接 bionic；静态链接会因 TLS 对齐被 Android 16 拒绝）。

## 安装

```sh
# 打包（模块 zip 的根目录即 bridge/ 的内容）
cd bridge && zip -r ../bridge.zip module.prop sepolicy.rule action.sh bin
adb push ../bridge.zip /data/local/tmp/
adb shell su -c "magisk --install-module /data/local/tmp/bridge.zip"
# 重启一次以加载 sepolicy.rule 并使模块生效
```

## 使用

Magisk → 模块 → Switch Pro Controller Rumble Bridge → 操作（Action）：

- **音量+ = 下一项，音量- = 确认**（按键式菜单，Magisk 的 Action 终端在本机不支持交互输入）
- 菜单项：启动桥接 / 震动强度 / 震动自测 / 查看日志 / 停止桥接 / 退出
- 强度档位：弱 70% → 标准 100% → 强 130% → 超强 160%（对中低强度提升明显，协议上限不变）

启动后建议重进游戏，让游戏重新识别手柄设备。

## 已踩过的坑

- **bionic 的 uapi 头文件很旧**：`uinput_ff_upload` 没有（也不该有）`effect_id`，效果 id 在 `effect.id`；结构体尺寸必须与内核一致，否则 ioctl 返回 EINVAL
- **`EVIOCSFF` 会阻塞**等待 uinput owner 应答，自测必须 `fork()` 子进程
- **`/dev/uinput` 必须 `O_NONBLOCK`**，否则读事件后阻塞在 read()，SIGTERM 也无法退出
- **魅族 21 音量键键码与物理标签相反**：物理音量+ = `KEY_VOLUMEDOWN(0073)@event0`，物理音量- = `KEY_VOLUMEUP(0072)@event2`
- **静态链接的 bionic 程序**会报 "TLS segment is underaligned"，改动态链接
- **150 Hz 与 160/320 Hz 编码在本手柄上都能震**；默认用内核的 160/320 Hz
- `settings put system volume_music` 用于菜单退出后恢复音量

## 编码参考

- Linux `drivers/hid/hid-nintendo.c`（0x10 报文与幅度表）
- SDL3 `src/joystick/hidapi/SDL_hidapi_switch.c`（101 项幅度查找表）
- dekuNukem/Nintendo_Switch_Reverse_Engineering 的 `rumble_data_table.md`

## 许可证

本项目以 **GNU General Public License v3.0** 发布，详见 [LICENSE](LICENSE)。

震动编码数据来源于 Linux 内核 `hid-nintendo.c`（GPL-2.0）和 SDL3 `SDL_hidapi_switch.c`（Zlib）；协议本身来自社区逆向文档。
