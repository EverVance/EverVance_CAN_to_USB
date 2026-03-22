#ifndef TJA1042_DRV_H
#define TJA1042_DRV_H

/* 文件说明：
 * 本头文件封装板上 4 路 TJA1042 收发器的 STB 控制与 BusOff 恢复状态机。
 * 它只负责“物理收发器模式”，不直接处理 CAN 协议层收发。 */

#include <stdbool.h>
#include <stdint.h>

/* TJA1042 通道编号，与硬件网表中的 4 路收发器一一对应。 */
typedef enum
{
    kTja1042_CanFd1 = 0, /* U301 */
    kTja1042_CanFd2,     /* U304 */
    kTja1042_Can1,       /* U401 */
    kTja1042_Can2,       /* U403 */
    kTja1042_Count
} tja1042_channel_t;

/* TJA1042 物理层模式：芯片本身仅支持 Normal / Standby。 */
typedef enum
{
    kTja1042Mode_Normal = 0,
    kTja1042Mode_Standby
} tja1042_mode_t;

typedef struct
{
    tja1042_mode_t requestedMode;
    tja1042_mode_t appliedMode;
    uint8_t ready;
    uint8_t busOffRecoveryActive;
    uint8_t reserved0;
    uint8_t reserved1;
    uint32_t transitionCount;
    uint32_t lastModeChangeTickMs;
    uint32_t busOffRecoverAtTickMs;
    uint32_t busOffRecoveryDelayMs;
    uint8_t busOffRecoveryCount;
    uint8_t reserved2;
    uint8_t reserved3;
    uint8_t reserved4;
} tja1042_status_t;

/** 初始化收发器控制层（依赖 BSP 已完成 GPIO 复用与输出配置）。 */
bool TJA1042_Init(void);

/** 设置指定收发器通道的 STB 模式。 */
bool TJA1042_SetMode(tja1042_channel_t channel, tja1042_mode_t mode);

/** 读取指定通道当前缓存的工作模式。 */
bool TJA1042_GetMode(tja1042_channel_t channel, tja1042_mode_t *mode);
/** 周期推进 BusOff 恢复状态机。 */
void TJA1042_Task(uint32_t tickMs);
/** 通知收发器层当前总线是否进入 BusOff。 */
bool TJA1042_NotifyBusState(tja1042_channel_t channel, bool busOff);
/** 获取指定收发器通道的状态快照。 */
bool TJA1042_GetStatus(tja1042_channel_t channel, tja1042_status_t *status);

#endif
