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

/* 文件说明：
 * 本文件只负责设备端启动编排：
 * 1. 完成芯片与板级基础初始化
 * 2. 打印启动诊断信息
 * 3. 把控制权交给 RTOS 应用层
 *
 * 如果设备上电后完全不进入任务系统，优先从这里确认启动顺序。 */

/* 设备端主入口。
 * 启动顺序：
 * 1. 板级时钟/引脚/调试串口
 * 2. RTOS 应用启动
 * 3. 进入调度器，不再返回 */
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
    PRINTF("CAN_APP start, core=%u ahb=%u ipg=%u can=%u lpspi_root=%u flexspi=%u uart=%u\r\n",
           BOARD_BOOTCLOCKRUN_CORE_CLOCK,
           BOARD_BOOTCLOCKRUN_AHB_CLK_ROOT,
           BOARD_BOOTCLOCKRUN_IPG_CLK_ROOT,
           BOARD_BOOTCLOCKRUN_CAN_CLK_ROOT,
           BOARD_BOOTCLOCKRUN_LPSPI_CLK_ROOT,
           BOARD_BOOTCLOCKRUN_FLEXSPI_CLK_ROOT,
           BOARD_BOOTCLOCKRUN_UART_CLK_ROOT);
    PRINTF("CAN_APP debug uart=LPUART1@115200, LED1-LED6 ready\r\n");

    RTOS_AppStart();

    return 0;
}
