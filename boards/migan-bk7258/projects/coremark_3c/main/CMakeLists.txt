list(APPEND ADD_INCLUDE "."
                        )

set(bench_srcs benchmark_main.c utils.h utils.c coremark.h core_list_join.c core_main.c core_matrix.c core_state.c core_util.c core_portme.h core_portme.c)
if (CONFIG_OVERRIDE_FLASH_PARTITION)
    list(APPEND bench_srcs vendor_flash.c)
endif()

armino_component_register(SRCS "${bench_srcs}" INCLUDE_DIRS "${incs}")

# Most aggressive optimization for the benchmark code only
# (same flag set as the reference projects: f4-demo / hlk-w800-kit)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Ofast
    -funroll-all-loops
    -fgcse-sm
    -finline-limit=500
    -fno-schedule-insns
    -Wno-maybe-uninitialized
    )
