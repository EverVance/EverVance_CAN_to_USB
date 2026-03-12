# VBA_CAN 今日会话导出

- 导出日期: 2026-03-10
- 会话主题: RT1062 Bootloader XIP 启动链 + CAN_APP LED 上板确认
- 参与方: User / Codex

## 1. 今日目标

1. 重新阅读工作区上下文，确认当前工程为 `Bootloader + CAN_APP` 双镜像结构。
2. 使用 `LinkServer` 和自定义 `CFX` 将 `Bootloader`、`CAN_APP` 烧录到外部 QSPI Flash。
3. 依据硬件网表点亮 `D201/D202/D203`，验证 `Bootloader -> CAN_APP` 启动链。
4. 排查外部 Flash 首启失败、第一页刷写异常、XIP 配置与地址映射问题。
5. 对今天的构建、烧录、调试和结论做工作留痕。

## 2. 今日主要结论

- `LinkServer` 默认 `MIMXRT1060_SFDP_QSPI.cfx` 在本板上会卡在第一页刷写，自定义 `CFX` 可稳定完成外部 Flash 擦写和校验。
- `Bootloader` 的镜像头布局已经对齐标准 XIP 结构:
  - `FCB  = 0x60000000`
  - `IVT  = 0x60001000`
  - `VEC  = 0x60002000`
  - `TEXT = 0x60002400`
- `Bootloader -> CAN_APP` 跳转地址已修正为从 `0x60022000` 读取 APP 向量表，不再从 `0x60020000` 误取 FCB。
- `CAN_APP` 点灯逻辑已按网表修正为共阳极、IO 下拉点亮，同时补了 `VCC_SYS_3V3` 使能时序。
- 当前阻塞点不在 APP 地址映射，而在更前面的 `BootROM -> Bootloader XIP`：
  - 复位后 CPU `PC` 持续停在 `0x0020E35x / 0x0020ED48`
  - `VTOR = 0x00200000`
  - 芯片未从外部 QSPI `0x6000_0000` 启动
- 今日已将 `Bootloader` 的 FCB 从激进配置收敛到保守配置，但芯片仍未完成外部 Flash 冷启动，说明问题继续收敛到 `BootROM 锁存的 BOOT_CFG / XIP 头匹配关系`。

## 3. 烧录链路与工具结论

- 可稳定工作的外部 Flash 烧录配置:
  - `tools/linkserver/VBA_CAN_RT1062_MX25L3233_customcfx_safe.json`
  - `tools/linkserver/RT1062_MX25L3233_4K.cfx`
- 该组合可完成:
  - `Bootloader` 擦写到 `0x60000000`
  - `CAN_APP` 擦写到 `0x60020000`
  - `verify` 校验
- 调试链路仍存在偶发性 `Ee(42) Could not connect to core` 或探头瞬时断连，通常通过重新上电/复位后恢复。
- `LinkServer` 调试侧 `wirespeed` 已从 `1 MHz` 提到 `4 MHz`，以改善烧录与连接效率。
- 用户提出“烧录时钟可提到 `60 MHz`”后，当前未直接把 SWD 时钟提到该量级；本轮优先保守验证 `Bootloader XIP`，待启动链稳定后再评估更高频率。

## 4. Bootloader 侧关键修改

### 4.1 APP 跳转修正
- 文件:
  - `Bootloader/startup/startup_mimxrt1062.c`
  - `Bootloader/source/Bootloader.c`
- 关键改动:
  - APP 基地址仍为 `0x60020000`
  - APP 向量表改为 `0x60022000`
  - 跳转前关闭中断与 `SysTick`
  - 设置 `SCB->VTOR = 0x60022000`
  - `Bootloader` 启动流程改为先尝试 `JumpToApp()`，再决定是否留在 Bootloader

### 4.2 XIP FCB 收敛到保守配置
- 文件:
  - `Bootloader/xip/evkbmimxrt1060_flexspi_nor_config.c`
- 调整内容:
  - `readSampleClkSrc`:
    - `kFlexSPIReadSampleClk_LoopbackFromDqsPad`
    - 改为 `kFlexSPIReadSampleClk_LoopbackInternally`
  - `serialClkFreq`:
    - `kFlexSpiSerialClk_100MHz`
    - 改为 `kFlexSpiSerialClk_50MHz`
  - Read LUT:
    - 从 `0xEB` 的 `1-4-4` 读模式
    - 改为 `0x6B` 的 `1-1-4` Fast Read Quad
  - Dummy cycles:
    - `0x06`
    - 改为 `0x08`
- 重新构建后，`objdump/readelf` 已确认 FCB 生效:
  - `readSampleClkSrc = 0`
  - `serialClkFreq = 0x02`
  - Read LUT 起始字为 `0x6B`

## 5. CAN_APP 侧关键修改

### 5.1 APP 最小确认版入口
- 文件:
  - `CAN_APP/source/CAN_APP.c`
- 改动:
  - 暂时收敛为板级初始化 + LED 点亮 + 空循环
  - 先不走原本完整的 FreeRTOS 业务链，目的是先确认 APP 能跑起来
- 备注:
  - 用户已提醒后续修改不要忘记 RTOS 结构，后续恢复任务化时需要回到 `FreeRTOS` 入口

### 5.2 LED GPIO 与共享 GPIO 选择器修正
- 文件:
  - `CAN_APP/source/bsp_peripherals.c`
  - `CAN_APP/source/bsp_peripherals.h`
- 依据网表确认:
  - `D201/D202/D203` 为共阳极
  - `VCC_SYS_3V3` 供电
  - IO 控制阴极，低电平点亮
- 已补充:
  - `GPIO2/GPIO7` 共享选择器 `IOMUXC_GPR->GPR27/GPR28`
  - LED 对应 6 路 GPIO 输出配置
  - `BSP_SetStatusLeds(true)` 采用 active-low 方式拉低

### 5.3 SYS_3V3 使能时序
- 文件:
  - `CAN_APP/source/bsp_peripherals.c`
- 用户补充网表约束:
  - `VCC_SYS_3V3` 不是上电即有
  - 必须先拉高 `A8_VCC_SYS_3V3_EN`
  - 对应 `GPIO_B0_06`
- 已实现:
  - `BSP_EnableSys3v3Rail()`
  - 拉高 `GPIO2_IO06`
  - 延时 `5 ms`
  - 再初始化 LED 相关 GPIO

## 6. 构建与烧录记录

- `Bootloader` 构建:
  - 命令: `cmake --build --preset arm-debug --target bootloader.elf --clean-first -j 8`
  - 结果: 成功
- `CAN_APP` 构建:
  - 命令: `cmake --build --preset arm-debug --target can_app.elf -j 8`
  - 结果: 成功
- `Bootloader` 烧录:
  - 地址: `0x60000000`
  - 长度: `0xBB98` (`48024` bytes)
  - 结果: 写入成功，复位完成
- `CAN_APP` 烧录:
  - 地址: `0x60020000`
  - 结果: 可写入并校验
- 备注:
  - 编译存在 `nosys` 相关 `_read/_write/_lseek/_close` 警告
  - 不阻塞当前固件构建

## 7. 启动链调试结果

- 使用脚本:
  - `tools/linkserver/sample_boot_pc.scp`
  - `tools/linkserver/peek_boot_state.scp`
- 关键观测:
  - 复位后 `PC = 0x0020E35E`
  - 放跑后 `PC = 0x0020ED48`
  - `VTOR = 0x00200000`
  - `GPIO2_GDIR = 0x00000000`
- 结论:
  - CPU 仍停留在片上 `BootROM`
  - `Bootloader` 没有从外部 Flash 执行

### 7.1 外部 Flash 窗口观测
- 在未成功 XIP 的状态下读取:
  - `0x60000000 -> 0x42464346`
  - `0x60001000 -> 0x42464346`
  - `0x60002000 -> 0x42464346`
  - `0x60022000 -> 0x42464346`
  - `0x60022004 -> 0x56010400`
- 解释:
  - AHB 侧外部 Flash 映射未处于正常可执行状态
  - `0x42464346 / 0x56010400` 分别对应 FCB Tag / Version
  - 这进一步说明当前卡点仍在 `BootROM 对外部 Flash 的首启识别`

### 7.2 启动模式锁存值
- 通过真实 `SRC` 基地址读取:
  - `SRC_SBMR1 = 0x000000C0`
  - `SRC_SBMR2 = 0x02000009`
- 含义:
  - 芯片不是落在单纯“调试器/串口下载器模式”这种显性异常状态
  - 当前更像是内部启动流程已经尝试执行，但没有成功从本板外部 QSPI 进入 XIP

## 8. 硬件网表确认项

- 外部 Flash 硬件连接:
  - `FLEXSPI_CLK`
  - `FLEXSPI_CS0`
  - `FLEXSPI_D0`
  - `FLEXSPI_D1`
  - `FLEXSPI_D2`
  - `FLEXSPI_D3`
- LED 供电与控制:
  - `D201/D202/D203` 由 `VCC_SYS_3V3` 供电
  - `A13/A11/B9/B13/D12/E12` 控制 LED 阴极
  - `A8_VCC_SYS_3V3_EN` 需要先置高

## 9. 当前判断

1. `CAN_APP` 点灯未成功的直接原因不是 LED 极性，而是芯片尚未正常从外部 Flash 启动到 `Bootloader` / `APP`。
2. `Bootloader` 的链接地址和 APP 跳转地址问题已经基本排清。
3. `Bootloader` 的 FCB 已从激进配置调到保守配置，但仍不足以让 BootROM 成功完成首启。
4. 当前最值得继续收敛的方向是:
   - `BOOT_CFG` 锁存值与实际外部 Flash 启动模式是否匹配
   - BootROM 对这颗 `MX25L3233` 的首启读模式/LUT/实例选择要求
   - 是否还存在用户之前修改过但未回收的 XIP 相关配置

## 10. 建议下一步

1. 继续核对 `BOOT_CFG[0..11]` 硬件上下拉与 `SRC_SBMR1/SBMR2` 的对应关系，确认 BootROM 选择的确实是当前 `FLEXSPI A1 / QSPI NOR`。
2. 对照用户“之前改过 XIP 并降过 QSPI 速率”的历史修改，追溯 Bootloader FCB 是否还缺一项关键配置。
3. 若需要，可继续在保守条件下尝试更接近 NXP 标准模板的 `0xEB 1-4-4` 方案与当前 `0x6B 1-1-4` 方案做 A/B 验证。
4. 在 `Bootloader` 真正从外部 Flash 起起来之前，不继续扩大 `CAN_APP` 业务修改范围，避免把启动链问题和应用层问题混在一起。
