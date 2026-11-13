#pragma once

#include <stddef.h>
#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backing store for LVGL's widget pool (LV_MEM_POOL_ALLOC). */
void *panda_lvgl_pool_alloc(size_t size);

#ifdef __cplusplus
}
#endif
