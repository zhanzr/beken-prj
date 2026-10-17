#include "utils.h"
#include <os/os.h>
#include <components/system.h>
#include <stdarg.h>

/* milliseconds since boot (bk_get_tick counts ticks, scale to ms) */
uint32_t HAL_GetTick(void)
{
	return (uint32_t)(bk_get_tick() * bk_get_ms_per_tick());
}

void HAL_Delay(uint32_t t)
{
	rtos_delay_milliseconds(t);
}

void uart_printf(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}
