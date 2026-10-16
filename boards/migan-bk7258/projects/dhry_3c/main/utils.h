#ifndef _BENCH_UTILS_H
#define _BENCH_UTILS_H

#include <stdint.h>
#include <stdio.h>

/* benchmark code is placed in flash (XIP), no special RAM section */
#define RAM_FUNC

/* the benchmark port measures time in milliseconds and uses
 * HZ = 1000 regardless of the RTOS tick rate (HAL_GetTick scales) */
#define configTICK_RATE_HZ	1000

#define PRINTF(...)		printf(__VA_ARGS__)

#define RAM_FUNC_BENCH		RAM_FUNC

uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t t);
void uart_printf(const char *fmt, ...);

#endif
