#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANDA_STATE_SUBTITLE_MAX 512

/* PRD 8.3: device, voice and emotion are three independent dimensions. */
typedef enum {
    PANDA_STATE_DEVICE_BOOT = 0,
    PANDA_STATE_DEVICE_READY,
    PANDA_STATE_DEVICE_SLEEP,
    PANDA_STATE_DEVICE_LOW_BATTERY,
} panda_state_device_t;

typedef enum {
    PANDA_STATE_VOICE_DISCONNECTED = 0,
    PANDA_STATE_VOICE_CONNECTING,
    PANDA_STATE_VOICE_READY,
    PANDA_STATE_VOICE_LISTENING,
    PANDA_STATE_VOICE_THINKING,
    PANDA_STATE_VOICE_SPEAKING,
    PANDA_STATE_VOICE_CANCELLING,
} panda_state_voice_t;

typedef enum {
    PANDA_STATE_EMOTION_NEUTRAL = 0,
    PANDA_STATE_EMOTION_HAPPY,
    PANDA_STATE_EMOTION_CURIOUS,
    PANDA_STATE_EMOTION_SURPRISED,
    PANDA_STATE_EMOTION_SAD,
    PANDA_STATE_EMOTION_SHY,
    PANDA_STATE_EMOTION_SLEEPY,
    PANDA_STATE_EMOTION_ANGRY,
} panda_state_emotion_t;

typedef enum {
    PANDA_STATE_ERROR_NONE = 0,
    PANDA_STATE_ERROR_ASR,
    PANDA_STATE_ERROR_LLM,
    PANDA_STATE_ERROR_TTS,
    PANDA_STATE_ERROR_NETWORK,
    PANDA_STATE_ERROR_AUDIO,
    PANDA_STATE_ERROR_AUTH,
    PANDA_STATE_ERROR_GENERIC,
} panda_state_error_t;

/* Screens available in the SquareLine export. User pages (settings,
 * notification, previews) are navigated by generated UI and are not
 * resolved by the state machine. */
typedef enum {
    PANDA_STATE_SCREEN_BOOT = 0,
    PANDA_STATE_SCREEN_WAKE,
    PANDA_STATE_SCREEN_IDLE,
    PANDA_STATE_SCREEN_LISTENING,
    PANDA_STATE_SCREEN_THINKING,
    PANDA_STATE_SCREEN_SPEAKING,
    PANDA_STATE_SCREEN_OFFLINE,
    PANDA_STATE_SCREEN_RECONNECTING,
    PANDA_STATE_SCREEN_ERROR_ASR,
    PANDA_STATE_SCREEN_ERROR_LLM,
    PANDA_STATE_SCREEN_ERROR_TTS,
    PANDA_STATE_SCREEN_LOW_BATTERY,
    PANDA_STATE_SCREEN_SLEEP,
    PANDA_STATE_SCREEN_FACE_HAPPY,
    PANDA_STATE_SCREEN_FACE_CURIOUS,
    PANDA_STATE_SCREEN_FACE_SURPRISED,
    PANDA_STATE_SCREEN_FACE_SAD,
    PANDA_STATE_SCREEN_FACE_SHY,
    PANDA_STATE_SCREEN_FACE_SLEEPY,
    PANDA_STATE_SCREEN_FACE_ANGRY,
} panda_state_screen_t;

typedef enum {
    PANDA_STATE_EVT_BOOT = 0,
    PANDA_STATE_EVT_READY,          /* boot completed, device usable */
    PANDA_STATE_EVT_WAKE,
    PANDA_STATE_EVT_SLEEP,
    PANDA_STATE_EVT_PTT_DOWN,
    PANDA_STATE_EVT_PTT_UP,
    PANDA_STATE_EVT_INTERRUPT,
    PANDA_STATE_EVT_VOICE_DISCONNECTED,
    PANDA_STATE_EVT_VOICE_CONNECTING,
    PANDA_STATE_EVT_VOICE_READY,
    PANDA_STATE_EVT_VOICE_LISTENING,
    PANDA_STATE_EVT_VOICE_THINKING,
    PANDA_STATE_EVT_VOICE_SPEAKING,
    PANDA_STATE_EVT_VOICE_CANCELLING,
    PANDA_STATE_EVT_VOICE_IDLE,
    PANDA_STATE_EVT_SUBTITLE,
    PANDA_STATE_EVT_EMOTION,
    PANDA_STATE_EVT_WIFI,
    PANDA_STATE_EVT_BATTERY,
    PANDA_STATE_EVT_ERROR,
    PANDA_STATE_EVT_CLEAR_ERROR,
} panda_state_event_type_t;

typedef struct {
    panda_state_event_type_t type;
    int value;          /* emotion / error kind / battery percent */
    bool flag;          /* battery present, wifi connected */
    bool flag2;         /* battery charging, wifi soft-ap */
    bool flag3;         /* wifi connecting */
    const char *text;   /* subtitle */
} panda_state_event_t;

typedef struct {
    panda_state_device_t device;
    panda_state_voice_t voice;
    panda_state_emotion_t emotion;
    panda_state_error_t error;
    panda_state_screen_t screen;
    const char *status;     /* static string, may be "" */
    const char *subtitle;   /* internal buffer, may be "" */
    int battery_percent;
    bool battery_present;
    bool charging;
    bool wifi_connected;
    bool wifi_ap;
    bool wifi_connecting;
    bool offline;
    bool ptt_active;
    uint32_t revision;
} panda_state_view_t;

typedef struct {
    panda_state_device_t device;
    panda_state_voice_t voice;
    panda_state_emotion_t emotion;
    panda_state_error_t error;
    bool booted;
    bool offline;
    bool ptt_active;
    bool wifi_connected;
    bool wifi_ap;
    bool wifi_connecting;
    bool battery_present;
    bool charging;
    int battery_percent;
    bool error_active;
    uint32_t wake_until;
    uint32_t emotion_until;
    uint32_t error_until;
    uint32_t connecting_until;
    uint32_t listening_until;
    uint32_t thinking_until;
    uint32_t cancelling_until;
    uint32_t now_ms;
    uint32_t revision;
    char subtitle[PANDA_STATE_SUBTITLE_MAX];
} panda_state_t;

void panda_state_init(panda_state_t *st, uint32_t now_ms);
void panda_state_dispatch(panda_state_t *st, const panda_state_event_t *ev, uint32_t now_ms);
void panda_state_tick(panda_state_t *st, uint32_t now_ms);
void panda_state_view(const panda_state_t *st, panda_state_view_t *out);

const char *panda_state_screen_name(panda_state_screen_t screen);
const char *panda_state_device_name(panda_state_device_t device);
const char *panda_state_voice_name(panda_state_voice_t voice);
const char *panda_state_emotion_name(panda_state_emotion_t emotion);
const char *panda_state_error_name(panda_state_error_t error);

#ifdef __cplusplus
}
#endif
