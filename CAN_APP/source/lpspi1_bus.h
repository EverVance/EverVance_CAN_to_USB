#ifndef LPSPI1_BUS_H
#define LPSPI1_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool LPSPI1_BusInit(uint32_t srcClockHz, uint32_t busHz);
bool LPSPI1_Transfer(const uint8_t *txData, uint8_t *rxData, size_t length);

#endif