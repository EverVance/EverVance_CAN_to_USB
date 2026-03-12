#include "can_stack.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "bsp_peripherals.h"
#include "can_internal_onchip.h"
#include "canfd1_ext_spi.h"
#include "fsl_debug_console.h"
#include "tja1042_drv.h"

#define CAN_CFG_STATUS_OK (0U)
#define CAN_CFG_STATUS_INVALID (1U)
#define CAN_CFG_STATUS_STAGED_ONLY (2U)

#define CAN_STACK_FEATURE_USB_VENDOR (1UL << 0U)
#define CAN_STACK_FEATURE_AUTO_CFG (1UL << 1U)
#define CAN_STACK_FEATURE_EXT_CANFD (1UL << 2U)
#define CAN_STACK_FEATURE_ONCHIP_CANFD (1UL << 3U)
#define CAN_STACK_FEATURE_TERM_CTRL (1UL << 4U)
#define CAN_STACK_FEATURE_PHY_STATE_MACHINE (1UL << 5U)

static bool s_StackReady;
static can_channel_config_t s_ChannelConfigs[kCanChannel_Count];
static can_channel_runtime_status_t s_RuntimeStatus[kCanChannel_Count];

static const char *CAN_StackFrameFormatName(can_frame_format_t frameFormat)
{
    return (frameFormat == kCanFrameFormat_Fd) ? "FD" : "CAN";
}

static const char *CAN_StackStatusName(uint8_t status)
{
    switch (status)
    {
        case CAN_CFG_STATUS_OK:
            return "OK";
        case CAN_CFG_STATUS_INVALID:
            return "INVALID";
        case CAN_CFG_STATUS_STAGED_ONLY:
            return "STAGED";
        default:
            return "UNKNOWN";
    }
}

static void CAN_StackPrintConfig(const char *prefix, can_channel_t channel, const can_channel_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    PRINTF("%s CH%u fmt=%s en=%u term=%u n=%u/%u d=%u/%u\r\n",
           prefix,
           (uint32_t)channel,
           CAN_StackFrameFormatName(config->frameFormat),
           config->enabled ? 1U : 0U,
           config->terminationEnabled ? 1U : 0U,
           config->nominalBitrate,
           config->nominalSamplePointPermille,
           config->dataBitrate,
           config->dataSamplePointPermille);
}

static bool CAN_StackIsValidChannel(can_channel_t channel)
{
    return ((uint8_t)channel < (uint8_t)kCanChannel_Count);
}

static tja1042_channel_t CAN_StackMapPhyChannel(can_channel_t channel)
{
    static const tja1042_channel_t s_PhyMap[kCanChannel_Count] = {
        kTja1042_CanFd1,
        kTja1042_CanFd2,
        kTja1042_Can1,
        kTja1042_Can2,
    };

    return s_PhyMap[channel];
}

bool CAN_StackGetChannelCapabilities(can_channel_t channel, can_channel_capabilities_t *capabilities)
{
    if ((!CAN_StackIsValidChannel(channel)) || (capabilities == NULL))
    {
        return false;
    }

    (void)memset(capabilities, 0, sizeof(*capabilities));
    capabilities->supportsClassic = 1U;
    capabilities->supportsTermination = 1U;
    capabilities->nominalBitrateMin = 10000U;
    capabilities->nominalBitrateMax = 1000000U;
    capabilities->nominalSampleMinPermille = 500U;
    capabilities->nominalSampleMaxPermille = 900U;
    capabilities->dataSampleMinPermille = 500U;
    capabilities->dataSampleMaxPermille = 850U;

    switch (channel)
    {
        case kCanChannel_CanFd1Ext:
            capabilities->supportsFd = 1U;
            capabilities->driverType = 1U;
            capabilities->dataBitrateMax = 5000000U;
            break;

        case kCanChannel_Can2:
            capabilities->supportsFd = 1U;
            capabilities->driverType = 4U;
            capabilities->dataBitrateMax = 4000000U;
            break;

        case kCanChannel_Can3:
            capabilities->supportsFd = 0U;
            capabilities->driverType = 2U;
            break;

        case kCanChannel_Can4:
            capabilities->supportsFd = 0U;
            capabilities->driverType = 3U;
            break;

        default:
            return false;
    }

    return true;
}

void CAN_StackGetDefaultChannelConfig(can_channel_t channel, can_channel_config_t *config)
{
    if ((!CAN_StackIsValidChannel(channel)) || (config == NULL))
    {
        return;
    }

    (void)memset(config, 0, sizeof(*config));
    config->enabled = true;
    config->terminationEnabled = false;
    config->frameFormat =
        ((channel == kCanChannel_CanFd1Ext) || (channel == kCanChannel_Can2)) ? kCanFrameFormat_Fd : kCanFrameFormat_Classic;
    config->nominalBitrate = 500000U;
    config->nominalSamplePointPermille = 800U;
    config->dataBitrate = (config->frameFormat == kCanFrameFormat_Fd) ? 2000000U : 0U;
    config->dataSamplePointPermille = (config->frameFormat == kCanFrameFormat_Fd) ? 750U : 0U;
}

static bool CAN_StackNormalizeConfig(can_channel_t channel, const can_channel_config_t *config, can_channel_config_t *normalized)
{
    can_channel_capabilities_t capabilities;

    if ((config == NULL) || (normalized == NULL))
    {
        return false;
    }
    if (!CAN_StackGetChannelCapabilities(channel, &capabilities))
    {
        return false;
    }
    if ((config->nominalBitrate == 0U) || (config->nominalSamplePointPermille < 500U) || (config->nominalSamplePointPermille > 900U))
    {
        return false;
    }

    *normalized = *config;
    if (normalized->frameFormat == kCanFrameFormat_Classic)
    {
        normalized->dataBitrate = 0U;
        normalized->dataSamplePointPermille = 0U;
        return true;
    }
    if (capabilities.supportsFd == 0U)
    {
        return false;
    }

    if ((normalized->dataBitrate == 0U) || (normalized->dataSamplePointPermille < 500U) || (normalized->dataSamplePointPermille > 850U))
    {
        return false;
    }

    return true;
}

static uint8_t CAN_StackApplyRuntimeSideEffects(can_channel_t channel, const can_channel_config_t *config)
{
    uint8_t status = CAN_CFG_STATUS_OK;
    tja1042_status_t phyStatus;

    if (!CAN_StackIsValidChannel(channel) || (config == NULL))
    {
        return CAN_CFG_STATUS_INVALID;
    }

    (void)BSP_SetCanTermination(channel, config->terminationEnabled);
    (void)TJA1042_SetMode(CAN_StackMapPhyChannel(channel), config->enabled ? kTja1042Mode_Normal : kTja1042Mode_Standby);

    if (channel == kCanChannel_CanFd1Ext)
    {
        if ((config->nominalBitrate != 500000U) || (config->nominalSamplePointPermille != 800U) ||
            ((config->frameFormat == kCanFrameFormat_Fd) &&
             ((config->dataBitrate != 2000000U) || (config->dataSamplePointPermille != 750U))))
        {
            status = CAN_CFG_STATUS_STAGED_ONLY;
        }
    }
    else if (!CAN_InternalOnChipApplyConfig(channel, config))
    {
        PRINTF("CAN cfg apply fail CH%u onchip apply rejected\r\n", (uint32_t)channel);
        return CAN_CFG_STATUS_INVALID;
    }

    s_RuntimeStatus[channel].enabled = config->enabled ? 1U : 0U;
    s_RuntimeStatus[channel].ready = s_StackReady ? 1U : 0U;
    if (TJA1042_GetStatus(CAN_StackMapPhyChannel(channel), &phyStatus))
    {
        s_RuntimeStatus[channel].phyStandby = (phyStatus.appliedMode == kTja1042Mode_Standby) ? 1U : 0U;
    }

    PRINTF("CAN cfg applied CH%u status=%s ready=%u en=%u phyStandby=%u tx=%u rx=%u lastErr=0x%08X\r\n",
           (uint32_t)channel,
           CAN_StackStatusName(status),
           s_RuntimeStatus[channel].ready,
           s_RuntimeStatus[channel].enabled,
           s_RuntimeStatus[channel].phyStandby,
           s_RuntimeStatus[channel].txCount,
           s_RuntimeStatus[channel].rxCount,
           (unsigned int)s_RuntimeStatus[channel].lastErrorCode);

    return status;
}

bool CAN_StackInit(void)
{
    uint32_t i;
    bool okPhy = TJA1042_Init();
    (void)memset(s_RuntimeStatus, 0, sizeof(s_RuntimeStatus));
#if defined(CAN_LOOPBACK_MODE) && (CAN_LOOPBACK_MODE == 1)
    (void)TJA1042_SetMode(kTja1042_CanFd1, kTja1042Mode_Standby);
    (void)TJA1042_SetMode(kTja1042_CanFd2, kTja1042Mode_Standby);
    (void)TJA1042_SetMode(kTja1042_Can1, kTja1042Mode_Standby);
    (void)TJA1042_SetMode(kTja1042_Can2, kTja1042Mode_Standby);
#endif
    /* 缁熶竴鍒濆鍖栧缃?CANFD 涓庣墖涓?CAN銆?*/
    {
        bool okExt = CANFD1_ExtSpiInit();
        bool okOnChip = CAN_InternalOnChipInit();
        s_StackReady = okPhy && okExt && okOnChip;
    }

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        s_RuntimeStatus[i].ready = s_StackReady ? 1U : 0U;
    }

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        uint8_t ignoredStatus;
        CAN_StackGetDefaultChannelConfig((can_channel_t)i, &s_ChannelConfigs[i]);
        (void)CAN_StackApplyChannelConfig((can_channel_t)i, &s_ChannelConfigs[i], &ignoredStatus);
    }

    return s_StackReady;
}

void CAN_StackTask(void)
{
    TJA1042_Task((uint32_t)xTaskGetTickCount());
    CANFD1_ExtSpiTask();
    CAN_InternalOnChipTask();
}

void CAN_StackTaskChannel(can_channel_t channel)
{
    tja1042_status_t phyStatus;

    TJA1042_Task((uint32_t)xTaskGetTickCount());
    if (CAN_StackIsValidChannel(channel) && TJA1042_GetStatus(CAN_StackMapPhyChannel(channel), &phyStatus))
    {
        s_RuntimeStatus[channel].phyStandby = (phyStatus.appliedMode == kTja1042Mode_Standby) ? 1U : 0U;
    }
    /* 閫氶亾 0 涓哄缃?MCP2517FD锛屽叾浣欎负鐗囦笂閫氶亾銆?*/
    if (channel == kCanChannel_CanFd1Ext)
    {
        CANFD1_ExtSpiTask();
        return;
    }

    CAN_InternalOnChipTaskChannel(channel);
}

bool CAN_StackApplyChannelConfig(can_channel_t channel, const can_channel_config_t *config, uint8_t *statusCode)
{
    can_channel_config_t normalized;
    uint8_t status;

    if (statusCode != NULL)
    {
        *statusCode = CAN_CFG_STATUS_INVALID;
    }
    if (!CAN_StackIsValidChannel(channel))
    {
        PRINTF("CAN cfg reject invalid channel=%u\r\n", (uint32_t)channel);
        return false;
    }
    if (!CAN_StackNormalizeConfig(channel, config, &normalized))
    {
        CAN_StackPrintConfig("CAN cfg reject", channel, config);
        return false;
    }

    CAN_StackPrintConfig("CAN cfg req", channel, &normalized);
    s_ChannelConfigs[channel] = normalized;
    status = CAN_StackApplyRuntimeSideEffects(channel, &normalized);
    if (statusCode != NULL)
    {
        *statusCode = status;
    }

    return true;
}

bool CAN_StackGetChannelConfig(can_channel_t channel, can_channel_config_t *config)
{
    if ((!CAN_StackIsValidChannel(channel)) || (config == NULL))
    {
        return false;
    }

    *config = s_ChannelConfigs[channel];
    return true;
}

bool CAN_StackGetChannelRuntimeStatus(can_channel_t channel, can_channel_runtime_status_t *status)
{
    if ((!CAN_StackIsValidChannel(channel)) || (status == NULL))
    {
        return false;
    }
    *status = s_RuntimeStatus[channel];
    return true;
}

uint32_t CAN_StackGetFeatureFlags(void)
{
    return CAN_STACK_FEATURE_USB_VENDOR | CAN_STACK_FEATURE_AUTO_CFG | CAN_STACK_FEATURE_EXT_CANFD |
           CAN_STACK_FEATURE_ONCHIP_CANFD | CAN_STACK_FEATURE_TERM_CTRL | CAN_STACK_FEATURE_PHY_STATE_MACHINE;
}

bool CAN_StackSend(can_channel_t channel, const can_frame_t *frame)
{
    const can_channel_config_t *cfg;

    if ((s_StackReady == false) || (frame == NULL) || !CAN_StackIsValidChannel(channel))
    {
        return false;
    }

    cfg = &s_ChannelConfigs[channel];
    if (!cfg->enabled)
    {
        return false;
    }
    if ((cfg->frameFormat == kCanFrameFormat_Classic) && (((frame->flags & 0x01U) != 0U) || (frame->dlc > 8U)))
    {
        return false;
    }

    if (channel == kCanChannel_CanFd1Ext)
    {
        if (CANFD1_ExtSpiSend(frame))
        {
            s_RuntimeStatus[channel].txCount++;
            return true;
        }
        s_RuntimeStatus[channel].lastErrorCode = 0x08U;
        return false;
    }
    if (CAN_InternalOnChipSend(channel, frame))
    {
        s_RuntimeStatus[channel].txCount++;
        return true;
    }
    s_RuntimeStatus[channel].lastErrorCode = 0x08U;
    return false;
}

bool CAN_StackReceive(can_channel_t channel, can_frame_t *frame)
{
    if ((s_StackReady == false) || (frame == NULL) || !CAN_StackIsValidChannel(channel))
    {
        return false;
    }

    if (!s_ChannelConfigs[channel].enabled)
    {
        return false;
    }

    if (channel == kCanChannel_CanFd1Ext)
    {
        if (CANFD1_ExtSpiReceive(frame))
        {
            s_RuntimeStatus[channel].rxCount++;
            return true;
        }
        return false;
    }
    if (CAN_InternalOnChipReceive(channel, frame))
    {
        s_RuntimeStatus[channel].rxCount++;
        return true;
    }
    return false;
}
