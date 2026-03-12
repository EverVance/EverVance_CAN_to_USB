# EverVance 上位机 UI

## 功能覆盖
- 导入 DBC（`BO_` / `SG_`）
- 发送 CAN 帧（通道选择、CAN/CAN FD 类型、bitrate 参数输入）
- 解析变量并实时绘图
- 变量存储（带时间戳）
- 导出 CSV

## 运行
1. 双击 `run_evervance.bat`
2. 首次会自动编译生成 `bin\EverVance.exe`
3. 后续可直接双击 `bin\EverVance.exe`

## 可移植
- 拷贝整个 `EverVance` 文件夹即可。
- 依赖：Windows 自带 .NET Framework 4.x（多数公司电脑默认具备）。

## 备注
- 当前传输层默认 `Mock` 回环。
- WinUSB 直连 Vendor Bulk 将在下一步接入（不影响 UI/协议主流程）。
