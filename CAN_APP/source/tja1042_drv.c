#include "tja1042_drv.h"

#include "fsl_gpio.h"

/* 维护提示：
 * - 本文件只负责“GPIO 最终写到哪个收发器的 STB 引脚”。
 * - 逻辑通道 CH0~CH3 到物理收发器 U301/U304/U401/U403 的对应关系，
 *   是由 can_stack.c 中的 CAN_StackMapPhyChannel() 决定的。
 * - 因此排查“关闭某一路却影响另一路”时，必须同时检查：
 *   1. 这里的 GPIO <-> 收发器绑定
 *   2. can_stack.c 的 逻辑通道 <-> 物理收发器 映射
 *   不能只盯其中一层。 */

/* 网表映射:
 * D8_CAN_FD_1_EN -> GPIO2_IO03 -> U301(STB)
 * C8_CAN_FD_2_EN -> GPIO2_IO04 -> U304(STB)
 * B8_CAN_1_EN    -> GPIO2_IO07 -> U401(STB)
 * A9_CAN_2_EN    -> GPIO2_IO05 -> U403(STB)
 *
 * TJA1042 的 STB=0 为 Normal，STB=1 为 Standby。
 */
typedef struct
{
    GPIO_Type *port;
    uint32_t pin;
} tja1042_pin_t;

static const tja1042_pin_t s_TjaPins[kTja1042_Count] = {
    {GPIO2, 3U},
    {GPIO2, 4U},
    {GPIO2, 7U},
    {GPIO2, 5U},
};

static tja1042_mode_t s_ModeCache[kTja1042_Count];
static tja1042_status_t s_Status[kTja1042_Count];
static bool s_Ready;

#define TJA1042_BUS_OFF_RECOVERY_FAST_MS (100U)
#define TJA1042_BUS_OFF_RECOVERY_MEDIUM_MS (250U)
#define TJA1042_BUS_OFF_RECOVERY_SLOW_MS (500U)
#define TJA1042_BUS_OFF_RECOVERY_MAX_MS (1000U)

static uint32_t TJA1042_GetRecoveryDelayMs(uint8_t recoveryCount)
{
    if (recoveryCount <= 1U)
    {
        return TJA1042_BUS_OFF_RECOVERY_FAST_MS;
    }
    if (recoveryCount == 2U)
    {
        return TJA1042_BUS_OFF_RECOVERY_MEDIUM_MS;
    }
    if (recoveryCount == 3U)
    {
        return TJA1042_BUS_OFF_RECOVERY_SLOW_MS;
    }

    return TJA1042_BUS_OFF_RECOVERY_MAX_MS;
}

static bool TJA1042_IsValidChannel(tja1042_channel_t channel)
{
    return ((uint8_t)channel < (uint8_t)kTja1042_Count);
}

bool TJA1042_Init(void)
{
    uint32_t i;

    /* 根据 TJA1042 数据手册：
     * - STB = LOW  -> Normal
     * - STB = HIGH -> Standby
     *
     * 这里上电先全部写成 LOW，不代表最终就让四个通道长期工作，
     * 只是为了让系统先进入一个“物理层可观测”的基础状态。
     * 后续真正的通道启停语义会被 CAN_StackApplyChannelConfig() 再次覆盖。 */
    /* 默认切 Normal，确保实车/台架接线后可直接收发。 */
    for (i = 0U; i < (uint32_t)kTja1042_Count; i++)
    {
        GPIO_PinWrite(s_TjaPins[i].port, s_TjaPins[i].pin, 0U);
        s_ModeCache[i] = kTja1042Mode_Normal;
        s_Status[i].requestedMode = kTja1042Mode_Normal;
        s_Status[i].appliedMode = kTja1042Mode_Normal;
        s_Status[i].ready = 1U;
        s_Status[i].busOffRecoveryActive = 0U;
        s_Status[i].transitionCount = 1U;
        s_Status[i].lastModeChangeTickMs = 0U;
        s_Status[i].busOffRecoverAtTickMs = 0U;
        s_Status[i].busOffRecoveryDelayMs = 0U;
        s_Status[i].busOffRecoveryCount = 0U;
    }

    s_Ready = true;
    return true;
}

bool TJA1042_SetMode(tja1042_channel_t channel, tja1042_mode_t mode)
{
    uint8_t stbLevel;

    if ((!s_Ready) || (!TJA1042_IsValidChannel(channel)))
    {
        return false;
    }

    /* 不要凭感觉把这里改反：
     * 这条极性已经被手册和实机共同确认。
     * 如果后续现象像“STB 写了却影响错总线”，优先怀疑的是逻辑通道到收发器
     * 的映射关系，而不是这里的高低电平定义。 */
    stbLevel = (mode == kTja1042Mode_Standby) ? 1U : 0U;
    GPIO_PinWrite(s_TjaPins[channel].port, s_TjaPins[channel].pin, stbLevel);
    s_ModeCache[channel] = mode;
    s_Status[channel].requestedMode = mode;
    s_Status[channel].appliedMode = mode;
    s_Status[channel].busOffRecoveryActive = 0U;
    s_Status[channel].busOffRecoverAtTickMs = 0U;
    s_Status[channel].busOffRecoveryDelayMs = 0U;
    s_Status[channel].busOffRecoveryCount = 0U;
    s_Status[channel].transitionCount++;
    return true;
}

bool TJA1042_GetMode(tja1042_channel_t channel, tja1042_mode_t *mode)
{
    if ((!s_Ready) || (mode == NULL) || (!TJA1042_IsValidChannel(channel)))
    {
        return false;
    }

    *mode = s_ModeCache[channel];
    return true;
}

void TJA1042_Task(uint32_t tickMs)
{
    uint32_t i;

    if (!s_Ready)
    {
        return;
    }

    for (i = 0U; i < (uint32_t)kTja1042_Count; i++)
    {
        if ((s_Status[i].busOffRecoveryActive != 0U) && (s_Status[i].busOffRecoverAtTickMs == 0U))
        {
            s_Status[i].busOffRecoverAtTickMs = tickMs + s_Status[i].busOffRecoveryDelayMs;
            continue;
        }

        if ((s_Status[i].busOffRecoveryActive != 0U) &&
            ((int32_t)(tickMs - s_Status[i].busOffRecoverAtTickMs) >= 0))
        {
            /* BusOff 恢复完成后重新回到 Normal，也就是 STB 拉低。 */
            GPIO_PinWrite(s_TjaPins[i].port, s_TjaPins[i].pin, 0U);
            s_ModeCache[i] = kTja1042Mode_Normal;
            s_Status[i].requestedMode = kTja1042Mode_Normal;
            s_Status[i].appliedMode = kTja1042Mode_Normal;
            s_Status[i].busOffRecoveryActive = 0U;
            s_Status[i].busOffRecoverAtTickMs = 0U;
            s_Status[i].lastModeChangeTickMs = tickMs;
            s_Status[i].transitionCount++;
        }
    }
}

bool TJA1042_NotifyBusState(tja1042_channel_t channel, bool busOff)
{
    if ((!s_Ready) || (!TJA1042_IsValidChannel(channel)))
    {
        return false;
    }

    if (!busOff)
    {
        /* 控制器侧确认 BusOff 已退出时，只清空恢复状态机；
         * 真正何时重新拉回 Normal，由 TJA1042_Task() 的定时恢复负责。 */
        s_Status[channel].busOffRecoveryActive = 0U;
        s_Status[channel].busOffRecoverAtTickMs = 0U;
        s_Status[channel].busOffRecoveryDelayMs = 0U;
        s_Status[channel].busOffRecoveryCount = 0U;
        return true;
    }

    /* 一旦进入 BusOff，先把收发器打到 Standby，避免继续把异常状态扩散到总线。 */
    GPIO_PinWrite(s_TjaPins[channel].port, s_TjaPins[channel].pin, 1U);
    s_ModeCache[channel] = kTja1042Mode_Standby;
    s_Status[channel].requestedMode = kTja1042Mode_Normal;
    s_Status[channel].appliedMode = kTja1042Mode_Standby;
    s_Status[channel].busOffRecoveryActive = 1U;
    s_Status[channel].busOffRecoverAtTickMs = 0U;
    if (s_Status[channel].busOffRecoveryCount < 0xFFU)
    {
        s_Status[channel].busOffRecoveryCount++;
    }
    s_Status[channel].busOffRecoveryDelayMs = TJA1042_GetRecoveryDelayMs(s_Status[channel].busOffRecoveryCount);
    s_Status[channel].transitionCount++;
    return true;
}

bool TJA1042_GetStatus(tja1042_channel_t channel, tja1042_status_t *status)
{
    if ((!s_Ready) || (status == NULL) || (!TJA1042_IsValidChannel(channel)))
    {
        return false;
    }

    *status = s_Status[channel];
    return true;
}
