# VBA_CAN 今日开发日志

- 日期: 2026-03-12
- 主题: USB 连接稳定化、Host_PC 交互修复、协议语义收敛、设备端调试输出补充
- 参与: User / Codex

## 1. 今日目标

1. 稳定 `RT1062 Bulk CAN <-> EverVance` 的 WinUSB 链路。
2. 修复上位机交互问题，避免输入被打断、错误提示卡死、连接状态失真。
3. 收敛 `LED5/LED6` 语义，使其反映 USB 枚举和 Host_PC 会话状态。
4. 收敛协议语义，避免把软件行为误报成真实总线事件。
5. 为后续联调补上设备端串口调试输出，直观看上位机配置是否真正被设备接受。

## 2. 今日完成的关键改动

### 2.1 上位机 UI 与连接管理

- 修复了通道管理页“每输入一个字符就打断编辑”的问题。
- 修复了配置错误时反复弹窗导致程序无法关闭的问题，改为状态栏提示。
- 在总线监控页增加“清空日志”按钮。
- 增加连接态防呆:
  - 已连接时锁定通道配置编辑和通道增删。
  - 断开后若配置被修改，会标记为“待同步”，下次连接时自动重新同步。
- 修复了上位机“自己误判掉线”的问题:
  - 之前 `_lastDeviceActivityUtc` 没有在收到设备数据后刷新。
  - 这会导致真机和 `Mock` 都在数秒后被 UI 自己判成断开。
  - 已在 `PollTransport()` 中补上设备活动时间戳刷新。
- 增加后台 `Heartbeat("LINK")` 保活。
- 增加主动断开时的 `Heartbeat("UNLK")`，用于通知设备立即清除连接态。

### 2.2 设备端 LED 语义收敛

- `LED1`: `CAN_FD_1 RX`
- `LED2`: `CAN_FD_1 TX`
- `LED3`: `CAN_FD_2` 活动
- `LED5`: 电脑已识别 USB 设备
- `LED6`: Host_PC 与设备的会话连接状态
- `LED4`: 明确禁止初始化和驱动，避免硬件冲突风险

当前 `LED6` 的实现语义:

- 仅表示 Host_PC 会话状态，不表示总线活动。
- 连接后由 Host_PC 立即发送 `LINK` 心跳置位。
- 断开前由 Host_PC 发送 `UNLK` 心跳清位。
- 若异常断开，设备端超时窗口约为 `2s`，超时后自动熄灭。

### 2.3 协议语义与总线监控收敛

- 设备端已关闭“软件 TX 成功即立刻回显为 TX 上报”的假事件。
- 因此当前总线监控不应再出现“点击发送就立刻看到假的 TX”。
- 当前语义是:
  - `RX`: 设备侧真实收到后上报
  - `TX`: 暂不再伪造，后续应在“控制器真实完成发送”后再恢复
- 这一步是为了避免总线监控误导调试。

### 2.4 USB 链路与连接状态

- WinUSB 链路已恢复到稳定可连接状态。
- `MockTransport` 和真机都已不再因为 UI 自己的超时逻辑而频繁误断开。
- `LED6` 与 Host_PC 会话状态已联动，但仍建议继续做真机边界验证。

### 2.5 设备端串口调试输出

设备端已经确认使用 `LPUART1` 作为调试串口输出:

- 引脚:
  - `K14 = LPUART1_TX`
  - `L14 = LPUART1_RX`
- 波特率:
  - `115200 8N1`

本轮没有另起一套 UART 驱动，而是直接复用已有 `BOARD_InitDebugConsole()` 和 `PRINTF()`。

串口上现在会输出以下高价值信息:

- 启动横幅
- `SetChannelConfig` 请求
- `GetChannelConfig` 请求
- `GetDeviceInfo` 请求
- `GetChannelCapabilities` 请求
- `GetRuntimeStatus` 请求
- `CAN cfg req`，即设备归一化后的配置
- `CAN cfg applied`，即底层应用后的状态
- 对应控制命令的响应摘要

刻意没有把后台 `LINK/UNLK` 心跳刷到串口，避免日志被保活包淹没。

## 3. 今天确认的硬件与通道结论

- `CH0 = LPSPI1 + MCP2517FD`
- `CH1 = FLEXCAN3`
- `CH2 = FLEXCAN1`
- `CH3 = FLEXCAN2`

能力边界当前按以下规则继续开发:

- `CH0`: 目标支持 `CAN FD`
- `CH1`: 支持 `CAN FD`
- `CH2`: 仅支持经典 `CAN`
- `CH3`: 仅支持经典 `CAN`

终端电阻控制已验证“至少 GPIO 联动有效”:

- 上位机下发通道配置后，终端电阻开关会随配置变化。

## 4. 今日编译产物

- 设备端固件:
  - `out/artifacts/CAN_APP/can_app.elf`
  - `out/artifacts/CAN_APP/can_app.bin`
- 上位机:
  - `Host_PC/EverVance/bin/EverVance.exe`

说明:

- 本轮没有改 XIP 工程。
- Bootloader / CAN_APP 的烧录布局未改:
  - `Bootloader -> 0x60000000`
  - `CAN_APP -> 0x60020000`

## 5. 当前推荐联调方式

下一步不要直接跳到高压总线压测，先做“配置面是否真实生效”的验证。

推荐顺序:

1. 上位机下发 `SetChannelConfig`
2. 观察 `LPUART1` 日志:
   - 是否出现 `USB ctrl req`
   - 是否出现 `CAN cfg req`
   - 是否出现 `CAN cfg applied`
   - 响应状态是 `OK / STAGED / INVALID`
3. 再用上位机发 `GetChannelConfig / GetRuntimeStatus`
4. 用串口日志和上位机回包对照确认:
   - 模式是否切换
   - 波特率/采样点是否被接受
   - 终端电阻和使能是否联动

之后再做数据面验证:

1. 先单通道经典 CAN
2. 再测 `CH1 CAN FD`
3. 最后再碰 `CH0 MCP2517FD`

## 6. 当前未完全收敛的问题

1. `TX` 语义仍未真正收敛到“控制器真实完成发送”
   - 本轮只是先去掉假的软件 TX 回显
   - 后续需要基于真实控制器发送完成事件上报

2. `CH0 MCP2517FD` 的 `CAN FD` 数据面仍未完全收敛
   - 当前优先用于继续做配置面和底层 bring-up 诊断

3. `ACK Error / Arbitration Lost` 分类还没有完全做成可依赖结论
   - 之前 `CH2` 无 ACK 场景出现过 `Arbitration Lost`
   - 后续仍需要继续核对控制器错误寄存器与协议映射

4. 总线监控当前更适合作为“真实 RX + 错误事件观察”
   - 不能再把它当成“TX 已真实上总线”的证明

## 7. 今天涉及的关键文件

- 设备端:
  - `CAN_APP/source/CAN_APP.c`
  - `CAN_APP/source/rtos_app.c`
  - `CAN_APP/source/usb_can_bridge.c`
  - `CAN_APP/source/usb_can_bridge.h`
  - `CAN_APP/source/can_stack.c`
  - `CAN_APP/source/bsp_peripherals.c`

- 上位机:
  - `Host_PC/EverVance/src/MainForm.cs`

- 协议文档:
  - `COMM_PROTOCOL_NOTES.md`

## 8. 给后续模型的接续提示

继续开发时，先默认以下上下文成立:

- USB 连接已基本稳定。
- `LED5/LED6` 的业务语义已经冻结。
- 上位机输入打断、错误弹窗、连接状态自断开这类基础交互问题已基本修完。
- 后续重点应转到“配置是否真的落到底层”和“数据面是否是真实总线事件”。
- 若需要看设备配置落地情况，优先看 `LPUART1` 串口日志，不要只看上位机界面。
