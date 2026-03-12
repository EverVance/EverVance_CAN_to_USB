# VBA_CAN 工程目录与续开发上下文

本文档用于让后续模型或开发者快速接手当前工程。重点记录:

- 工程目录划分
- 每个关键文件夹的作用
- 当前开发边界
- 不应踩的约束
- 从哪里继续推进最合适

## 1. 当前工程总体目标

本工程是一个基于 `RT1062` 的 USB 转 CAN / CAN FD 项目，包含:

- 外部 `MCP2517FD` 通道
- 片上 `FlexCAN` 通道
- `Bootloader + CAN_APP` 双镜像
- Windows 上位机 `EverVance`
- WinUSB 链路

目标是实现:

- Host_PC 通过 USB 与设备通信
- 设备将 USB 协议转换为 CAN / CAN FD 控制与数据收发
- 后续支持稳定量产维护

## 2. 当前已经冻结的重要约束

### 2.1 XIP 约束

- 当前已经实现 XIP。
- 不要修改 XIP 工程。
- 本项目后续开发默认保持现有 XIP 方案不变。

### 2.2 烧录布局

- `Bootloader` 烧录到 `0x60000000`
- `CAN_APP` 烧录到 `0x60020000`

### 2.3 通道定义冻结

- `CH0 = LPSPI1 + MCP2517FD`
- `CH1 = FLEXCAN3`
- `CH2 = FLEXCAN1`
- `CH3 = FLEXCAN2`

能力边界:

- `CH0`: 目标支持 `CAN FD`
- `CH1`: 支持 `CAN FD`
- `CH2`: 仅支持经典 `CAN`
- `CH3`: 仅支持经典 `CAN`

### 2.4 LED 语义冻结

- `LED1`: `CAN_FD_1 RX`
- `LED2`: `CAN_FD_1 TX`
- `LED3`: `CAN_FD_2` 活动
- `LED5`: 电脑已识别 USB 设备
- `LED6`: Host_PC 与设备的连接状态
- `LED4`: 禁止初始化和驱动

说明:

- `LED6` 不是总线活动灯，只表示 Host_PC 会话状态。

## 3. 根目录结构

### `Bootloader/`

设备端 Bootloader 工程。

主要作用:

- 提供设备启动入口
- 跳转到 `CAN_APP`
- 使用与应用一致的外部 Flash / XIP 启动布局

关键内容:

- `source/`: Bootloader 业务代码
- `startup/`: 启动文件、向量表、跳转逻辑
- `board/`: 板级 pin/clock/debug console 配置
- `drivers/`, `device/`, `CMSIS/`, `component/`, `utilities/`: MCU SDK 基础组件
- `xip/`: XIP 相关支持
- `Bootloader_Debug.ld`: 链接脚本
- `CMakeLists.txt`: Bootloader 构建入口

### `CAN_APP/`

设备主应用工程，是当前最主要的固件开发目录。

主要作用:

- USB Vendor/Bulk 通信
- USB-CAN 协议桥接
- CAN / CAN FD 通道控制
- RTOS 任务调度
- 板级 GPIO / LED / 终端电阻 / 收发器控制

关键子目录:

- `source/`: 主要业务代码
  - `CAN_APP.c`: 主入口
  - `rtos_app.c`: RTOS 启动与任务
  - `usb_can_bridge.c`: USB 协议桥
  - `can_stack.c`: 通道抽象层
  - `bsp_peripherals.c`: 板级 GPIO / LED / 终端电阻 / pin mux
  - `canfd1_ext_spi.c`: `CH0 MCP2517FD`
  - `can_internal_onchip.c`: 片上 `FlexCAN`
  - `tja1042_drv.c`: 收发器控制
- `board/`: 板级 `pin_mux` / `board.c` / 调试串口配置
- `rtos/`: FreeRTOS 内核
- `usb/`: USB device stack、EHCI、PHY
- `startup/`: 启动文件和向量表
- `drivers/`, `device/`, `CMSIS/`, `component/`, `utilities/`: MCU SDK 基础组件
- `xip/`: XIP 相关支持
- `CAN_APP_Debug.ld`: 应用链接脚本
- `CMakeLists.txt`: 应用构建入口

### `Host_PC/`

Windows 侧上位机与脚本工具。

主要作用:

- 人机交互界面
- 发送控制命令和 CAN/CAN FD 数据
- 观察总线监控与运行状态
- WinUSB 连接测试和脚本化压测

关键子目录:

- `EverVance/`: 当前主上位机工程
  - `src/`: C# 源码
  - `build.bat`: 编译脚本
  - `bin/`: 输出目录，生成 `EverVance.exe`
  - `assets/`: 图标等资源
- `scripts/`: PowerShell 脚本
  - 连接测试
  - WinUSB 传输测试
  - 压测脚本
- `winusb/`: WinUSB 相关 INF / 驱动包材料
- `docs/`: 上位机补充文档
- `logs/`: 上位机日志
- `README.md`: Host_PC 使用说明

### `Hardware/`

硬件参考资料目录。

主要作用:

- 原理图网表
- 芯片手册
- 外设芯片手册

关键文件:

- `Netlist_Schematic1_2026-03-08.tel`: 当前主要使用的网表
- `IMXRT1060RM.pdf`: RT1062 参考手册
- `IMXRT1060CEC.pdf`: RT1062 芯片手册
- `MCP2517FD.pdf`: 外部 CAN FD 控制器手册
- `TJA1042.pdf`: 收发器手册
- `MX25L3233F.pdf`: 外部 Flash 手册

### `tools/`

工具链相关目录。

当前关键内容:

- `linkserver/`: LinkServer 烧录、恢复、FlashDriver、配置文件

### `out/`

统一输出目录。

主要作用:

- `build/`: CMake / Ninja 构建目录
- `artifacts/`: 最终产物目录

关键产物:

- `out/artifacts/Bootloader/bootloader.elf`
- `out/artifacts/CAN_APP/can_app.elf`

### `EverVance_V1.0/`

旧版上位机副本/历史保留目录。

用途:

- 作为历史参考
- 当前主开发不优先改这里，默认改 `Host_PC/EverVance`

### 其他根目录文件

- `CMakeLists.txt`: 顶层 CMake 入口
- `CMakePresets.json`: 构建预设
- `COMM_PROTOCOL_NOTES.md`: 协议与联调约束文档
- `USB_DRIVER_STRATEGY_2026-03-11.md`: USB 驱动策略记录
- `Chat_Export_2026-03-08.md`, `Chat_Export_2026-03-10.md`, `Chat_Export_2026-03-12.md`: 分日期开发日志

## 4. 当前协议与连接状态

### 已经稳定的部分

- WinUSB 枚举与连接链路已基本打通。
- 上位机连接状态不再因 UI 自己的超时逻辑而误断开。
- `LED5/LED6` 的业务语义已明确。
- 通道配置下发至少已验证能联动终端电阻 GPIO。

### 当前串口调试方式

设备端当前使用 `LPUART1` 输出协议级调试信息:

- `K14 = TX`
- `L14 = RX`
- `115200 8N1`

串口日志适合观察:

- 上位机是否真的发出了配置命令
- 设备是否接受配置
- 配置是否被归一化
- 底层是否返回 `OK / STAGED / INVALID`

### 当前总线监控的真实含义

当前总线监控不能再被理解成“所有 TX 都已真实上总线”。

当前状态:

- `RX`: 设备真实接收到总线帧后上报
- `TX`: 假的“软件发送成功即回显”已被关闭

后续若要恢复 `TX` 监控，必须基于控制器真实完成发送事件。

## 5. 当前最重要的代码位置

### 设备端

- `CAN_APP/source/CAN_APP.c`
  - 主入口
  - Debug console 初始化

- `CAN_APP/source/rtos_app.c`
  - RTOS 任务创建
  - USB、CAN worker 调度
  - LED 状态更新

- `CAN_APP/source/usb_can_bridge.c`
  - USB 协议解析与封包
  - 控制命令处理
  - Host_PC 会话状态维护
  - 串口协议日志

- `CAN_APP/source/can_stack.c`
  - 通道能力
  - 配置归一化
  - 配置真正落到底层
  - 串口配置日志

- `CAN_APP/source/canfd1_ext_spi.c`
  - `CH0 MCP2517FD` 驱动与 bring-up

- `CAN_APP/source/can_internal_onchip.c`
  - `CH1/CH2/CH3` 片上 `FlexCAN`

- `CAN_APP/source/bsp_peripherals.c`
  - LED
  - 终端电阻 GPIO
  - 收发器控制
  - LPSPI / FLEXCAN pin mux

### 上位机

- `Host_PC/EverVance/src/MainForm.cs`
  - UI
  - 连接状态
  - 配置同步
  - Heartbeat 保活
  - 总线监控

- `Host_PC/EverVance/src/Transport.cs`
  - MockTransport
  - WinUSB 传输层

## 6. 当前已知风险与未完成点

1. `CH0 MCP2517FD` 的 `CAN FD` 数据面还没有完全收敛。

2. `TX` 真实发送完成事件尚未建立。
   - 当前只是先关闭假的软件 TX 回显。

3. `ACK Error / Arbitration Lost` 的错误分类仍需继续验证。

4. 配置面需要继续验证“是否真正落到底层控制器”，不能只看上位机 UI 是否显示成功。

## 7. 下一步建议的开发顺序

推荐按以下顺序继续:

1. 配置面验证
   - 上位机下发 `SetChannelConfig`
   - 观察 `LPUART1` 日志
   - 再做 `GetChannelConfig / GetRuntimeStatus`

2. 单通道数据面验证
   - 先 `CH2/CH3` 经典 CAN
   - 再 `CH1 CAN FD`
   - 最后 `CH0 MCP2517FD`

3. 错误路径验证
   - 无 ACK
   - 总线断开
   - BusOff / ErrorPassive
   - 错误码上报是否正确

4. 最后再做并发压测
   - 四通道同时打开
   - 大吞吐下的队列与丢包统计

## 8. 后续模型接手时的注意事项

- 不要改 XIP 工程。
- 不要驱动 `LED4`。
- `LED6` 只表示 Host_PC 会话状态，不表示总线活动。
- 若要判断配置是否真的生效，优先看 `LPUART1` 串口日志。
- 若要判断总线发送是否真实成功，当前不能只看总线监控里的 `TX`。
- 若要继续做协议开发，必须同步维护 `COMM_PROTOCOL_NOTES.md`。
