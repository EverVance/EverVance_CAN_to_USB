#ifndef BSP_PERIPHERALS_H
#define BSP_PERIPHERALS_H

#include <stdbool.h>
#include <stdint.h>

#include "can_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    kBspStatusLed1 = 0,
    kBspStatusLed2,
    kBspStatusLed3,
    kBspStatusLed4,
    kBspStatusLed5,
    kBspStatusLed6,
    kBspStatusLedCount
} bsp_status_led_t;

/*
 * Initialize board-level peripheral controls derived from Hardware netlist:
 * - Power/enable/resistor control GPIOs for CAN/CAN-FD channels
 * - MCP interrupt GPIO inputs
 * - LED control GPIO outputs
 * - LPSPI1 and FLEXCAN1/2/3 clock + pin mux preparation
 */
void BSP_PeripheralsInit(void);
void BSP_SetStatusLeds(bool on);
void BSP_SetStatusLed(bsp_status_led_t led, bool on);
void BSP_RunStartupLedSweep(void);
void BSP_NotifyCanTxActivity(can_channel_t channel, uint32_t tickMs);
void BSP_NotifyCanRxActivity(can_channel_t channel, uint32_t tickMs);
void BSP_UpdateCanActivityLeds(uint32_t tickMs);
bool BSP_SetCanTermination(can_channel_t channel, bool enabled);
bool BSP_GetCanTermination(can_channel_t channel, bool *enabled);

#ifdef __cplusplus
}
#endif

#endif
