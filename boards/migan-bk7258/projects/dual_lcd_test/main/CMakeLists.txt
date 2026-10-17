set(incs .)
set(srcs
    dual_lcd_test_main.c
    )

if (CONFIG_OVERRIDE_FLASH_PARTITION)
    list(APPEND srcs
        vendor_flash.c
        )
endif()

armino_component_register(SRCS "${srcs}" INCLUDE_DIRS "${incs}")
