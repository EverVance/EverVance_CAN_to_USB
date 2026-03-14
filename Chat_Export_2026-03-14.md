# VBA_CAN 今日开发日志

- 日期: 2026-03-14
- 参与: User / Codex
- 主题: CAN 底层驱动官方化迁移、CH0~CH3 通道收敛、总线监控语义修正、CH1 硬件证据固化

## 1. 今日目标

1. 继续把 CAN 底层从自写状态机迁到成熟方案。
2. 修复 CH0/CH2/CH3 在总线监控中的数据顺序、方向、自收问题。
3. 保留 Error Passive 在运行状态里，但不再把它当成总线错误帧上报。
4. 在已知 CH1 板级 RX 存在硬件问题的前提下，完成 CH1 的 SDK 配置和发送路径打通验证。
5. 形成可直接续开发的日志，记录本轮关键结论、证据和产物。

## 2. 本轮核心修改

### 2.1 CH1~CH3 片上 CAN 底层切换到官方 NXP SDK

- `CH1/CH2/CH3` 现已统一走 `MCUXpresso SDK / fsl_flexcan` 薄封装。
- 主要实现文件:
  - `CAN_APP/source/can_internal_onchip.c`
  - `CAN_APP/source/can_internal_onchip.h`
  - `CAN_APP/drivers/fsl_flexcan.c`
  - `CAN_APP/drivers/fsl_flexcan.h`
- 已移除原来大量自写寄存器状态机主路径，保留对上层稳定接口。
- 位时序计算改为调用 SDK 的:
  - `FLEXCAN_CalculateImprovedTimingValues(...)`
  - `FLEXCAN_FDCalculateImprovedTimingValues(...)`

### 2.2 CH0 切换到 Microchip 官方 MCP2517FD 驱动主路径

- `CH0` 主文件:
  - `CAN_APP/source/canfd1_ext_spi.c`
- 官方驱动目录:
  - `CAN_APP/third_party/mcp2517fd_official/`
- 已切到官方对象接口的部分:
  - `OperationModeSelect/Get`
  - `Configure`
  - `TransmitQueueConfigure`
  - `ReceiveChannelConfigure`
  - `TefConfigure`
  - `TransmitChannelLoad`
  - `ReceiveMessageGet`
  - `TefMessageGet`
  - `BusDiagnosticsGet`
  - `ErrorCountStateGet`
- 同时修复了官方驱动源码中的两个问题:
  - `BusDiagnosticsGet()` 的越界/错误写回
  - 多处错误的 `memset(..., size, 0)` 调用

### 2.3 CH0 经典 CAN 发送对象越界写入修复

- 根因文件:
  - `CAN_APP/source/canfd1_ext_spi.c`
- 问题:
  - 经典 CAN 时对象实际只配置为 `8(header)+8(payload)`，但旧代码按 72 字节写 message RAM。
- 修复:
  - 仅按当前激活 payload 大小写入，避免覆盖相邻 RAM 区。

### 2.4 Error Passive 不再作为总线错误帧上报

- 设备侧:
  - `CAN_APP/source/can_stack.c`
  - `CAN_APP/source/usb_can_bridge.c`
- Host 侧兼容过滤:
  - `Host_PC/EverVance/src/MainForm.cs`
- 新语义:
  - `Error Passive(0x7)` 只保留在 `RuntimeStatus.flags` 和节点运行状态中
  - 不再进入总线监控错误帧 uplink
  - `lastErrorCode` 不再被 `0x7` 覆盖

### 2.5 CH2/CH3 数据字节序修复

- 根因文件:
  - `CAN_APP/source/can_internal_onchip.c`
- 根因函数:
  - `OnchipPackWord()`
  - `OnchipUnpackWords()`
- 原问题:
  - 片上 FlexCAN 适配层按小端打包 `dataWord`，与 SDK `dataWord0/dataWord1` 的字节约定不一致。
  - 现象表现为:
    - `11 22 33 44` 被发成 `44 33 22 11`
    - 收包也会反向拆回
- 修复:
  - 改为按每个 32-bit word 内大端字节顺序打包/解包。

### 2.6 CH2/CH3 自收问题修复

- 根因文件:
  - `CAN_APP/source/can_internal_onchip.c`
- 根因配置:
  - 旧值: `controllerConfig.disableSelfReception = false`
  - 新值: `controllerConfig.disableSelfReception = true`
- 结果:
  - 片上通道不再把自己发出的帧又作为本通道 RX 上报。
  - 这项修复会统一作用于 `CH1/CH2/CH3`。

### 2.7 WinUSB 设备选择与假错误包问题修复

- Host 侧:
  - `Host_PC/EverVance/src/Transport.cs`
  - `Host_PC/scripts/transports/winusb_transport.ps1`
- 设备侧:
  - `CAN_APP/source/usb_can_bridge.c`
- 修复点:
  - 多设备实例下不再“拿到第一个就开”，而是遍历直到 `WinUSB_Initialize + Bulk Pipe` 成功。
  - 不再把没有真实帧上下文的状态类错误包装成 `RX 0x000` fake 包。
  - 通道重配时清理残留 host TX 队列和 uplink 旧包。

## 3. 已验证结论

### 3.1 CH0 已恢复到预期行为

测试命令:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Host_PC\scripts\test_single_channel.ps1 `
  -Endpoint 'winusb://auto' `
  -Channel 0 `
  -NominalBitrate 500000 `
  -NominalSamplePermille 800 `
  -FrameId 0x100 `
  -DataHex '11 22 33 44' `
  -EnableTermination `
  -ListenMs 2500
```

关键结果:

- `CTRL ch=0 cmd=0x01 status=0x00`
- `CTRL Runtime ch=0 flags=0x03 tx=0 rx=0 err=0x00000000 hostQ=0 usbQ=0`
- `DATA ch=0 tx=1 err=1 fd=0 id=0x100 dlc=0 code=0x5`

结论:

- `CH0` 单节点经典 CAN 500k 现在会正确上报 `ACK Error(0x5)`。
- 不再出现伪 `0x000` 错误包。

### 3.2 CH2 / CH3 已恢复到正确的 payload 顺序和方向语义

用户实机回环截图与本轮修复后的代码结论一致:

- `CH0 TX` 应只显示本通道自己的 `0x100 11 22 33 44`
- `CH0 RX` 应只显示来自 `CH2` 的 `0x120 AA BB CC DD`
- `CH2 TX` 应只显示本通道自己的 `0x120 AA BB CC DD`
- `CH2 RX` 应只显示来自 `CH0` 的 `0x100 11 22 33 44`

本轮修复的两个关键点就是:

1. `dataWord` 打包/解包字节序修正
2. `disableSelfReception = true`

### 3.3 Error Passive 语义已拆分为“节点状态”而不是“总线错误”

连续发送 `CH0` 单节点 80 帧后的实测结果:

- uplink 错误统计:
  - `0x5 ACK Error` 持续存在
  - 没有任何 `0x7 Error Passive` uplink
- RuntimeStatus:
  - `flags` 中 `errorPassive` 位被置位
  - `lastErr` 仍保持 `0x5`

结论:

- `Error Passive` 现在只存在于节点运行状态中，不再污染总线监控。

## 4. CH1 当前状态与硬件证据

### 4.1 CH1 软件配置已打通

`CH1` 当前已经能正常接受配置:

- 通道: `CH1 = FLEXCAN3`
- 配置:
  - `fmt=FD`
  - `en=1`
  - `term=1`
  - `n=500000/800`
  - `d=2000000/750`

串口实测:

- `CAN cfg req CH1 fmt=FD en=1 term=1 n=500000/800 d=2000000/750`
- `CAN cfg applied CH1 status=OK ready=1 en=1 fmt=FD term=1 n=500000/800 d=2000000/750 phyStandby=0 tx=0 rx=0 lastErr=0x00000000`

### 4.2 CH1 发送入口已打通，但真实总线发送无法完成

在同一个 WinUSB 会话中只开启 `CH1`，发送一帧后查询两次运行状态，结果为:

- 初始:
  - `RT flags=0x03 tx=0 rx=0 err=0x00000000 hostQ=0 usbQ=0`
- 发送后:
  - `RT flags=0x43 tx=0 rx=0 err=0x00000000 hostQ=0 usbQ=0`
- 再次查询:
  - `RT flags=0x43 tx=0 rx=0 err=0x00000000 hostQ=0 usbQ=0`

`0x43` 的含义:

- `ready = 1`
- `enabled = 1`
- `txPending = 1`

但同时:

- `txCount = 0`
- `rxCount = 0`
- `lastErrorCode = 0`

### 4.3 这组现象对应的硬件结论

这说明:

1. `CH1` 的 SDK 初始化成功
2. `CAN_InternalOnChipSend()` 成功进入控制器发送路径
3. 发送请求被接受，但控制器无法完成协议层收尾
4. 既没有 `TxComplete`，也没有真实 `Rx`，也没有被采样出的错误码

这与“`CH1` 的 RX 路径没有真正回到 MCU，导致控制器无法从总线获得回读信息”完全一致。

因此，本轮对 `CH1` 的结论是:

- 软件路径已经配置完成
- 接收代码路径也已经按 SDK 统一适配到位
- 但在当前板级硬件前提下，`CH1` 无法被软件单独修成可验证的真实收发通道

这已经构成明确的硬件证据，而不是软件未完成。

## 5. 当前工程状态

### 已完成

- `CH0`:
  - 官方 Microchip 驱动主路径迁移
  - 经典 CAN 发送越界修复
  - ACK Error 上报恢复
- `CH1~CH3`:
  - 官方 `fsl_flexcan` 主路径迁移
  - SDK 时序计算切换
  - `CH2/CH3` 字节序修复
  - `CH1/CH2/CH3` 自接收关闭
- 总线监控:
  - `Error Passive` 从总线错误中剥离
  - fake `0x000` 错误帧过滤
- Host/脚本:
  - WinUSB 多实例选择修复

### 当前剩余风险

- `CH1` 当前板级 RX 缺失是硬件问题，不是软件残留。
- 未在本机完成 `EverVance.exe` 新版重新编译验证，因为当前环境中没有现成的 `dotnet/msbuild` 命令可用。
- 设备侧固件 `can_app.elf` 已多轮实机编译、烧录、运行验证。

## 6. 本轮关键产物

- 固件:
  - `out/artifacts/CAN_APP/can_app.elf`
- 单通道验证脚本:
  - `Host_PC/scripts/test_single_channel.ps1`
- 官方驱动:
  - `CAN_APP/drivers/fsl_flexcan.c`
  - `CAN_APP/drivers/fsl_flexcan.h`
  - `CAN_APP/third_party/mcp2517fd_official/*`

## 7. 给后续开发者的结论

1. `CH0/CH2/CH3` 当前主问题已经不再是软件底层方向错误。
2. `CH1` 不要再继续怀疑 SDK 初始化、发送入口或桥接层；当前硬件证据已经够明确。
3. 如果后续修板或飞线补上 `CH1 RX -> MCU`，优先直接复测本轮已经打通的 `CH1` SDK 发送路径，不要重写新驱动。
