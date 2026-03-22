#ifndef RTOS_APP_H
#define RTOS_APP_H

/* 文件说明：
 * 本头文件暴露 RTOS 应用层启动入口。
 * RTOS 应用层负责创建任务、组织 USB/CAN/LED 等后台执行链。 */

/** 启动 RTOS 应用，创建任务并开始调度。 */
void RTOS_AppStart(void);

#endif
