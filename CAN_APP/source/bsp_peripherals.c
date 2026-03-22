#include "bsp_peripherals.h"

#include <string.h>

#include "fsl_common.h"
#include "fsl_clock.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"

/* 文件说明：
 * 本文件实现板级外设控制，负责：
 * - GPIO 与 pin mux 初始化
 * - 可控终端电阻开关
 * - 状态灯与活动灯
 * - LPSPI1 / FlexCAN 的板级准备
 *
 * 如果问题更像“硬件控制链异常”而不是“协议逻辑异常”，优先看本文件。 */

#define PAD_CTL_GPIO_OUT (0x10B0U)
#define PAD_CTL_GPIO_STRONG_PP \
    (IOMUXC_SW_PAD_CTL_PAD_SRE(1U) | IOMUXC_SW_PAD_CTL_PAD_DSE(7U) | IOMUXC_SW_PAD_CTL_PAD_SPEED(3U))
#define PAD_CTL_GPIO_IN_PULLUP (0x01B0B0U)
#define GPIO_MUX2_USED_MASK (0xD73101FFU)
#define GPIO_MUX3_USED_MASK (0x00002000U)
#define SYS_3V3_ENABLE_SETTLE_US (5000U)
#define STARTUP_LED_STEP_MS (120U)
#define CAN_ACTIVITY_HOLD_MS (220U)
#define CAN_ACTIVITY_BLINK_MS (100U)
#define CAN_ENABLED_BREATH_MS (800U)

static const uint8_t s_StatusLedPins[kBspStatusLedCount] = {
    25U,
    16U,
    8U,
    26U,
    21U,
    20U,
};
static const uint8_t s_CanTerminationPins[kCanChannel_Count] = {
    28U, /* D13_GPIO_CAN_FD_1_RES -> GPIO2_IO28 */
    24U, /* A12_GPIO_CAN_FD_2_RES -> GPIO2_IO24 */
    31U, /* B14_GPIO_CAN1_RES     -> GPIO2_IO31 */
    30U, /* C14_GPIO_CAN2_RES     -> GPIO2_IO30 */
};
static const bsp_status_led_t s_ActiveStatusLeds[] = {
    kBspStatusLed1,
    kBspStatusLed2,
    kBspStatusLed3,
    kBspStatusLed4,
    kBspStatusLed5,
    kBspStatusLed6,
};
static const bsp_status_led_t s_ChannelStatusLeds[kCanChannel_Count] = {
    kBspStatusLed1,
    kBspStatusLed2,
    kBspStatusLed3,
    kBspStatusLed4,
};
static uint32_t s_CanLedExpireTickMs[kBspStatusLedCount];
static uint16_t s_ChannelLedPdmAccumulator[kCanChannel_Count];
static uint8_t s_StatusLedLogicalOn[kBspStatusLedCount];

#define GPIO_OUT_INIT(level) \
    {                        \
        kGPIO_DigitalOutput, \
        (level),             \
        kGPIO_NoIntmode      \
    }

#define GPIO_IN_INIT()      \
    {                       \
        kGPIO_DigitalInput, \
        0U,                 \
        kGPIO_NoIntmode     \
    }

#define BSP_CONFIG_GPIO_OUTPUT(PIN_MUX_ID, PORT, PIN, LEVEL) \
    do                                                        \
    {                                                         \
        const gpio_pin_config_t cfg = GPIO_OUT_INIT(LEVEL);  \
        IOMUXC_SetPinMux(PIN_MUX_ID, 0U);                    \
        IOMUXC_SetPinConfig(PIN_MUX_ID, PAD_CTL_GPIO_OUT);   \
        GPIO_PinInit((PORT), (PIN), &cfg);                   \
    } while (0)

#define BSP_CONFIG_GPIO_INPUT(PIN_MUX_ID, PORT, PIN)            \
    do                                                          \
    {                                                           \
        const gpio_pin_config_t cfg = GPIO_IN_INIT();          \
        IOMUXC_SetPinMux(PIN_MUX_ID, 0U);                      \
        IOMUXC_SetPinConfig(PIN_MUX_ID, PAD_CTL_GPIO_IN_PULLUP); \
        GPIO_PinInit((PORT), (PIN), &cfg);                     \
    } while (0)

static void BSP_InitControlGpioFromNetlist(void)
{
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B0_05_GPIO2_IO05, GPIO2, 5U, 0U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B0_07_GPIO2_IO07, GPIO2, 7U, 0U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B0_03_GPIO2_IO03, GPIO2, 3U, 0U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B0_04_GPIO2_IO04, GPIO2, 4U, 0U);

    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_15_GPIO2_IO31, GPIO2, 31U, 0U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_14_GPIO2_IO30, GPIO2, 30U, 0U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_12_GPIO2_IO28, GPIO2, 28U, 0U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_08_GPIO2_IO24, GPIO2, 24U, 0U);

    BSP_CONFIG_GPIO_INPUT(IOMUXC_GPIO_B0_00_GPIO2_IO00, GPIO2, 0U);
    BSP_CONFIG_GPIO_INPUT(IOMUXC_GPIO_B0_01_GPIO2_IO01, GPIO2, 1U);
    BSP_CONFIG_GPIO_INPUT(IOMUXC_GPIO_B0_02_GPIO2_IO02, GPIO2, 2U);

    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_09_GPIO2_IO25, GPIO2, 25U, 1U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_00_GPIO2_IO16, GPIO2, 16U, 1U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B0_08_GPIO2_IO08, GPIO2, 8U, 1U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_10_GPIO2_IO26, GPIO2, 26U, 1U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_05_GPIO2_IO21, GPIO2, 21U, 1U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_04_GPIO2_IO20, GPIO2, 20U, 1U);
}

/* 打开板上的 3V3 受控电源轨。
 * 某些外设依赖该电源轨，若漏调会表现为“软件初始化成功但硬件完全无响应”。 */
static void BSP_EnableSys3v3Rail(void)
{
    const gpio_pin_config_t cfg = GPIO_OUT_INIT(1U);

    IOMUXC_SetPinMux(IOMUXC_GPIO_B0_06_GPIO2_IO06, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B0_06_GPIO2_IO06, PAD_CTL_GPIO_STRONG_PP);
    GPIO_PinInit(GPIO2, 6U, &cfg);
    SDK_DelayAtLeastUs(SYS_3V3_ENABLE_SETTLE_US, SystemCoreClock);
}

/* 选择共享 GPIO 实例，保证后续 LED/控制引脚都已切到 GPIO 模式。 */
static void BSP_SelectSharedGpioInstances(void)
{
    /* GPIO_B0/B1 are shared between GPIO2/GPIO7, while GPIO_SD_B0 can be
     * shared between GPIO3/GPIO8. Force the pins used by this board netlist
     * onto the GPIO instances we actually drive in software. */
    IOMUXC_GPR->GPR27 &= ~GPIO_MUX2_USED_MASK;
    IOMUXC_GPR->GPR28 &= ~GPIO_MUX3_USED_MASK;
}

/* 控制某路终端电阻开关。 */
bool BSP_SetCanTermination(can_channel_t channel, bool enabled)
{
    if ((uint8_t)channel >= (uint8_t)kCanChannel_Count)
    {
        return false;
    }

    /* The resistor-enable network defaults low in the current board design;
     * drive high to connect the selectable termination path. */
    GPIO_PinWrite(GPIO2, s_CanTerminationPins[channel], enabled ? 1U : 0U);
    return true;
}

/* 读取某路终端电阻当前缓存状态。 */
bool BSP_GetCanTermination(can_channel_t channel, bool *enabled)
{
    if (((uint8_t)channel >= (uint8_t)kCanChannel_Count) || (enabled == NULL))
    {
        return false;
    }

    *enabled = (GPIO_PinRead(GPIO2, s_CanTerminationPins[channel]) != 0U);
    return true;
}

/* 一次性设置所有状态灯。 */
void BSP_SetStatusLeds(bool on)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)(sizeof(s_ActiveStatusLeds) / sizeof(s_ActiveStatusLeds[0])); i++)
    {
        BSP_SetStatusLed(s_ActiveStatusLeds[i], on);
    }
}

/* 控制单个状态灯。
 * 当前板子 LED 为共阳，低电平点亮，因此这里不能随意改极性。 */
void BSP_SetStatusLed(bsp_status_led_t led, bool on)
{
    uint8_t level;

    if ((uint32_t)led >= (uint32_t)kBspStatusLedCount)
    {
        return;
    }
    if (s_StatusLedLogicalOn[led] == (uint8_t)(on ? 1U : 0U))
    {
        return;
    }

    /* Netlist shows the LED current path comes from VCC_SYS_3V3 through resistors,
     * so the MCU GPIOs sink current and are active-low. */
    level = on ? 0U : 1U;
    GPIO_PinWrite(GPIO2, s_StatusLedPins[led], level);
    s_StatusLedLogicalOn[led] = on ? 1U : 0U;
}

/* 上电扫灯，用于确认 LED 硬件与 GPIO 初始化正常。 */
void BSP_RunStartupLedSweep(void)
{
    uint32_t i;

    BSP_SetStatusLeds(false);

    for (i = 0U; i < (uint32_t)(sizeof(s_ActiveStatusLeds) / sizeof(s_ActiveStatusLeds[0])); i++)
    {
        BSP_SetStatusLed(s_ActiveStatusLeds[i], true);
        SDK_DelayAtLeastUs(STARTUP_LED_STEP_MS * 1000U, SystemCoreClock);
        BSP_SetStatusLed(s_ActiveStatusLeds[i], false);
    }
}

/* 记录某个灯最近一次活动时间戳。 */
static void BSP_MarkLedActivity(bsp_status_led_t led, uint32_t tickMs)
{
    if ((uint32_t)led >= (uint32_t)kBspStatusLedCount)
    {
        return;
    }
    s_CanLedExpireTickMs[led] = tickMs + CAN_ACTIVITY_HOLD_MS;
}

/* 将逻辑通道活动映射到对应 LED。 */
static void BSP_MarkChannelActivity(can_channel_t channel, uint32_t tickMs)
{
    if ((uint32_t)channel >= (uint32_t)kCanChannel_Count)
    {
        return;
    }

    BSP_MarkLedActivity(s_ChannelStatusLeds[channel], tickMs);
}

/* 记录 CAN 发送活动，用于灯语闪烁。 */
void BSP_NotifyCanTxActivity(can_channel_t channel, uint32_t tickMs)
{
    BSP_MarkChannelActivity(channel, tickMs);
}

/* 记录 CAN 接收活动，用于灯语闪烁。 */
void BSP_NotifyCanRxActivity(can_channel_t channel, uint32_t tickMs)
{
    BSP_MarkChannelActivity(channel, tickMs);
}

/* 重置通道灯语内部状态。 */
void BSP_ResetCanLedState(void)
{
    uint32_t i;

    (void)memset(s_CanLedExpireTickMs, 0, sizeof(s_CanLedExpireTickMs));
    (void)memset(s_ChannelLedPdmAccumulator, 0, sizeof(s_ChannelLedPdmAccumulator));
    for (i = 0U; i < (uint32_t)kBspStatusLedCount; i++)
    {
        s_StatusLedLogicalOn[i] = 0xFFU;
    }
}

/* 计算“呼吸灯”某一时刻的点亮占空比。 */
static bool BSP_ComputeBreathingPdm(uint32_t tickMs, uint32_t channelIndex)
{
    uint32_t phaseMs;
    uint32_t halfCycleMs;
    uint32_t brightness;

    halfCycleMs = CAN_ENABLED_BREATH_MS / 2U;
    phaseMs = tickMs % CAN_ENABLED_BREATH_MS;
    if (phaseMs < halfCycleMs)
    {
        brightness = (phaseMs * 255U) / halfCycleMs;
    }
    else
    {
        brightness = ((CAN_ENABLED_BREATH_MS - phaseMs) * 255U) / halfCycleMs;
    }

    s_ChannelLedPdmAccumulator[channelIndex] =
        (uint16_t)(s_ChannelLedPdmAccumulator[channelIndex] + (uint16_t)brightness);
    if (s_ChannelLedPdmAccumulator[channelIndex] >= 256U)
    {
        s_ChannelLedPdmAccumulator[channelIndex] = (uint16_t)(s_ChannelLedPdmAccumulator[channelIndex] - 256U);
        return true;
    }

    return false;
}

void BSP_UpdateCanActivityLeds(uint32_t tickMs,
                               const bool *channelEnabled,
                               const bool *channelBusOff,
                               uint32_t channelCount)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        bool enabled = false;
        bool busOff = false;
        bool active = false;
        bool ledOn = false;
        bsp_status_led_t led = s_ChannelStatusLeds[i];

        if ((channelEnabled != NULL) && (i < channelCount))
        {
            enabled = channelEnabled[i];
        }
        if ((channelBusOff != NULL) && (i < channelCount))
        {
            busOff = channelBusOff[i];
        }

        active = ((int32_t)(s_CanLedExpireTickMs[led] - tickMs) > 0);
        if (!enabled)
        {
            ledOn = false;
            s_ChannelLedPdmAccumulator[i] = 0U;
        }
        else if (busOff)
        {
            ledOn = true;
        }
        else if (active)
        {
            ledOn = (((tickMs / CAN_ACTIVITY_BLINK_MS) & 0x1U) == 0U);
        }
        else
        {
            ledOn = BSP_ComputeBreathingPdm(tickMs, i);
        }

        BSP_SetStatusLed(led, ledOn);
    }
}

/* 初始化 CAN 相关 pinmux。 */
static void BSP_InitCanPinMux(void)
{
    /* Match the official SDK FlexCAN examples: force the input path on CAN pads
     * and keep all TX/RX pads on the same 0x10B0 electrical configuration.
     */
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_02_FLEXCAN1_TX, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_03_FLEXCAN1_RX, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_02_FLEXCAN2_TX, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_03_FLEXCAN2_RX, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_14_FLEXCAN3_TX, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_15_FLEXCAN3_RX, 1U);

    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_02_FLEXCAN1_TX, PAD_CTL_GPIO_OUT);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_03_FLEXCAN1_RX, PAD_CTL_GPIO_OUT);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_02_FLEXCAN2_TX, PAD_CTL_GPIO_OUT);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_03_FLEXCAN2_RX, PAD_CTL_GPIO_OUT);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_14_FLEXCAN3_TX, PAD_CTL_GPIO_OUT);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_15_FLEXCAN3_RX, PAD_CTL_GPIO_OUT);

    /* Route each FlexCAN RX input to the board's actual pad instead of the
     * reset-default daisy source, otherwise the controller samples the wrong
     * pin and TX can remain stuck pending forever.
     */
    IOMUXC->SELECT_INPUT[kIOMUXC_FLEXCAN1_RX_SELECT_INPUT] = IOMUXC_SELECT_INPUT_DAISY(0x0U);
    IOMUXC->SELECT_INPUT[kIOMUXC_FLEXCAN2_RX_SELECT_INPUT] = IOMUXC_SELECT_INPUT_DAISY(0x1U);
    IOMUXC->SELECT_INPUT_1[kIOMUXC_CANFD_IPP_IND_CANRX_SELECT_INPUT] = IOMUXC_SELECT_INPUT_1_DAISY(0x1U);
}

/* 初始化 LPSPI1 引脚复用。 */
static void BSP_InitLpspi1PinMux(void)
{
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B0_00_LPSPI1_SCK, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B0_02_LPSPI1_SDO, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B0_03_LPSPI1_SDI, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B0_01_GPIO3_IO13, 0U);

    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B0_00_LPSPI1_SCK, PAD_CTL_GPIO_OUT);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B0_02_LPSPI1_SDO, PAD_CTL_GPIO_OUT);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B0_03_LPSPI1_SDI, PAD_CTL_GPIO_IN_PULLUP);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B0_01_GPIO3_IO13, PAD_CTL_GPIO_OUT);

    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_SD_B0_01_GPIO3_IO13, GPIO3, 13U, 1U);
}

/* 初始化本项目用到的关键外设时钟。 */
static void BSP_InitPeripheralClocks(void)
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);
    CLOCK_EnableClock(kCLOCK_IomuxcGpr);

    CLOCK_EnableClock(kCLOCK_Can1);
    CLOCK_EnableClock(kCLOCK_Can1S);
    CLOCK_EnableClock(kCLOCK_Can2);
    CLOCK_EnableClock(kCLOCK_Can2S);
    CLOCK_EnableClock(kCLOCK_Can3);
    CLOCK_EnableClock(kCLOCK_Can3S);

    CLOCK_EnableClock(kCLOCK_Lpspi1);
}

/* 板级外设总初始化入口。 */
void BSP_PeripheralsInit(void)
{
    BSP_ResetCanLedState();
    BSP_InitPeripheralClocks();
    BSP_SelectSharedGpioInstances();
    BSP_EnableSys3v3Rail();
    BSP_InitCanPinMux();
    BSP_InitLpspi1PinMux();
    BSP_InitControlGpioFromNetlist();
}
