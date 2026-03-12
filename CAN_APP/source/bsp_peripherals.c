#include "bsp_peripherals.h"

#include "fsl_common.h"
#include "fsl_clock.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"

#define PAD_CTL_GPIO_OUT (0x10B0U)
#define PAD_CTL_GPIO_STRONG_PP \
    (IOMUXC_SW_PAD_CTL_PAD_SRE(1U) | IOMUXC_SW_PAD_CTL_PAD_DSE(7U) | IOMUXC_SW_PAD_CTL_PAD_SPEED(3U))
#define PAD_CTL_GPIO_IN_PULLUP (0x01B0B0U)
#define GPIO_MUX2_USED_MASK (0xD73101FFU)
#define GPIO_MUX3_USED_MASK (0x00002000U)
#define SYS_3V3_ENABLE_SETTLE_US (5000U)
#define STARTUP_LED_STEP_MS (120U)
#define CAN_ACTIVITY_HOLD_MS (60U)

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
    kBspStatusLed5,
    kBspStatusLed6,
};
static uint32_t s_CanLedExpireTickMs[kBspStatusLedCount];

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
    /* Do not touch LED4 pin. Hardware netlist ties it unsafely with LED1 control,
     * so configuring or driving GPIO2_IO26 risks electrical mismatch. */
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_05_GPIO2_IO21, GPIO2, 21U, 1U);
    BSP_CONFIG_GPIO_OUTPUT(IOMUXC_GPIO_B1_04_GPIO2_IO20, GPIO2, 20U, 1U);
}

static void BSP_EnableSys3v3Rail(void)
{
    const gpio_pin_config_t cfg = GPIO_OUT_INIT(1U);

    IOMUXC_SetPinMux(IOMUXC_GPIO_B0_06_GPIO2_IO06, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B0_06_GPIO2_IO06, PAD_CTL_GPIO_STRONG_PP);
    GPIO_PinInit(GPIO2, 6U, &cfg);
    SDK_DelayAtLeastUs(SYS_3V3_ENABLE_SETTLE_US, SystemCoreClock);
}

static void BSP_SelectSharedGpioInstances(void)
{
    /* GPIO_B0/B1 are shared between GPIO2/GPIO7, while GPIO_SD_B0 can be
     * shared between GPIO3/GPIO8. Force the pins used by this board netlist
     * onto the GPIO instances we actually drive in software. */
    IOMUXC_GPR->GPR27 &= ~GPIO_MUX2_USED_MASK;
    IOMUXC_GPR->GPR28 &= ~GPIO_MUX3_USED_MASK;
}

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

bool BSP_GetCanTermination(can_channel_t channel, bool *enabled)
{
    if (((uint8_t)channel >= (uint8_t)kCanChannel_Count) || (enabled == NULL))
    {
        return false;
    }

    *enabled = (GPIO_PinRead(GPIO2, s_CanTerminationPins[channel]) != 0U);
    return true;
}

void BSP_SetStatusLeds(bool on)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)(sizeof(s_ActiveStatusLeds) / sizeof(s_ActiveStatusLeds[0])); i++)
    {
        BSP_SetStatusLed(s_ActiveStatusLeds[i], on);
    }
}

void BSP_SetStatusLed(bsp_status_led_t led, bool on)
{
    uint8_t level;

    if ((uint32_t)led >= (uint32_t)kBspStatusLedCount)
    {
        return;
    }

    if (led == kBspStatusLed4)
    {
        return;
    }

    /* Netlist shows the LED current path comes from VCC_SYS_3V3 through resistors,
     * so the MCU GPIOs sink current and are active-low. */
    level = on ? 0U : 1U;
    GPIO_PinWrite(GPIO2, s_StatusLedPins[led], level);
}

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

static void BSP_MarkLedActivity(bsp_status_led_t led, uint32_t tickMs)
{
    if ((uint32_t)led >= (uint32_t)kBspStatusLedCount)
    {
        return;
    }
    if (led == kBspStatusLed4)
    {
        return;
    }

    s_CanLedExpireTickMs[led] = tickMs + CAN_ACTIVITY_HOLD_MS;
}

void BSP_NotifyCanTxActivity(can_channel_t channel, uint32_t tickMs)
{
    switch (channel)
    {
        case kCanChannel_CanFd1Ext:
            BSP_MarkLedActivity(kBspStatusLed2, tickMs);
            break;
        case kCanChannel_Can2:
            BSP_MarkLedActivity(kBspStatusLed3, tickMs);
            break;
        default:
            break;
    }
}

void BSP_NotifyCanRxActivity(can_channel_t channel, uint32_t tickMs)
{
    switch (channel)
    {
        case kCanChannel_CanFd1Ext:
            BSP_MarkLedActivity(kBspStatusLed1, tickMs);
            break;
        case kCanChannel_Can2:
            BSP_MarkLedActivity(kBspStatusLed3, tickMs);
            break;
        default:
            break;
    }
}

void BSP_UpdateCanActivityLeds(uint32_t tickMs)
{
    BSP_SetStatusLed(kBspStatusLed1, (int32_t)(s_CanLedExpireTickMs[kBspStatusLed1] - tickMs) > 0);
    BSP_SetStatusLed(kBspStatusLed2, (int32_t)(s_CanLedExpireTickMs[kBspStatusLed2] - tickMs) > 0);
    BSP_SetStatusLed(kBspStatusLed3, (int32_t)(s_CanLedExpireTickMs[kBspStatusLed3] - tickMs) > 0);
}

static void BSP_InitCanPinMux(void)
{
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_17_FLEXCAN1_TX, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_18_FLEXCAN1_RX, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_02_FLEXCAN2_TX, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_03_FLEXCAN2_RX, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_14_FLEXCAN3_TX, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_15_FLEXCAN3_RX, 0U);
}

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

void BSP_PeripheralsInit(void)
{
    BSP_InitPeripheralClocks();
    BSP_SelectSharedGpioInstances();
    BSP_EnableSys3v3Rail();
    BSP_InitCanPinMux();
    BSP_InitLpspi1PinMux();
    BSP_InitControlGpioFromNetlist();
}
