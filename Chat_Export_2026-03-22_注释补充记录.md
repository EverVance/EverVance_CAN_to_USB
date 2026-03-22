# 2026-03-22 注释补充记录

## 目的

本记录用于补充今天在设备端代码中新增的中文维护注释，便于后续人工接手时快速理解：

- 每个模块负责什么
- 每个函数为什么存在
- 哪些地方属于任务上下文，哪些属于回调/中断语义
- 哪些位置是历史故障点，后续出现类似现象时应优先检查哪里

## 本次补充原则

这次不是简单做“逐行翻译式注释”，而是按维护视角补齐下面几类信息：

- 文件级职责说明
- 头文件引用原因
- 关键静态变量和上下文缓存的用途
- 函数声明前的中文说明
- 关键流程内部的维护说明
- 与 CAN/USB/RTOS 分层边界相关的注意事项

## 已补充注释的设备端文件

- `CAN_APP/source/CAN_APP.c`
- `CAN_APP/source/bsp_peripherals.c`
- `CAN_APP/source/bsp_peripherals.h`
- `CAN_APP/source/can_bridge.c`
- `CAN_APP/source/can_bridge.h`
- `CAN_APP/source/can_internal_onchip.c`
- `CAN_APP/source/can_internal_onchip.h`
- `CAN_APP/source/can_stack.c`
- `CAN_APP/source/can_stack.h`
- `CAN_APP/source/can_types.h`
- `CAN_APP/source/canfd1_ext_spi.c`
- `CAN_APP/source/canfd1_ext_spi.h`
- `CAN_APP/source/lpspi1_bus.c`
- `CAN_APP/source/lpspi1_bus.h`
- `CAN_APP/source/rtos_app.c`
- `CAN_APP/source/rtos_app.h`
- `CAN_APP/source/tja1042_drv.c`
- `CAN_APP/source/tja1042_drv.h`
- `CAN_APP/source/usb_can_bridge.c`
- `CAN_APP/source/usb_can_bridge.h`
- `CAN_APP/source/usb_vendor_bulk.c`
- `CAN_APP/source/usb_vendor_bulk.h`
- `CAN_APP/source/usb_vendor_bulk_desc.c`
- `CAN_APP/source/usb_vendor_bulk_desc.h`

## 本次重点补充的三大模块

### 1. `canfd1_ext_spi.c`

这一层是 CH0 外置 MCP2517FD 的主驱动实现。本次新增注释重点说明了：

- TXQ / RX FIFO / TEF 各自的作用
- 为什么错误诊断优先看 `BDIAG1/TREC/TXQSTA`
- 为什么当前 TX 对象大小必须按激活 payload 大小计算
- 哪些函数是主路径，哪些只是保留作历史对照
- 哪些函数只允许在任务上下文里调用，避免和 SPI 访问冲突

### 2. `can_internal_onchip.c`

这一层是 CH1~CH3 的片上 FlexCAN 适配层。本次新增注释重点说明了：

- 上下文数组为什么索引 0 为空
- 经典 CAN / CAN FD 位时序与采样点如何换算
- `OnchipPackWord/OnchipUnpackWords` 为什么是字节序故障的关键点
- 软件接收队列 / 事件队列如何与 SDK 回调配合
- 通道关闭时为什么不再激进 Deinit，而改用“温和禁用”

### 3. `usb_can_bridge.c`

这一层是 Host 与设备之间的桥接协议层。本次新增注释重点说明了：

- Host 活跃状态为什么不能只靠心跳维持
- fake `RX 0x000` 为什么要在这里拦截
- 为什么连接/断开时要刷新桥接层队列
- 控制包与数据包各自怎么编码/解码
- 运行态中的 pending/drop 统计如何产生

## 维护性补充说明

这次注释特别补了下面这些“后续最容易踩坑”的地方：

- `STB` 极性没有写反，`LOW=Normal / HIGH=Standby`
- 当前实物板上 CH2/CH3 的有效 `STB` 控制关系，需要以实测为准而不是只看名义网表顺序
- CH0 的错误分类优先级已经改成“ACK/Bit/Stuff/CRC/Form 优先于 BusOff/ErrorPassive”
- `Error Passive` 只保留为节点状态，不再作为总线错误包上报
- LED 任务只能读缓存状态，不能主动去轮询底层驱动

## 编译验证

注释补充后已重新执行：

```powershell
cmake --build --preset can-app-debug
```

结果：

- `can_app.elf` 编译通过
- 注释未引入新的编译错误
- 仅保留原有 NXP/newlib warning

## 结论

当前设备端的“非 SDK 自研部分”已经补到可维护状态，后续继续开发时：

- 优先先看文件级职责说明
- 再看函数声明前的中文注释
- 出现历史相似故障时，先按注释里的“维护提示/排查入口”去定位
