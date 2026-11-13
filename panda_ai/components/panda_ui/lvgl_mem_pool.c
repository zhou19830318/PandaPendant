#include "lvgl_mem_pool.h"

void *panda_lvgl_pool_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
