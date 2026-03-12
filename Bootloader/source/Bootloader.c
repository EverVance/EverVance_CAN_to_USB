/*
 * Copyright 2016-2026 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file    Bootloader.c
 * @brief   Application entry point.
 */
#include <stdio.h>
#include "board.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "peripherals.h"
#include "pin_mux.h"

extern void JumpToApp(void);

int main(void)
{
    BOARD_ConfigMPU();
    JumpToApp();

    /* No valid application was found, stay in bootloader mode. */
    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
    BOARD_InitDebugConsole();
#endif

    PRINTF("No valid CAN_APP image, bootloader stays active.\r\n");

    while (1)
    {
        __asm volatile("wfi");
    }

    return 0;
}
