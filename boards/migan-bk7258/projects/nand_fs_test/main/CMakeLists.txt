set(incs . D:/armino/beken-prj/vendor/bk_aidk/bk_avdk/bk_idk/components/fatfs)
set(srcs
    nand_fs_test_main.c
    )

if (CONFIG_OVERRIDE_FLASH_PARTITION)
    list(APPEND srcs
        vendor_flash.c
        )
endif()

armino_component_register(SRCS "${srcs}" INCLUDE_DIRS "${incs}" REQUIRES fatfs driver)
