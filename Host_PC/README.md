# VBA_CAN 上位机（Host_PC）

## 推荐使用方式（UI）
- 双击 [run_evervance.bat](f:/AS/NXP_Workspace/VBA_CAN/Host_PC/run_evervance.bat)
- 或直接双击 [EverVance.exe](f:/AS/NXP_Workspace/VBA_CAN/Host_PC/EverVance/bin/EverVance.exe)
- 启动后会先弹出 WorkSpace 目录选择窗口，并记住上次路径。

`EverVance` 是独立桌面 UI（不是命令行），包含：
- DBC 导入
- CAN/CAN FD 参数配置（通道、bitrate、采样点、终端电阻、帧格式）
- 变量解析与绘图
- 总线监控（四通道、TX/RX 方向、错误状态/错误类型）
- 总线记录持久化（每个工程自动生成 `.buslog.csv`，重开可回看）
- 工程保存/加载（`.evproj`，可恢复 DBC/通道/发送帧/监控变量）
- 带时间戳的数据存储与 CSV 导出

## 兼容性与依赖
- 当前 UI 基于 Windows 自带 .NET Framework WinForms（大多数公司电脑可直接运行）。
- 整个 `Host_PC` 文件夹可直接拷贝到其他电脑使用。

## 目录说明
- `EverVance/`：桌面 UI 工程与可执行文件
- `run_evervance.bat`：UI 一键启动
- `scripts/` 与 `run_host.bat`：命令行调试工具（可选，现已支持 `WinUSB` 真机）

## 说明
- 目前固件侧是 USB Vendor Bulk，真实硬件通信已接入 WinUSB 传输层。
- 现在可以先用 UI + Mock 流程联调交互与显示逻辑。
- 协议与下位机联调注意事项见仓库根目录 `COMM_PROTOCOL_NOTES.md`。
- 通道参数会在每次连接成功后自动同步到设备，不再提供手工逐条下发入口。
- 常用快捷键：`Ctrl+N` 新建工程、`Ctrl+O` 打开工程、`Ctrl+S` 保存工程、`Ctrl+Shift+S` 另存工程、`Ctrl+F` 变量筛选、`F5` 连接。
- `Ctrl+F` 按当前窗口就地查找：变量监控=信号筛选、总线监控=报文筛选、变量存储=记录筛选。
- 各窗口查找都支持“字段可选”：
  - 变量监控：全部/消息名/信号名
  - 总线监控：全部/时间/方向/类型/ID/DLC/数据/状态/错误类型/通道
  - 变量存储：全部/时间戳/通道/消息名/信号名/数值
- Mock 速率可配置：地址栏示例 `mock://localhost?period=200`（ms）。
- 设备切换方式（地址栏）：
  - 虚拟设备：`mock://localhost?period=200`
  - WinUSB 真机：`winusb://auto` 或 `winusb://vid=0x1FC9&pid=0x0135`
  - 点击“连接”后会按地址自动选择传输层。
- 地址栏旁有“预设”下拉，可直接选择 Mock 或 WinUSB 预设。
- 命令行调试可运行 `run_host.bat`，选择 `winusb` 后输入 `winusb://auto` 或 `winusb://vid=0x1FC9&pid=0x0135`。

## 设备识别说明
- Mock 在其他同事电脑通常可直接使用（无需硬件）。
- USB Vendor/Bulk 不会枚举成 COM，需要 WinUSB 驱动（如 Zadig 绑定 WinUSB）。
- 若固件改成 USB CDC-ACM 才会枚举为 COM 口。

## 首次建链与驱动策略
- 目标设备是 `USB\VID_1FC9&PID_0135`，正常情况下应显示为 `RT1062 Bulk CAN`。
- 对一台新电脑而言，“首次建立 Link”本质上是 Windows 首次为该 `VID/PID[/REV]` 建立驱动绑定与接口类登记。
- 若设备管理器显示 `Code 28 / Failed Install`，说明当前机器没有为该设备建立可用驱动绑定，Host_PC 无法稳定打开 WinUSB。

### 推荐优先级
- `量产/长期维护推荐`：优先依赖设备端 `Microsoft OS Descriptors + WinUSB` 自动绑定，不把“手工装驱动”作为量产默认步骤。
- `次优方案`：提供 `已签名` 的驱动包（INF/CAT），首次接入新电脑时由管理员安装一次。
- `仅研发临时方案`：`Zadig` 或测试签名模式。可用于实验室快速恢复，不建议作为量产 SOP。

### 为什么不建议依赖未签名 INF
- 未签名 INF 在多数公司电脑上需要管理员权限，且经常被组策略或驱动签名策略拦截。
- 这会把“首次建链”变成一件不可预测的人工操作，不利于批量部署和售后维护。
- 一旦 `bcdDevice` 或 USB 描述符版本变化，Windows 可能把设备当成新实例重新枚举；若驱动包不可自动安装，就会再次掉进 `Code 28`。

### 当前工程约束
- Host_PC 的 WinUSB 传输层依赖稳定的 `VID/PID` 与接口路径。
- 设备端若调整 `DeviceInterfaceGUID(s)`、`Compatible ID`、`bcdDevice`，必须同步记录，并在新电脑上重新验证“首次建链”。
- 若只是普通应用版本迭代，尽量不要随意变更 `VID/PID`、接口 GUID、USB 修订号，以免触发 Windows 重建驱动绑定。

### 首次建链检查清单
1. 连接设备后，确认设备管理器中出现 `RT1062 Bulk CAN`。
2. 若状态为 `OK`，再使用 `EverVance` 的 `winusb://vid=0x1FC9&pid=0x0135` 验证连接。
3. 若状态为 `Code 28`，优先安装 `已签名` 驱动包；不要默认继续使用未签名 INF。
4. 若当前阶段只能临时恢复研发环境，可使用 `Zadig` 绑定 `WinUSB`，但必须在留痕文档里注明该电脑使用的是临时方案。
5. 建链通过后，再验证 `GetDeviceInfo / Heartbeat` 等真实收发，而不是只看设备是否出现在设备管理器中。
