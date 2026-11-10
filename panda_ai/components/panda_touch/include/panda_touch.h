#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANDA_TOUCH_TAP = 0,
    PANDA_TOUCH_DOUBLE_TAP,
    PANDA_TOUCH_LONG_PRESS,
    PANDA_TOUCH_SWIPE_LEFT,
    PANDA_TOUCH_SWIPE_RIGHT,
    PANDA_TOUCH_SWIPE_UP,
    PANDA_TOUCH_SWIPE_DOWN,
} panda_touch_gesture_t;

typedef void (*panda_touch_gesture_cb_t)(panda_touch_gesture_t gesture, int x, int y);

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
    uint32_t tap_count;
    uint32_t gesture_count;
    panda_touch_gesture_t last_gesture;
    int last_gesture_x;
    int last_gesture_y;
} panda_touch_state_t;

esp_err_t panda_touch_init(void);
void panda_touch_diag(void);
void panda_touch_set_callback(panda_touch_gesture_cb_t cb);
const panda_touch_state_t *panda_touch_get_state(void);
const char *panda_touch_gesture_name(panda_touch_gesture_t gesture);

#ifdef __cplusplus
}
#endif
