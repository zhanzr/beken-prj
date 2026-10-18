set(incs .
    D:/armino/beken-prj/vendor/bk_aidk/bk_avdk/components/bk_peripheral/include
    D:/armino/beken-prj/vendor/bk_aidk/bk_avdk/bk_idk/components/lwip_intf_v2_1/lwip-2.1.2/src/include
    D:/armino/beken-prj/vendor/bk_aidk/bk_avdk/bk_idk/components/lwip_intf_v2_1/lwip-2.1.2/port
    )
set(srcs
    http_server_main.c
    )

# vendor DVP sensor drivers (normally compiled via the bk_peripheral
# component, which is not registered in this project)
set(DVP_PERIPH D:/armino/beken-prj/vendor/bk_aidk/bk_avdk/components/bk_peripheral/src/dvp)
list(APPEND srcs
    ${DVP_PERIPH}/dvp_sensor_devices.c
    ${DVP_PERIPH}/dvp_gc2145.c
    ${DVP_PERIPH}/dvp_gc0308.c
    ${DVP_PERIPH}/dvp_gc0328c.c
    ${DVP_PERIPH}/dvp_hm1055.c
    ${DVP_PERIPH}/dvp_ov2640.c
    ${DVP_PERIPH}/dvp_sc101.c
    )

if (CONFIG_OVERRIDE_FLASH_PARTITION)
    list(APPEND srcs
        vendor_flash.c
        )
endif()

armino_component_register(SRCS "${srcs}" INCLUDE_DIRS "${incs}" REQUIRES lwip_intf_v2_1 PRIV_REQUIRES driver temp_detect)
