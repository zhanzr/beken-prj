#include <stdio.h>

#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include "gpio_driver.h"

#if (CONFIG_SYS_CPU0)

/* R1 board: LED1 -> P40, LED2 -> P41, both high-active.
 * NOTE: both pins default-mux to the RGB LCD function (LCD_G4/G3,
 * see middleware/soc/bk7258/soc/gpio_map.h) and must be unmapped
 * back to plain GPIO before use. */
#define LED1_GPIO	GPIO_40
#define LED2_GPIO	GPIO_41
#define BLINK_PERIOD_MS	500

/* User application thread, spawned by the system after bk_init() */
void user_app_main(void)
{
	bk_gpio_driver_init();

	gpio_dev_unmap(LED1_GPIO);
	gpio_dev_unmap(LED2_GPIO);

	bk_gpio_enable_output(LED1_GPIO);
	bk_gpio_enable_output(LED2_GPIO);

	/* both off to start */
	bk_gpio_set_output_low(LED1_GPIO);
	bk_gpio_set_output_low(LED2_GPIO);

	printf("Hello World from BK7258! UART0/CH340 console, LED1=P40, LED2=P41 (high active)\r\n");

	int count = 0;

	while (1) {
		/* alternate the two high-active LEDs */
		bk_gpio_set_output_high(LED1_GPIO);
		bk_gpio_set_output_low(LED2_GPIO);
		rtos_delay_milliseconds(BLINK_PERIOD_MS);

		bk_gpio_set_output_low(LED1_GPIO);
		bk_gpio_set_output_high(LED2_GPIO);
		rtos_delay_milliseconds(BLINK_PERIOD_MS);

		printf("Hello World from BK7258 CPU0! (toggle %d)\r\n", ++count);
	}
}

#else /* CONFIG_SYS_CPU1 / CONFIG_SYS_CPU2: keep slave cores idle */

void user_app_main(void)
{
}

#endif

int main(void)
{
#if (CONFIG_SYS_CPU0)
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();

	return 0;
}
