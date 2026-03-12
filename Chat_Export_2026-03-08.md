# VBA_CAN 今日会话导出

- 导出日期: 2026-03-08
- 会话主题: VBA_CAN 固件 + EverVance 上位机联调开发
- 参与方: User / Codex

## 1. 目标与阶段

1. 初期: CMake 构建修复、Git/Ninja 环境联调、RT1062 宏定义与工程可编译。
2. 中期: 下位机 USB Vendor Bulk + CAN/CAN FD 基础驱动分层、RTOS 任务架构。
3. 中后期: EverVance 上位机从基础功能扩展到工程化 UI、多通道、监控、日志、项目保存。
4. 后期: UI 细节修复（主题、白边、布局、快捷键、状态灯、工作区流程等）。
5. 当前收尾: 回到 CAN_APP，按上位机协议完成 Vendor 驱动层与协议层分离适配。

## 2. 今日主要结论

- USB 通信链路采用 Vendor Bulk + WinUSB，不走 CDC/COM。
- Host/Device 统一使用 `0xA5` 帧协议:
  - Byte0 Sync
  - Byte1 Channel
  - Byte2 DLC
  - Byte3 Flags
  - Byte4~7 ID (LE)
  - Byte8.. Data
- Flags 语义对齐:
  - bit0 CANFD
  - bit1 TX/RX(Host视角)
  - bit2 Error
  - bit7..bit4 ErrorCode
- 下位机实现采用分层:
  - `usb_vendor_bulk.*` 仅做USB驱动收发
  - `can_bridge.*` 仅做协议编解码
  - `rtos_app.*` 负责业务调度

## 3. 上位机（EverVance）关键结果

- 支持工程概念与 `.evproj` 保存/加载（通道、发送帧、监控配置等）。
- 支持 WinUSB 预设与 Mock 联调。
- 支持总线监控、变量监控、变量存储、日志导入导出、查找筛选。
- 启动流程改为 Workspace 引导弹窗:
  - 显示上次路径
  - 历史路径下拉
  - 打开/关闭按钮
  - Enter 默认触发“打开”
- 顶部菜单新增“帮助/关于”，并可打开 README/docs。
- 窗体图标与左侧丝印按 EverVance 视觉调整（后续可继续美术微调）。

## 4. 固件（CAN_APP）本轮关键改动

### 4.1 Vendor 驱动层
- 文件:
  - `CAN_APP/source/usb_vendor_bulk.h`
  - `CAN_APP/source/usb_vendor_bulk.c`
- 重点:
  - 去除 OUT 回调中的“原样回显”逻辑
  - 新增统计结构 `usb_vendor_bulk_stats_t`
  - 新增 `USB_VendorBulkIsConfigured()`
  - 新增 `USB_VendorBulkGetStats()`

### 4.2 协议层
- 文件:
  - `CAN_APP/source/can_bridge.h`
  - `CAN_APP/source/can_bridge.c`
- 重点:
  - 新增 Flags 常量（CANFD/TX/ERROR/ErrorCode）
  - 新增 `CAN_BridgeNormalizeHostTx()`
  - 新增 `CAN_BridgeBuildTxEcho()`
  - 新增 `CAN_BridgeBuildRxUplink()`
  - 新增 `CAN_BridgeBuildError()`

### 4.3 RTOS 调度层
- 文件:
  - `CAN_APP/source/rtos_app.c`
- 重点:
  - USB 下行包先 Decode+Normalize 再入 CAN 队列
  - TX 成功上报 TX 回显帧
  - TX 失败上报错误帧（当前使用 `ErrorCode=0x8` 表示通用 TX 路径失败）
  - RX 帧统一按 RX 语义上报
  - 统计类信息改为串口日志输出，不混入协议帧

### 4.4 USB PID 对齐
- 文件:
  - `CAN_APP/source/usb_vendor_bulk_desc.c`
- 改动:
  - PID 改为 `0x0135`，与上位机 WinUSB 预设一致

### 4.5 协议文档更新
- 文件:
  - `COMM_PROTOCOL_NOTES.md`
- 增补:
  - 当前固件实现约束
  - Vendor/协议分层开发建议

## 5. 当前编译状态

- 命令: `cmake --build --preset arm-debug`
- 结果: `can_app.elf` 链接成功。
- 备注: 存在 `nosys` 常见 `_read/_write/_lseek/_close` 警告，不阻塞当前构建。

## 6. 建议下一步

1. 上板联调 WinUSB：验证 TX 回显与 RX 上报路径。
2. 将 CAN 硬件真实错误映射到 ErrorCode(1~8)，替换当前通用 `0x8` 失败码。
3. 协议后续若加版本字段，优先在 `can_bridge.*` 扩展并同步 `COMM_PROTOCOL_NOTES.md`。
