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

static bool CAN_StackIsNodeStateOnlyError(uint8_t errorCode);

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

static bool CAN_StackChannelIsReady(can_channel_t channel)
{
    return CAN_StackIsValidChannel(channel) && (s_RuntimeStatus[channel].ready != 0U);
}

static tja1042_channel_t CAN_StackMapPhyChannel(can_channel_t channel)
{
    static const tja1042_channel_t s_PhyMap[kCanChannel_Count] = {
        kTja1042_CanFd1,
        kTja1042_CanFd2,
        /* 这是当前实物板上“逻辑通道 -> 实际生效 STB 控制对象”的映射。
         *
         * 2026-03-22 做过专门的实机对照：
         * - 若按网表名义顺序恢复 CH2/CH3 的 STB 对应关系，
         *   则“只开 CH0/CH2”会稳定复现 CH0 ACK Error、CH2 Bit Error/BusOff。
         * - 恢复成当前顺序后，同样的 CH0/CH2 双通道回环可 12 秒压力 0 error。
         *
         * 因此这里记录的是“当前实板有效行为”，不是理论命名顺序。 */
        kTja1042_Can2,
        kTja1042_Can1,
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
    if (!normalized->enabled)
    {
        /* 关闭通道时强制关闭板载终端。
         * 否则 UI 上虽然显示“已关闭”，物理层上却还可能残留终端负载，
         * 后续排查 ACK/Bit/BusOff 时会非常容易被误导。 */
        normalized->terminationEnabled = false;
    }
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

static void CAN_StackBuildEffectiveConfig(can_channel_t channel,
                                          const can_channel_config_t *requested,
                                          uint8_t status,
                                          can_channel_config_t *effective)
{
    if ((requested == NULL) || (effective == NULL) || !CAN_StackIsValidChannel(channel))
    {
        return;
    }

    *effective = *requested;

    /* If a driver can only partially apply a request, keep the externally
     * visible config aligned with the controller's actual running state.
     */
    if ((channel == kCanChannel_CanFd1Ext) && (status == CAN_CFG_STATUS_STAGED_ONLY))
    {
        effective->frameFormat = kCanFrameFormat_Classic;
        effective->nominalBitrate = 500000U;
        effective->nominalSamplePointPermille = 800U;
        effective->dataBitrate = 0U;
        effective->dataSamplePointPermille = 0U;
    }
}

static void CAN_StackBuildHostSessionResetConfig(can_channel_t channel, can_channel_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    CAN_StackGetDefaultChannelConfig(channel, config);
    /* Host 会话级复位的语义是“回到统一的禁用态”，而不是恢复成默认启用。
     * 这样每次重新连接后，四个通道都会从相同起点开始，只有真正收到配置的
     * 通道才会重新进入工作状态。 */
    config->enabled = false;
    config->terminationEnabled = false;
}

static uint8_t CAN_StackApplyRuntimeSideEffects(can_channel_t channel,
                                                const can_channel_config_t *config,
                                                can_channel_config_t *appliedConfig)
{
    uint8_t status = CAN_CFG_STATUS_OK;
    tja1042_status_t phyStatus;
    can_channel_config_t driverAppliedConfig;

    if (!CAN_StackIsValidChannel(channel) || (config == NULL))
    {
        return CAN_CFG_STATUS_INVALID;
    }

    /* 这里是设备侧最关键的分界点：
     * 1. 先把配置真正落到 CAN 控制器
     * 2. 再去切板级终端、切 STB、刷新运行态缓存
     *
     * 这样后续排查时就能明确知道，问题是出在控制器拒绝配置，
     * 还是出在控制器已经配好但板级副作用没有跟上。 */
    driverAppliedConfig = *config;

    if (channel == kCanChannel_CanFd1Ext)
    {
        if (!CANFD1_ExtSpiApplyConfig(config))
        {
            PRINTF("CAN cfg apply fail CH%u mcp2517fd apply rejected\r\n", (uint32_t)channel);
            return CAN_CFG_STATUS_INVALID;
        }
    }
    else if (!CAN_InternalOnChipApplyConfig(channel, config))
    {
        PRINTF("CAN cfg apply fail CH%u onchip apply rejected\r\n", (uint32_t)channel);
        return CAN_CFG_STATUS_INVALID;
    }
    else if (!CAN_InternalOnChipGetAppliedConfig(channel, &driverAppliedConfig))
    {
        driverAppliedConfig = *config;
    }

    if (appliedConfig != NULL)
    {
        *appliedConfig = driverAppliedConfig;
    }

    /* 终端和 STB 是两条不同的板级控制链：
     * - 终端：按逻辑通道直接控制
     * - STB：必须先映射到真正对应的物理收发器
     * 两者不能混为一谈。 */
    (void)BSP_SetCanTermination(channel, driverAppliedConfig.terminationEnabled);
    (void)TJA1042_SetMode(
        CAN_StackMapPhyChannel(channel), driverAppliedConfig.enabled ? kTja1042Mode_Normal : kTja1042Mode_Standby);

    s_RuntimeStatus[channel].enabled = driverAppliedConfig.enabled ? 1U : 0U;
    s_RuntimeStatus[channel].ready = 1U;
    s_RuntimeStatus[channel].busOff = 0U;
    s_RuntimeStatus[channel].errorPassive = 0U;
    s_RuntimeStatus[channel].lastErrorCode = 0U;
    if (TJA1042_GetStatus(CAN_StackMapPhyChannel(channel), &phyStatus))
    {
        s_RuntimeStatus[channel].phyStandby = (phyStatus.appliedMode == kTja1042Mode_Standby) ? 1U : 0U;
    }

    PRINTF("CAN cfg applied CH%u status=%s ready=%u en=%u fmt=%s term=%u n=%u/%u d=%u/%u phyStandby=%u tx=%u rx=%u lastErr=0x%08X\r\n",
           (uint32_t)channel,
           CAN_StackStatusName(status),
           s_RuntimeStatus[channel].ready,
           s_RuntimeStatus[channel].enabled,
           CAN_StackFrameFormatName(driverAppliedConfig.frameFormat),
           driverAppliedConfig.terminationEnabled ? 1U : 0U,
           driverAppliedConfig.nominalBitrate,
           driverAppliedConfig.nominalSamplePointPermille,
           driverAppliedConfig.dataBitrate,
           driverAppliedConfig.dataSamplePointPermille,
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
    bool okExt = CANFD1_ExtSpiInit();
    bool okOnChip = CAN_InternalOnChipInit();
    (void)memset(s_RuntimeStatus, 0, sizeof(s_RuntimeStatus));
#if defined(CAN_LOOPBACK_MODE) && (CAN_LOOPBACK_MODE == 1)
    (void)TJA1042_SetMode(kTja1042_CanFd1, kTja1042Mode_Standby);
    (void)TJA1042_SetMode(kTja1042_CanFd2, kTja1042Mode_Standby);
    (void)TJA1042_SetMode(kTja1042_Can1, kTja1042Mode_Standby);
    (void)TJA1042_SetMode(kTja1042_Can2, kTja1042Mode_Standby);
#endif
    /* 缁熶竴鍒濆鍖栧缃?CANFD 涓庣墖涓?CAN銆?*/
    {
        s_StackReady = okPhy && okExt && okOnChip;
    }

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        s_RuntimeStatus[i].ready = 0U;
    }

    /* 启动时先把四个通道都压到 Host 会话复位态，避免设备一上电就带着默认
     * enabled=true 的状态在后台工作，给后续调试制造“历史残留”。 */
    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        uint8_t ignoredStatus;
        can_channel_config_t resetConfig;
        CAN_StackBuildHostSessionResetConfig((can_channel_t)i, &resetConfig);
        s_ChannelConfigs[i] = resetConfig;
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

static void CAN_StackRefreshDriverRuntimeStatus(can_channel_t channel)
{
    can_driver_runtime_state_t driverState;
    bool ok = false;
    bool busOffBefore;

    if (!CAN_StackIsValidChannel(channel))
    {
        return;
    }

    if (channel == kCanChannel_CanFd1Ext)
    {
        ok = CANFD1_ExtSpiGetRuntimeState(&driverState);
    }
    else
    {
        ok = CAN_InternalOnChipGetRuntimeState(channel, &driverState);
    }

    if (!ok)
    {
        return;
    }

    busOffBefore = (s_RuntimeStatus[channel].busOff != 0U);
    s_RuntimeStatus[channel].busOff = driverState.busOff;
    s_RuntimeStatus[channel].errorPassive = driverState.errorPassive;
    s_RuntimeStatus[channel].rxPending = driverState.rxPending;
    s_RuntimeStatus[channel].txPending = driverState.txPending;
    if ((driverState.lastErrorCode != 0U) && !CAN_StackIsNodeStateOnlyError((uint8_t)driverState.lastErrorCode))
    {
        s_RuntimeStatus[channel].lastErrorCode = driverState.lastErrorCode;
    }

    /* 只在 BusOff 状态发生边沿变化时通知收发器状态机，避免周期任务里重复
     * 触发 Standby/恢复流程。 */
    if ((!busOffBefore) && (driverState.busOff != 0U))
    {
        (void)TJA1042_NotifyBusState(CAN_StackMapPhyChannel(channel), true);
    }
    else if (busOffBefore && (driverState.busOff == 0U))
    {
        (void)TJA1042_NotifyBusState(CAN_StackMapPhyChannel(channel), false);
    }
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
        CAN_StackRefreshDriverRuntimeStatus(channel);
        return;
    }

    CAN_InternalOnChipTaskChannel(channel);
    CAN_StackRefreshDriverRuntimeStatus(channel);
}

bool CAN_StackApplyChannelConfig(can_channel_t channel, const can_channel_config_t *config, uint8_t *statusCode)
{
    can_channel_config_t normalized;
    can_channel_config_t applied;
    can_channel_config_t effective;
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
    status = CAN_StackApplyRuntimeSideEffects(channel, &normalized, &applied);
    if (statusCode != NULL)
    {
        *statusCode = status;
    }
    if (status == CAN_CFG_STATUS_INVALID)
    {
        return false;
    }

    CAN_StackBuildEffectiveConfig(channel, &applied, status, &effective);
    s_ChannelConfigs[channel] = effective;

    return true;
}

bool CAN_StackRecoverChannel(can_channel_t channel)
{
    can_channel_config_t applied;
    can_channel_config_t effective;
    uint8_t status;

    if (!CAN_StackIsValidChannel(channel))
    {
        return false;
    }

    status = CAN_StackApplyRuntimeSideEffects(channel, &s_ChannelConfigs[channel], &applied);
    if (status == CAN_CFG_STATUS_INVALID)
    {
        return false;
    }

    CAN_StackBuildEffectiveConfig(channel, &applied, status, &effective);
    s_ChannelConfigs[channel] = effective;
    return true;
}

bool CAN_StackResetChannelToDefault(can_channel_t channel)
{
    can_channel_config_t defaultConfig;
    uint8_t status;

    if (!CAN_StackIsValidChannel(channel))
    {
        return false;
    }

    CAN_StackGetDefaultChannelConfig(channel, &defaultConfig);
    return CAN_StackApplyChannelConfig(channel, &defaultConfig, &status);
}

bool CAN_StackResetChannelForHostSession(can_channel_t channel)
{
    can_channel_config_t resetConfig;
    uint8_t status;

    if (!CAN_StackIsValidChannel(channel))
    {
        return false;
    }

    CAN_StackBuildHostSessionResetConfig(channel, &resetConfig);
    return CAN_StackApplyChannelConfig(channel, &resetConfig, &status);
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
    CAN_StackRefreshDriverRuntimeStatus(channel);
    *status = s_RuntimeStatus[channel];
    return true;
}

bool CAN_StackPeekChannelRuntimeStatus(can_channel_t channel, can_channel_runtime_status_t *status)
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

static bool CAN_StackIsNodeStateOnlyError(uint8_t errorCode)
{
    return (errorCode == 0x07U);
}

static void CAN_StackApplyEventRuntimeStatus(can_channel_t channel, const can_bus_event_t *event)
{
    if ((!CAN_StackIsValidChannel(channel)) || (event == NULL))
    {
        return;
    }

    switch (event->type)
    {
        case kCanBusEvent_RxFrame:
            s_RuntimeStatus[channel].rxCount++;
            break;

        case kCanBusEvent_TxComplete:
            s_RuntimeStatus[channel].txCount++;
            s_RuntimeStatus[channel].lastErrorCode = 0U;
            break;

        case kCanBusEvent_Error:
            if (!CAN_StackIsNodeStateOnlyError(event->errorCode))
            {
                s_RuntimeStatus[channel].lastErrorCode = event->errorCode;
            }
            break;

        default:
            break;
    }

    if (event->errorCode == 0x06U)
    {
        s_RuntimeStatus[channel].busOff = 1U;
    }
    else if (event->errorCode == 0x07U)
    {
        s_RuntimeStatus[channel].errorPassive = 1U;
    }
}

bool CAN_StackSend(can_channel_t channel, const can_frame_t *frame)
{
    const can_channel_config_t *cfg;

    if ((frame == NULL) || !CAN_StackIsValidChannel(channel))
    {
        return false;
    }
    if (!CAN_StackChannelIsReady(channel))
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
        return CANFD1_ExtSpiSend(frame);
    }
    return CAN_InternalOnChipSend(channel, frame);
}

bool CAN_StackReceive(can_channel_t channel, can_frame_t *frame)
{
    if ((frame == NULL) || !CAN_StackIsValidChannel(channel))
    {
        return false;
    }
    if (!CAN_StackChannelIsReady(channel))
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

bool CAN_StackPollEvent(can_channel_t channel, can_bus_event_t *event)
{
    bool ok;

    if ((event == NULL) || !CAN_StackIsValidChannel(channel))
    {
        return false;
    }
    if (!CAN_StackChannelIsReady(channel))
    {
        return false;
    }
    if (!s_ChannelConfigs[channel].enabled)
    {
        return false;
    }

    if (channel == kCanChannel_CanFd1Ext)
    {
        ok = CANFD1_ExtSpiPollEvent(event);
    }
    else
    {
        ok = CAN_InternalOnChipPollEvent(channel, event);
    }

    if (ok)
    {
        CAN_StackApplyEventRuntimeStatus(channel, event);
    }
    return ok;
}
