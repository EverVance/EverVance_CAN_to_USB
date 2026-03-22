#ifndef LPSPI1_BUS_H
#define LPSPI1_BUS_H

/* 文件说明：
 * 本头文件封装 LPSPI1 总线的最小访问接口，专门服务于 CH0 外置 MCP2517FD。
 * 它只负责“可靠地搬运 SPI 字节流”，不关心具体寄存器协议。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 初始化 LPSPI1，配置源时钟与目标波特率。 */
bool LPSPI1_BusInit(uint32_t srcClockHz, uint32_t busHz);
/** 执行一次全双工 SPI 传输。 */
bool LPSPI1_Transfer(const uint8_t *txData, uint8_t *rxData, size_t length);
/** 获取当前 LPSPI1 源时钟频率。 */
uint32_t LPSPI1_GetSourceClockHz(void);
/** 获取当前配置的 SPI 波特率。 */
uint32_t LPSPI1_GetConfiguredBaudHz(void);

#endif
