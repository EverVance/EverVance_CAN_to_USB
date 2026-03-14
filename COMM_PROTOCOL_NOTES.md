# VBA_CAN 通信协议与下位机联调注意事项

本文档用于约束 Host_PC 与下位机（Bootloader/CAN_APP）之间的 USB/CAN 数据协议，避免后续开发遗漏。

## 硬件通道冻结（2026-03-11）
- `Channel 0`：`LPSPI1 + MCP2517FD`，对应 `CAN_FD_1`
- `Channel 1`：`FLEXCAN3`，对应 `CAN_FD_2`
- `Channel 2`：`FLEXCAN1`，对应 `CAN_1`
- `Channel 3`：`FLEXCAN2`，对应 `CAN_2`

补充约束：
- `Channel 1 (FLEXCAN3)` 支持 `CAN FD`
- `Channel 2 (FLEXCAN1)` 仅支持经典 `CAN`
- `Channel 3 (FLEXCAN2)` 仅支持经典 `CAN`
- 固件内部若沿用历史枚举名，必须保证对外 `Channel` 编号仍按以上定义解释

## LED 约束（2026-03-12）
- `LED1`：`CAN_FD_1 RX`
- `LED2`：`CAN_FD_1 TX`
- `LED3`：`CAN_FD_2` 收发活动
- `LED5`：电脑已识别 USB 设备
- `LED6`：`Host_PC` 与设备通信链路已建立
- 禁止初始化或驱动 `LED4`；原理图网表显示其控制引脚与 `LED1` 存在硬件冲突风险

补充约束：
- `LED6` 不以“USB 已配置”代替“上位机已连接”，应由设备根据最近有效的 Host 控制命令判断。
- `Host_PC` 连接成功后应周期性发送 `Heartbeat(0x06)`，用于维持设备端连接态指示。
- `Host_PC` 断开连接前应先发送一个 `Heartbeat(0x06)`，载荷为 ASCII `UNLK`，设备收到后立即清除连接态指示。

## 0. 维护约定（必须执行）
- 只要涉及“通信协议、总线状态上报、下位机接口字段”改动，必须同步更新本文档。
- 合并代码前，至少确认 `1.1 Flags`、`2. 错误类型编码`、`3. 总线监控联调要求` 是否需要变更。
- 若新增字段，先在本文档定义语义，再在 Host/Device 两侧实现。

## 1. 统一帧封包格式（Host <-> Device）
当前上位机使用如下二进制封包：

- Byte0: `0xA5`（帧头）
- Byte1: `Channel`（0~3）
- Byte2: `DLC`（数据长度）
- Byte3: `Flags`（位定义见下）
- Byte4~7: `CAN ID`（uint32，小端）
- Byte8...: `Data`（长度为 DLC）

当前协议分两类：
- `普通数据帧`：`Flags.bit3 = 0`，用于 CAN/CAN FD 的 TX/RX/错误上报。
- `控制命令帧`：`Flags.bit3 = 1`，用于通道参数配置、能力查询、状态查询等管理命令。

### 1.1 Flags 位定义（必须与下位机一致）
- bit0: `1=CAN FD`, `0=经典 CAN`
- bit1: `1=TX`, `0=RX`（以 Host 视角）
- bit2: `1=错误帧`, `0=正常帧`
- bit3: `1=控制命令帧`, `0=普通 CAN/CAN FD 数据帧`
- bit7..bit4: `ErrorCode`（错误类型编码）

说明：
- 当 `bit3=1` 时，Byte4~7 不再表示 CAN ID，而是控制命令头。
- 非错误帧时 `ErrorCode=0`。

## 1.2 控制命令头（Host <-> Device）
当 `Flags.bit3 = 1` 时，Byte4~7 定义如下：

- Byte4: `Command`
- Byte5: `Status`
- Byte6: `Sequence`
- Byte7: `ProtocolVersion`

当前约定：
- `Command = 0x01`：下发通道配置 / 配置回执
- `ProtocolVersion = 0x01`

`Status` 当前定义：
- `0x00`：配置已生效
- `0x01`：参数非法，设备拒绝
- `0x02`：仅应用了立即可生效部分；回包与 `GetChannelConfig` 返回设备当前实际运行配置，未下沉的位时序不会被伪装成已生效
- `0x03`：设备内部错误

### 1.2.1 命令字冻结建议
当前已用：
- `0x01`：`SetChannelConfig`

建议立即冻结但可后续实现：
- `0x02`：`GetChannelConfig`
- `0x03`：`GetDeviceInfo`
- `0x04`：`GetChannelCapabilities`
- `0x05`：`GetRuntimeStatus`
- `0x06`：`Heartbeat`

说明：
- 本轮先不要求全部实现，但命令号应先冻结，避免后续 Host/Device 并行开发时反复改协议。
- `Sequence` 由 Host 侧递增维护；Device 回包必须原样带回，便于后续做超时与重试。

### 1.2.2 当前已实现控制命令（2026-03-11 更新）
- `0x01 SetChannelConfig`
  - 请求：`16` 字节配置载荷
  - 响应：回显“设备当前实际运行配置”，载荷同 `SetChannelConfig`
- `0x02 GetChannelConfig`
  - 请求：无载荷，`Channel=目标通道`
  - 响应：`16` 字节当前实际运行配置载荷
- `0x03 GetDeviceInfo`
  - 请求：无载荷
  - 响应：`16` 字节设备信息载荷
- `0x04 GetChannelCapabilities`
  - 请求：无载荷，`Channel=目标通道`
  - 响应：`20` 字节能力载荷
- `0x05 GetRuntimeStatus`
  - 请求：无载荷，`Channel=目标通道`
  - 响应：`20` 字节运行态载荷
- `0x06 Heartbeat`
  - 请求：可带任意短载荷
  - 响应：原样回显载荷
  - 约定保留载荷：
    - `LINK`：Host_PC 后台保活心跳
    - `UNLK`：Host_PC 主动断开通知，设备收到后应立即清除连接态

## 1.3 通道配置命令载荷（Command = 0x01）
Payload 长度固定 `16` 字节：

- Byte0: `PayloadVersion`，当前固定 `0x01`
- Byte1: `FrameType`，`0=CAN`，`1=CAN FD`
- Byte2: `Enabled`，`0=关闭通道`，`1=打开通道`
- Byte3: `TerminationEnabled`，`0=终端电阻断开`，`1=终端电阻接入`
- Byte4~7: `NominalBitrate`，uint32，小端
- Byte8~9: `NominalSamplePointPermille`，uint16，小端，单位 `0.1%`
- Byte10~13: `DataBitrate`，uint32，小端；经典 CAN 固定填 `0`
- Byte14~15: `DataSamplePointPermille`，uint16，小端；经典 CAN 固定填 `0`

上位机防呆约束：
- 经典 CAN 只允许配置仲裁域采样点。
- CAN FD 允许配置仲裁域与数据域两个采样点。
- 常用采样点通过下拉预设给出，自定义时填写百分比字符串，由上位机换算成 `0.1%` 单位下发。
- 当前 EverVance 行为：每次连接成功后自动批量下发全部通道配置，不再提供手工单条下发按钮。

### 1.3.1 `GetChannelConfig(0x02)` 响应载荷
- 与 `SetChannelConfig(0x01)` 完全一致，便于 Host 共用解析逻辑。
- 若 `Status=0x02`，则该载荷必须表示“设备当前实际运行配置”，而不是尚未落到底层控制器的目标值。

## 1.5 DeviceInfo 响应载荷（Command = 0x03）
Payload 长度固定 `16` 字节：

- Byte0: `PayloadVersion`，当前固定 `0x01`
- Byte1: `FwMajor`
- Byte2: `FwMinor`
- Byte3: `FwPatch`
- Byte4~7: `FeatureFlags`，uint32，小端
- Byte8~9: `USB VID`，uint16，小端
- Byte10~11: `USB PID`，uint16，小端
- Byte12: `ChannelCount`
- Byte13~15: 保留

`FeatureFlags` 当前定义：
- bit0: `USB Vendor/Bulk` 已启用
- bit1: 连接后自动同步通道配置
- bit2: 存在外置 `CAN FD` 控制器
- bit3: 存在片上 `CAN FD` 控制器
- bit4: 支持终端电阻控制
- bit5: 支持 `TJA1042` 软件状态机

## 1.6 ChannelCapabilities 响应载荷（Command = 0x04）
Payload 长度固定 `20` 字节：

- Byte0: `PayloadVersion`
- Byte1: `Channel`
- Byte2: `CapabilityFlags`
- Byte3: `DriverType`
- Byte4~7: `NominalBitrateMin`
- Byte8~11: `NominalBitrateMax`
- Byte12~15: `DataBitrateMax`
- Byte16~17: `NominalSampleMinPermille`
- Byte18~19: `NominalSampleMaxPermille`

`CapabilityFlags` 当前定义：
- bit0: 支持经典 CAN
- bit1: 支持 CAN FD
- bit2: 支持终端电阻控制

`DriverType` 当前定义：
- `0x01`: `MCP2517FD`
- `0x02`: `FlexCAN1`
- `0x03`: `FlexCAN2`
- `0x04`: `FlexCAN3`

## 1.7 RuntimeStatus 响应载荷（Command = 0x05）
Payload 长度固定 `20` 字节：

- Byte0: `PayloadVersion`
- Byte1: `RuntimeFlags`
- Byte2: `LastErrorCodeLow8`
- Byte3: 保留
- Byte4~7: `TxCount`
- Byte8~11: `RxCount`
- Byte12~15: `LastErrorCode`
- Byte16: `HostToCanPending`
- Byte17: `DeviceToHostPending`
- Byte18: `HostToCanDropCountLow8`
- Byte19: `DeviceToHostDropCountLow8`

`RuntimeFlags` 当前定义：
- bit0: 控制器已 ready
- bit1: 通道已启用
- bit2: `BusOff`
- bit3: `ErrorPassive`
- bit4: 物理层当前处于 `Standby`
- bit5: 存在待取接收帧
- bit6: 存在待发发送帧

补充说明：
- `HostToCanPending` 表示该通道当前等待送入底层 CAN 驱动的 Host 普通数据帧数量，按 `0xFF` 饱和。
- `DeviceToHostPending` 表示该通道当前等待通过 USB 发回 Host 的数据帧数量，按 `0xFF` 饱和。
- 两个 `DropCountLow8` 都是低 8 位观测值，只用于快速发现压测中的堆积/溢出；完整计数仍以固件调试日志为准。

## 1.4 收发帧管理约定
此部分用于冻结“业务帧怎么收、怎么发、怎么回显”，避免后续上位机与固件理解偏差。

### 1.4.1 Host -> Device 发送约定
- Host 下发的普通数据帧必须满足对应通道当前 `FrameType`。
- 经典 CAN：`DLC <= 8`
- CAN FD：`DLC <= 64`
- Host 只负责按协议封包，不在普通数据帧里重复携带 bitrate/sample point；这些属于通道配置命令。
- Device 收到非法帧时，不应静默吞掉；建议返回错误帧或控制回执。

### 1.4.2 Device -> Host 上报约定
- TX 成功：只能在底层控制器确认发送完成后上报，回显同一帧，`bit1=1`，`bit2=0`
- RX 正常：上报原始总线帧，`bit1=0`，`bit2=0`
- 错误事件：`bit2=1`，并填 `ErrorCode`
- 经典 CAN 与 CAN FD 的区分只看 `bit0`
- 通道号必须始终保留，禁止因内部桥接简化而丢失来源通道
- 禁止把“软件 TX 回显”伪装成 `RX`；若需要上报发送事件，只能作为 `TX` 上报

2026-03-12 当前实现补充：
- `CH1/CH2/CH3(FlexCAN)`：`TX` 上报已改为基于控制器真实完成事件，不再在 Host 请求入队时立刻回显。
- `CH0(MCP2517FD)`：`TX` 上报改为基于 `TEF(Transmit Event FIFO)`；`RX` 上报改为基于 `FIFO1` 实际收帧。
- `错误上报`：
  - `FlexCAN` 当前优先基于 `ESR1/ECR`
  - `MCP2517FD` 当前优先基于 `TXQSTA/BDIAG1/TREC`
  - 空总线单节点场景下，后续仍应继续重点验证 `ACK Error` 与 `Bus Off` 的先后与分类是否完全符合预期

### 1.4.3 顺序与流控约定
- 单通道内尽量保持“Host 下发顺序 == Device 送入驱动顺序”。
- 多通道之间允许交错，不保证全局顺序。
- USB Bulk 层不保证“一个 USB 包只承载一个业务时刻”；Host 解析必须按 `0xA5 + DLC` 逐帧拆包。
- Device 端也必须按字节流重组 USB OUT 数据；一个 USB 包内可包含 `0..N` 个完整业务帧，且允许最后一个业务帧跨包续接。
- 若流中出现伪同步字节、`DLC > 64`、`ProtocolVersion` 不匹配或控制帧长度非法，Host/Device 都必须做重同步并丢弃该坏帧，禁止把坏帧继续注入 CAN 通道。
- 当前协议不做分片；单帧最大长度固定为 `8 + 64 = 72` 字节。
- 若后续增加时间戳，应优先通过控制命令协商版本，而不是直接改普通帧头。

### 1.4.5 队列与丢包策略（2026-03-11 冻结）
- Device 端必须把 `控制回包` 与 `总线数据上报` 分离排队；控制回包优先发送，避免在四通道大流量下被监控数据饿死。
- Host -> Device 普通数据帧进入“每通道独立发送队列”；若队列已满，新帧丢弃，并累计该通道 `HostToCanDropCount`。
- Device -> Host 的总线数据上报走独立 USB 数据队列；若队列已满，或 USB 当前未配置/断开，则新上报帧直接丢弃，并累计该通道 `DeviceToHostDropCount`。
- USB 断链期间不缓存历史总线流量，避免旧数据在重连后集中喷发。
- 非法 Host 协议帧不回灌到 CAN 总线；当前实现以“丢弃并累计统计”为准，统计值通过 `GetRuntimeStatus(0x05)` 暴露。

### 1.4.4 超时与重试建议
- 普通数据帧默认不做应用层 ACK。
- 控制命令帧建议由 Host 做 `200~500 ms` 超时等待。
- 若无回执，可重发 1 次；连续失败则提示设备未实现或链路异常。

## 2. 错误类型编码（ErrorCode）
当前上位机解析约定：
- `0x1` Bit Error
- `0x2` Stuff Error
- `0x3` CRC Error
- `0x4` Form Error
- `0x5` ACK Error
- `0x6` Bus Off
- `0x7` Error Passive
- `0x8` Arbitration Lost
- `0x0` None

下位机在 CAN 控制器错误中断/状态机中，必须能映射到上述编码并上报。

## 3. 总线监控（Bus Monitor）联调要求
上位机“总线监控”页依赖以下信息：
- 通道号（0~3）
- 方向（TX/RX）
- 帧类型（CAN/CAN FD）
- ID/DLC/Data
- 错误状态（OK/ERROR）
- 错误类型（如 ACK Error / Bus Off）

下位机注意：
- RX 数据必须带 `bit1=0`。
- Host 下发后，若下位机回显发送事件，需带 `bit1=1`。
- 错误事件需置 `bit2=1` 并填写 `ErrorCode`。

### 3.1 当前固件实现约束（2026-03-08 更新）
- 不再使用“自定义状态帧”（如旧版 `0x80` 状态标记）上报驱动状态，避免污染标准协议解析。
- TX 成功时，上报同 ID 的 TX 回显帧（`bit1=1`，`bit2=0`）。
- TX 失败时，上报错误帧（`bit1=1`，`bit2=1`，`ErrorCode` 当前使用 `0x8` 作为通用 TX 路径失败指示）。
- 监控统计（USB/CAN 队列丢包等）仅走串口日志 `PRINTF`，不混入总线数据协议。

## 3.2 分层开发建议（Vendor 与协议分离）
- `usb_vendor_bulk.*` 只负责 USB 端点收发与缓冲，不解析 CAN 协议字段。
- `can_bridge.*` 只负责 `0xA5` 协议编解码和 Flags 规范化。
- `usb_can_bridge.*` 负责把 USB 收发、协议解析、FreeRTOS 队列调度串起来。
- `rtos_app.*` 负责系统启动、任务创建和状态监控，不再直接处理协议细节。
- 后续扩展（加协议版本、命令字）优先改 `can_bridge`，避免在 USB 驱动层做业务判断。

## 4. DBC 与非 DBC 数据并行
- “变量监控”页只对 DBC 已定义信号解码绘图。
- “总线监控”页必须显示所有总线帧（包括 DBC 未定义 ID、错误帧）。

下位机不要只上报 DBC 内消息，原始总线数据也要可观测。

## 5. 四通道并行开发约束
- 每个通道配置独立（CAN/CAN FD、速率、采样点、终端电阻）并可单独下发。
- 同通道帧格式必须一致（由通道配置统一约束）。
- 上报时不得混淆通道号；多通道同 ID 帧必须保留来源通道。
- 建议后续补充“通道能力查询”，让 Host 能知道某通道是否支持 CAN FD、最大数据域速率、是否支持终端电阻控制。

## 6. USB 真实链路开发注意
当前上位机已支持 mock 仿真；后续接真机时请保持协议不变。
推荐步骤：
1. 固件先跑通 Vendor Bulk 收发。
2. 确认 Host 下发帧可进入驱动队列并按通道发送。
3. 增加 RX/错误上报路径，补齐 Flags。
4. 最后再接入自动枚举（VID/PID）。

### 6.2 分阶段开发状态（2026-03-11 更新）
- 阶段 1 `基础枚举`：设备描述符/配置描述符/字符串描述符已能被 Windows 读取，设备管理器可显示 `RT1062 Bulk CAN`。
- 阶段 2 `WinUSB 绑定`：设备端开始提供 Microsoft OS 描述符，用于让 Windows 把该 Vendor/Bulk 设备自动绑定到 `WinUSB`，避免手工 Zadig。
- 阶段 3 `协议适配`：在 WinUSB 成功绑定后，再按本文件 `0xA5` 帧格式接通 Host_PC 收发。
- 阶段 4 `通道配置`：协议已支持 `采样点/终端电阻/通道开关` 下发与回执；其中终端电阻与通道使能已落地到 GPIO/收发器控制。对于尚未实时切换到底层驱动的参数，设备必须返回 `0x02`，且回读值保持为“实际运行配置”。
- 当前主机侧有两条调试入口：
  - `EverVance` UI：主路径，内置 WinUSB 传输层
  - `Host_PC/scripts/host_main.ps1`：命令行路径，支持 `mock / serial / winusb`

### 6.1 当前 Host 传输端点约定
- `mock://localhost?period=200`：模拟设备（无硬件可用）
- `winusb://auto`：WinUSB 自动匹配设备
- `winusb://vid=0x1FC9&pid=0x0135`：按 VID/PID 匹配 WinUSB 设备

说明：
- WinUSB 模式要求设备端按统一 `0xA5` 帧格式收发。
- USB Vendor/Bulk 默认不枚举 COM 口，需 WinUSB 驱动绑定。
- 当前设备端优先提供 Microsoft OS 1.0 `WINUSB` Compatible ID 与 `DeviceInterfaceGUID` 扩展属性，目标是在 Windows 上自动加载 `WinUSB`。

### 6.3 首次建链留痕要求（2026-03-11 更新）
- 新电脑首次接入设备时，必须把“是否自动绑定 WinUSB”作为单独检查项记录下来。
- 不能只看设备是否枚举为 `RT1062 Bulk CAN`；还必须确认 Host_PC 能实际打开 WinUSB 并完成至少一次控制命令往返。
- 若出现 `Code 28 / CM_PROB_FAILED_INSTALL`，说明 Windows 没有为当前 `VID/PID[/REV]` 建立可用绑定；此时 Host_PC 不具备稳定通信前提。
- 若临时使用 `Zadig`、测试签名模式或其他研发临时手段恢复链路，必须在工作留痕中写明：
- 电脑名/操作者
- 使用的驱动方案
- 对应设备 `VID/PID/REV`
- 是否需要管理员权限
- 该方案是否允许用于量产
- 当前项目的量产目标不应依赖“未签名 INF + 人工安装”。
- 设备端若变更 `bcdDevice`、`VID/PID`、`DeviceInterfaceGUID(s)`、Microsoft OS 描述符版本，需视为“会影响首次建链”的变更，必须补做新电脑验证。

## 7. 回归测试清单（每次协议改动后执行）
1. 正常 RX：四通道各至少 1 个 ID，Host 总线监控显示 RX。
2. 正常 TX：Host 发送帧后总线监控显示 TX。
3. CAN/CAN FD：bit0 与实际帧类型一致。
4. 错误上报：至少验证 ACK Error 与 Bus Off。
5. DBC 变量：能在变量监控页正确解码。
6. 非 DBC ID：仅在总线监控页显示，不应导致程序异常。
7. 自动下发通道配置：连接成功后，4 个通道配置均应按协议自动同步。
8. 经典 CAN 与 CAN FD 的采样点防呆：CAN 只有一个采样点，CAN FD 有两个，错误输入必须被拦截。

## 8. 工程文件（.evproj）与协议联动项
- 上位机工程文件会保存：地址栏、DBC 路径、通道配置、发送帧配置、监控变量勾选状态。
- 通道配置新增保存字段：`NominalSamplePreset`、`NominalSamplePointText`、`DataSamplePreset`、`DataSamplePointText`、`TerminationEnabled`。
- 下位机协议版本升级时，若影响上述配置含义，需在工程加载流程加入兼容处理。
- 建议后续在协议头中加入 `ProtocolVersion` 字段，并在工程文件中同步记录版本号。

## 10. 建议在开发前先冻结的协议项
如果本项目要先把协议做完整，再并行开发 Host/Device，建议本轮先把下列事项定死：

1. 普通数据帧头保持 `0xA5 + Channel + DLC + Flags + ID + Data` 不再改。
2. `Flags.bit3` 永久保留给控制命令帧，不再挪作他用。
3. 控制命令统一使用 `Byte4~7 = Command / Status / Sequence / ProtocolVersion`。
4. `SetChannelConfig(0x01)` 的 `16` 字节载荷固定，不再增减字段；若以后扩展，走 `PayloadVersion`。
5. 经典 CAN 与 CAN FD 的采样点字段都用 `permille(0.1%)`，避免字符串协议。
6. 设备信息、通道能力、运行状态、心跳这 4 类命令号先冻结，即使暂未实现。
7. 后续若要加时间戳、统计信息、设备序列号，不要塞进普通数据帧，统一走控制命令。

## 11. 当前未实现但应提前考虑的协议能力
以下内容现在可以不做，但建议在设计上预留：

- `DeviceInfo`：固件版本、协议版本、VID/PID、设备名称
- `ChannelCapabilities`：每个通道是否支持 CAN FD、最大数据域波特率、是否支持终端电阻控制
- `RuntimeStatus`：Bus Off、Error Passive、USB 配置状态、队列丢包计数
- `Heartbeat`：判断链路是否在线，后续可用于 UI 连接状态更稳地刷新
- `GetChannelConfig`：让 Host 在重新连接后可从设备读回实际生效配置
- `ConfigApplyMode`：未来如果某些驱动改参数需要重新初始化，可通过状态码区分“立即生效”和“需重启通道”

## 9. 模拟链路流量控制（联调保护）
- Mock 链路默认节拍为 `100ms`，并采用轮询单帧输出，避免一次注入过多数据。
- 可通过地址栏配置节拍：`mock://localhost?period=200`（单位 ms，范围 20~2000）。
- 当联调下位机桥接链路时，先使用 `period>=100`，确认 MCU 负载后再逐步提速。
