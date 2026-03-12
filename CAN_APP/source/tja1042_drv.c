#include "tja1042_drv.h"

#include "fsl_gpio.h"

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

#define TJA1042_BUS_OFF_RECOVERY_MS (100U)

static bool TJA1042_IsValidChannel(tja1042_channel_t channel)
{
    return ((uint8_t)channel < (uint8_t)kTja1042_Count);
}

bool TJA1042_Init(void)
{
    uint32_t i;

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

    stbLevel = (mode == kTja1042Mode_Standby) ? 1U : 0U;
    GPIO_PinWrite(s_TjaPins[channel].port, s_TjaPins[channel].pin, stbLevel);
    s_ModeCache[channel] = mode;
    s_Status[channel].requestedMode = mode;
    s_Status[channel].appliedMode = mode;
    s_Status[channel].busOffRecoveryActive = 0U;
    s_Status[channel].busOffRecoverAtTickMs = 0U;
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
            s_Status[i].busOffRecoverAtTickMs = tickMs + TJA1042_BUS_OFF_RECOVERY_MS;
            continue;
        }

        if ((s_Status[i].busOffRecoveryActive != 0U) &&
            ((int32_t)(tickMs - s_Status[i].busOffRecoverAtTickMs) >= 0))
        {
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
        return true;
    }

    GPIO_PinWrite(s_TjaPins[channel].port, s_TjaPins[channel].pin, 1U);
    s_ModeCache[channel] = kTja1042Mode_Standby;
    s_Status[channel].requestedMode = kTja1042Mode_Normal;
    s_Status[channel].appliedMode = kTja1042Mode_Standby;
    s_Status[channel].busOffRecoveryActive = 1U;
    s_Status[channel].busOffRecoverAtTickMs = 0U;
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
