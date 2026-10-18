set(incs .
    ${armino_path}/components/lwip_intf_v2_1/lwip-2.1.2
    ${armino_path}/components/lwip_intf_v2_1/lwip-2.1.2/src
    ${armino_path}/components/lwip_intf_v2_1/lwip-2.1.2/src/include
    ${armino_path}/components/lwip_intf_v2_1/lwip-2.1.2/src/include/lwip
    ${armino_path}/components/lwip_intf_v2_1/lwip-2.1.2/port
    )
set(srcs
    http_server_main.c
    )

if (CONFIG_OVERRIDE_FLASH_PARTITION)
    list(APPEND srcs
        vendor_flash.c
        )
endif()

armino_component_register(SRCS "${srcs}" INCLUDE_DIRS "${incs}" REQUIRES lwip_intf_v2_1 PRIV_REQUIRES driver temp_detect)
