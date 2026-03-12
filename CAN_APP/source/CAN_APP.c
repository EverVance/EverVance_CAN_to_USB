/*
 * Copyright 2016-2026 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file    CAN_APP.c
 * @brief   Application entry point.
 */
#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "bsp_peripherals.h"
#include "rtos_app.h"

int main(void)
{
    BOARD_ConfigMPU();
    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
    BOARD_InitDebugConsole();
#endif

    BSP_PeripheralsInit();
    BSP_SetStatusLeds(false);
    PRINTF("CAN_APP start, LPUART1 debug active @115200, LED4 untouched, launching FreeRTOS\r\n");

    RTOS_AppStart();

    return 0;
}
