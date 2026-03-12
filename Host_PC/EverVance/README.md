# EverVance 上位机 UI

## 功能覆盖
- 导入 DBC（`BO_` / `SG_`）
- 发送 CAN 帧（按通道配置约束 CAN/CAN FD 类型）
- 通道参数配置（bitrate、采样点、终端电阻），连接后自动同步
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

## 备注890
- 当前传输层默认 `Mock` 回环。
- WinUSB 直连 Vendor Bulk 已接入。
- 协议主说明见仓库根目录 `COMM_PROTOCOL_NOTES.md`。
