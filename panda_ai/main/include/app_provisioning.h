#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_provisioning_start(void);
void app_provisioning_stop(void);
bool app_provisioning_is_running(void);

#ifdef __cplusplus
}
#endif
