#ifndef __CUSTOM_DEF_H__
#define __CUSTOM_DEF_H__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <os/mem.h>

#include "utils.h"

/* Dhrystone allocates its two records via malloc: use the OS heap */
#define malloc	os_malloc

#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ	1000
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define COMPILER_FLAGS "-Ofast -funroll-all-loops -fgcse-sm -finline-limit=500 -fno-schedule-insns"

#ifdef __GNUC__
    #define COMPILER_NAME "GCC " __VERSION__
#else
    #define COMPILER_NAME "Unknown Compiler"
#endif

#endif
