#include "panda_state.h"

#include <stdio.h>
#include <string.h>

#define WAKE_MS 1200u
#define EMOTION_MS 2500u
/* Emotion parsed from an answer tag must outlive a 2.5 s idle flash: the face
 * is meant to carry the whole TTS turn, and the turn length is known only at
 * playback idle. */
#define TURN_EMOTION_MS 60000u
#define ERROR_MS 4000u
#define CONNECTING_MS 20000u
#define LISTENING_MS 15000u
#define THINKING_MS 20000u
#define CANCELLING_MS 400u

static void set_subtitle(panda_state_t *st, const char *text)
{
    if (text == NULL) {
        text = "";
    }
    const size_t cap = sizeof(st->subtitle);
    size_t len = strlen(text);
    if (len < cap) {
        memcpy(st->subtitle, text, len + 1);
        return;
    }
    /* Never cut inside a UTF-8 sequence: a broken trailing glyph shows up as
     * mojibake on the subtitle pill. Leave room for "…" (3 bytes) + NUL. */
    size_t keep = cap - 4;
    while (keep > 0 && ((unsigned char)text[keep] & 0xC0) == 0x80) {
        keep--;
    }
    memcpy(st->subtitle, text, keep);
    memcpy(st->subtitle + keep, "\xE2\x80\xA6", 3);
    st->subtitle[keep + 3] = '\0';
}

static void clear_subtitle(panda_state_t *st)
{
    st->subtitle[0] = '\0';
}

static bool low_battery(const panda_state_t *st)
{
    return st->battery_present && st->battery_percent >= 0 &&
           st->battery_percent <= 10 && !st->charging;
}

static void ready_or_low(panda_state_t *st)
{
    st->device = low_battery(st) ? PANDA_STATE_DEVICE_LOW_BATTERY
                                 : PANDA_STATE_DEVICE_READY;
}

static panda_state_screen_t error_screen(panda_state_error_t error)
{
    switch (error) {
    case PANDA_STATE_ERROR_ASR:
        return PANDA_STATE_SCREEN_ERROR_ASR;
    case PANDA_STATE_ERROR_TTS:
    case PANDA_STATE_ERROR_AUDIO:
        return PANDA_STATE_SCREEN_ERROR_TTS;
    case PANDA_STATE_ERROR_NETWORK:
        return PANDA_STATE_SCREEN_OFFLINE;
    case PANDA_STATE_ERROR_LLM:
    case PANDA_STATE_ERROR_AUTH:
    case PANDA_STATE_ERROR_GENERIC:
    default:
        return PANDA_STATE_SCREEN_ERROR_LLM;
    }
}

static bool emotion_active(const panda_state_t *st)
{
    return st->emotion != PANDA_STATE_EMOTION_NEUTRAL &&
           st->emotion_until != 0 && st->now_ms < st->emotion_until;
}

static panda_state_screen_t emotion_screen(const panda_state_t *st)
{
    switch (st->emotion) {
    case PANDA_STATE_EMOTION_HAPPY: return PANDA_STATE_SCREEN_FACE_HAPPY;
    case PANDA_STATE_EMOTION_CURIOUS: return PANDA_STATE_SCREEN_FACE_CURIOUS;
    case PANDA_STATE_EMOTION_SURPRISED: return PANDA_STATE_SCREEN_FACE_SURPRISED;
    case PANDA_STATE_EMOTION_SAD: return PANDA_STATE_SCREEN_FACE_SAD;
    case PANDA_STATE_EMOTION_SHY: return PANDA_STATE_SCREEN_FACE_SHY;
    case PANDA_STATE_EMOTION_SLEEPY: return PANDA_STATE_SCREEN_FACE_SLEEPY;
    case PANDA_STATE_EMOTION_ANGRY: return PANDA_STATE_SCREEN_FACE_ANGRY;
    default: return PANDA_STATE_SCREEN_IDLE;
    }
}

static panda_state_screen_t resolve_screen(const panda_state_t *st)
{
    if (!st->booted) {
        return PANDA_STATE_SCREEN_BOOT;
    }
    if (st->device == PANDA_STATE_DEVICE_SLEEP) {
        return PANDA_STATE_SCREEN_SLEEP;
    }
    if (st->device == PANDA_STATE_DEVICE_LOW_BATTERY) {
        return PANDA_STATE_SCREEN_LOW_BATTERY;
    }
    if (st->error_active) {
        return error_screen(st->error);
    }
    if (st->wake_until != 0 && st->now_ms < st->wake_until) {
        return PANDA_STATE_SCREEN_WAKE;
    }
    if (st->offline && !st->wifi_ap) {
        return PANDA_STATE_SCREEN_OFFLINE;
    }
    switch (st->voice) {
    case PANDA_STATE_VOICE_CONNECTING:
        return PANDA_STATE_SCREEN_RECONNECTING;
    case PANDA_STATE_VOICE_LISTENING:
        return PANDA_STATE_SCREEN_LISTENING;
    case PANDA_STATE_VOICE_THINKING:
        return PANDA_STATE_SCREEN_THINKING;
    case PANDA_STATE_VOICE_SPEAKING:
        /* Keep the answer's face on screen while its audio plays; the subtitle
         * is an overlay, so the pill still shows over the face. */
        return emotion_active(st) ? emotion_screen(st) : PANDA_STATE_SCREEN_SPEAKING;
    case PANDA_STATE_VOICE_CANCELLING:
        return PANDA_STATE_SCREEN_LISTENING;
    default:
        break;
    }
    if (emotion_active(st)) {
        return emotion_screen(st);
    }
    return PANDA_STATE_SCREEN_IDLE;
}

static const char *status_text(const panda_state_t *st)
{
    if (!st->booted) {
        return "启动中...";
    }
    if (st->device == PANDA_STATE_DEVICE_SLEEP) {
        return "";
    }
    if (st->device == PANDA_STATE_DEVICE_LOW_BATTERY) {
        return "电量低";
    }
    if (st->error_active) {
        switch (st->error) {
        case PANDA_STATE_ERROR_ASR: return "没听清，再说一次";
        case PANDA_STATE_ERROR_LLM: return "服务开小差了";
        case PANDA_STATE_ERROR_TTS: return "播放出错了";
        case PANDA_STATE_ERROR_AUDIO: return "音频出错了";
        case PANDA_STATE_ERROR_NETWORK: return "网络异常";
        case PANDA_STATE_ERROR_AUTH: return "服务暂不可用";
        default: return "出错了";
        }
    }
    if (st->offline && !st->wifi_ap) {
        return "网络未连接";
    }
    switch (st->voice) {
    case PANDA_STATE_VOICE_CONNECTING:
        return "连接中...";
    case PANDA_STATE_VOICE_LISTENING:
        return "在听...";
    case PANDA_STATE_VOICE_THINKING:
        return "思考中...";
    case PANDA_STATE_VOICE_SPEAKING:
        return "";
    default:
        break;
    }
    if (st->wifi_ap && !st->wifi_connected) {
        return "配网模式";
    }
    return "";
}

void panda_state_init(panda_state_t *st, uint32_t now_ms)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    st->device = PANDA_STATE_DEVICE_BOOT;
    st->voice = PANDA_STATE_VOICE_DISCONNECTED;
    st->emotion = PANDA_STATE_EMOTION_NEUTRAL;
    st->error = PANDA_STATE_ERROR_NONE;
    st->battery_percent = -1;
    st->now_ms = now_ms;
    st->revision = 1;
}

void panda_state_dispatch(panda_state_t *st, const panda_state_event_t *ev, uint32_t now_ms)
{
    if (st == NULL || ev == NULL) {
        return;
    }
    st->now_ms = now_ms;

    switch (ev->type) {
    case PANDA_STATE_EVT_BOOT:
        panda_state_init(st, now_ms);
        return;

    case PANDA_STATE_EVT_READY:
        st->booted = true;
        st->device = PANDA_STATE_DEVICE_READY;
        st->wake_until = 0;
        break;

    case PANDA_STATE_EVT_WAKE:
        st->booted = true;
        ready_or_low(st);
        st->ptt_active = false;
        st->error_active = false;
        st->error = PANDA_STATE_ERROR_NONE;
        st->wake_until = now_ms + WAKE_MS;
        clear_subtitle(st);
        break;

    case PANDA_STATE_EVT_SLEEP:
        st->device = PANDA_STATE_DEVICE_SLEEP;
        /* Sleep only changes presentation; the voice socket and Wi-Fi stay
         * as-is so a wake + PTT can resume without a reconnect. */
        st->voice = PANDA_STATE_VOICE_READY;
        st->ptt_active = false;
        st->error_active = false;
        st->error = PANDA_STATE_ERROR_NONE;
        st->wake_until = 0;
        st->connecting_until = 0;
        st->listening_until = 0;
        st->thinking_until = 0;
        st->cancelling_until = 0;
        clear_subtitle(st);
        break;

    case PANDA_STATE_EVT_PTT_DOWN:
        st->booted = true;
        if (st->device == PANDA_STATE_DEVICE_SLEEP) {
            ready_or_low(st);
        }
        st->error_active = false;
        st->error = PANDA_STATE_ERROR_NONE;
        st->ptt_active = true;
        st->emotion = PANDA_STATE_EMOTION_NEUTRAL;
        st->emotion_until = 0;
        clear_subtitle(st);
        if (st->voice == PANDA_STATE_VOICE_DISCONNECTED ||
            st->voice == PANDA_STATE_VOICE_CONNECTING) {
            st->voice = PANDA_STATE_VOICE_CONNECTING;
            st->connecting_until = now_ms + CONNECTING_MS;
        } else {
            st->voice = PANDA_STATE_VOICE_LISTENING;
            st->listening_until = now_ms + LISTENING_MS;
        }
        break;

    case PANDA_STATE_EVT_PTT_UP:
        st->ptt_active = false;
        st->listening_until = 0;
        if (st->voice == PANDA_STATE_VOICE_LISTENING ||
            st->voice == PANDA_STATE_VOICE_CANCELLING) {
            st->voice = PANDA_STATE_VOICE_THINKING;
            st->thinking_until = now_ms + THINKING_MS;
        }
        break;

    case PANDA_STATE_EVT_INTERRUPT:
        if (st->voice == PANDA_STATE_VOICE_SPEAKING ||
            st->voice == PANDA_STATE_VOICE_THINKING ||
            st->voice == PANDA_STATE_VOICE_CONNECTING) {
            st->voice = PANDA_STATE_VOICE_CANCELLING;
            st->cancelling_until = now_ms + CANCELLING_MS;
            st->ptt_active = true;
            st->error_active = false;
            clear_subtitle(st);
        }
        break;

    case PANDA_STATE_EVT_VOICE_DISCONNECTED:
        st->voice = PANDA_STATE_VOICE_DISCONNECTED;
        st->offline = true;
        st->ptt_active = false;
        st->connecting_until = 0;
        st->listening_until = 0;
        st->thinking_until = 0;
        clear_subtitle(st);
        break;

    case PANDA_STATE_EVT_VOICE_CONNECTING:
        st->voice = PANDA_STATE_VOICE_CONNECTING;
        st->offline = false;
        st->connecting_until = now_ms + CONNECTING_MS;
        clear_subtitle(st);
        break;

    case PANDA_STATE_EVT_VOICE_READY:
        st->voice = PANDA_STATE_VOICE_READY;
        st->offline = false;
        st->connecting_until = 0;
        break;

    case PANDA_STATE_EVT_VOICE_LISTENING:
        st->voice = PANDA_STATE_VOICE_LISTENING;
        st->offline = false;
        st->ptt_active = true;
        st->listening_until = now_ms + LISTENING_MS;
        st->connecting_until = 0;
        /* A new turn starts from the listening face; the previous answer's
         * emotion must not leak into it. */
        st->emotion = PANDA_STATE_EMOTION_NEUTRAL;
        st->emotion_until = 0;
        clear_subtitle(st);
        break;

    case PANDA_STATE_EVT_VOICE_THINKING:
        st->voice = PANDA_STATE_VOICE_THINKING;
        st->ptt_active = false;
        st->listening_until = 0;
        st->thinking_until = now_ms + THINKING_MS;
        break;

    case PANDA_STATE_EVT_VOICE_SPEAKING:
        st->voice = PANDA_STATE_VOICE_SPEAKING;
        st->ptt_active = false;
        st->listening_until = 0;
        st->thinking_until = 0;
        break;

    case PANDA_STATE_EVT_VOICE_CANCELLING:
        st->voice = PANDA_STATE_VOICE_CANCELLING;
        st->cancelling_until = now_ms + CANCELLING_MS;
        break;

    case PANDA_STATE_EVT_VOICE_IDLE:
        st->voice = PANDA_STATE_VOICE_READY;
        st->offline = false;
        st->ptt_active = false;
        st->error_active = false;
        st->error = PANDA_STATE_ERROR_NONE;
        st->connecting_until = 0;
        st->listening_until = 0;
        st->thinking_until = 0;
        st->cancelling_until = 0;
        /* Turn end: the answer's face has done its job, return to the idle
         * panda instead of holding the last emotion. */
        st->emotion = PANDA_STATE_EMOTION_NEUTRAL;
        st->emotion_until = 0;
        clear_subtitle(st);
        break;

    case PANDA_STATE_EVT_SUBTITLE:
        set_subtitle(st, ev->text);
        break;

    case PANDA_STATE_EVT_EMOTION: {
        st->emotion = (panda_state_emotion_t)ev->value;
        bool in_turn = st->voice == PANDA_STATE_VOICE_THINKING ||
                       st->voice == PANDA_STATE_VOICE_SPEAKING ||
                       st->voice == PANDA_STATE_VOICE_CANCELLING;
        st->emotion_until = (st->emotion == PANDA_STATE_EMOTION_NEUTRAL)
                                ? 0
                                : now_ms + (in_turn ? TURN_EMOTION_MS : EMOTION_MS);
        break;
    }

    case PANDA_STATE_EVT_WIFI:
        st->wifi_connected = ev->flag;
        st->wifi_ap = ev->flag2;
        st->wifi_connecting = ev->flag3;
        if (st->wifi_connected) {
            st->offline = false;
            if (st->voice == PANDA_STATE_VOICE_DISCONNECTED) {
                st->voice = PANDA_STATE_VOICE_CONNECTING;
                st->connecting_until = now_ms + CONNECTING_MS;
            }
        } else if (st->wifi_connecting) {
            st->offline = false;
            if (st->voice == PANDA_STATE_VOICE_DISCONNECTED) {
                st->voice = PANDA_STATE_VOICE_CONNECTING;
                st->connecting_until = now_ms + CONNECTING_MS;
            }
        } else if (st->wifi_ap) {
            st->offline = false;
            st->voice = PANDA_STATE_VOICE_DISCONNECTED;
            st->ptt_active = false;
        } else {
            st->offline = true;
            st->voice = PANDA_STATE_VOICE_DISCONNECTED;
            st->ptt_active = false;
            st->connecting_until = 0;
            clear_subtitle(st);
        }
        break;

    case PANDA_STATE_EVT_BATTERY:
        st->battery_present = ev->flag;
        st->charging = ev->flag2;
        if (ev->flag && ev->value >= 0) {
            st->battery_percent = ev->value;
            if (st->charging) {
                if (st->device == PANDA_STATE_DEVICE_LOW_BATTERY) {
                    st->device = PANDA_STATE_DEVICE_READY;
                }
            } else if (ev->value <= 10) {
                st->device = PANDA_STATE_DEVICE_LOW_BATTERY;
            } else if (ev->value > 15 && st->device == PANDA_STATE_DEVICE_LOW_BATTERY) {
                st->device = PANDA_STATE_DEVICE_READY;
            }
        } else if (!ev->flag && st->device == PANDA_STATE_DEVICE_LOW_BATTERY) {
            /* No ADC/pack information: keep the explicit low-battery state. */
        }
        break;

    case PANDA_STATE_EVT_ERROR:
        st->error = (panda_state_error_t)ev->value;
        st->error_active = true;
        st->error_until = now_ms + ERROR_MS;
        st->ptt_active = false;
        st->listening_until = 0;
        st->thinking_until = 0;
        st->voice = PANDA_STATE_VOICE_READY;
        clear_subtitle(st);
        break;

    case PANDA_STATE_EVT_CLEAR_ERROR:
        st->error_active = false;
        st->error = PANDA_STATE_ERROR_NONE;
        st->error_until = 0;
        break;

    default:
        break;
    }

    st->revision++;
}

void panda_state_tick(panda_state_t *st, uint32_t now_ms)
{
    if (st == NULL) {
        return;
    }
    st->now_ms = now_ms;

    if (st->connecting_until != 0 && now_ms >= st->connecting_until) {
        st->connecting_until = 0;
        if (st->voice == PANDA_STATE_VOICE_CONNECTING) {
            st->voice = PANDA_STATE_VOICE_DISCONNECTED;
            st->offline = true;
        }
    }

    if (st->cancelling_until != 0 && now_ms >= st->cancelling_until) {
        st->cancelling_until = 0;
        if (st->voice == PANDA_STATE_VOICE_CANCELLING) {
            st->voice = PANDA_STATE_VOICE_LISTENING;
            st->ptt_active = true;
            st->listening_until = now_ms + LISTENING_MS;
        }
    }

    if (st->listening_until != 0 && now_ms >= st->listening_until) {
        st->listening_until = 0;
        if (st->voice == PANDA_STATE_VOICE_LISTENING) {
            st->voice = PANDA_STATE_VOICE_THINKING;
            st->ptt_active = false;
            st->thinking_until = now_ms + THINKING_MS;
        }
    }

    if (st->thinking_until != 0 && now_ms >= st->thinking_until) {
        st->thinking_until = 0;
        if (st->voice == PANDA_STATE_VOICE_THINKING) {
            st->voice = PANDA_STATE_VOICE_READY;
            st->error = PANDA_STATE_ERROR_LLM;
            st->error_active = true;
            st->error_until = now_ms + ERROR_MS;
        }
    }

    if (st->error_active && st->error_until != 0 && now_ms >= st->error_until) {
        st->error_active = false;
        st->error = PANDA_STATE_ERROR_NONE;
        st->error_until = 0;
    }

    if (st->emotion_until != 0 && now_ms >= st->emotion_until) {
        st->emotion_until = 0;
        st->emotion = PANDA_STATE_EMOTION_NEUTRAL;
    }

    if (st->wake_until != 0 && now_ms >= st->wake_until) {
        st->wake_until = 0;
    }

    st->revision++;
}

void panda_state_view(const panda_state_t *st, panda_state_view_t *out)
{
    if (st == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->device = st->device;
    out->voice = st->voice;
    out->emotion = st->emotion;
    out->error = st->error;
    out->screen = resolve_screen(st);
    out->status = status_text(st);
    out->subtitle = st->subtitle;
    out->battery_percent = st->battery_percent;
    out->battery_present = st->battery_present;
    out->charging = st->charging;
    out->wifi_connected = st->wifi_connected;
    out->wifi_ap = st->wifi_ap;
    out->wifi_connecting = st->wifi_connecting;
    out->offline = st->offline;
    out->ptt_active = st->ptt_active;
    out->revision = st->revision;
}

const char *panda_state_screen_name(panda_state_screen_t screen)
{
    switch (screen) {
    case PANDA_STATE_SCREEN_BOOT: return "boot";
    case PANDA_STATE_SCREEN_WAKE: return "wake";
    case PANDA_STATE_SCREEN_IDLE: return "idle";
    case PANDA_STATE_SCREEN_LISTENING: return "listening";
    case PANDA_STATE_SCREEN_THINKING: return "thinking";
    case PANDA_STATE_SCREEN_SPEAKING: return "speaking";
    case PANDA_STATE_SCREEN_OFFLINE: return "offline";
    case PANDA_STATE_SCREEN_RECONNECTING: return "reconnecting";
    case PANDA_STATE_SCREEN_ERROR_ASR: return "error_asr";
    case PANDA_STATE_SCREEN_ERROR_LLM: return "error_llm";
    case PANDA_STATE_SCREEN_ERROR_TTS: return "error_tts";
    case PANDA_STATE_SCREEN_LOW_BATTERY: return "low_battery";
    case PANDA_STATE_SCREEN_SLEEP: return "sleep";
    case PANDA_STATE_SCREEN_FACE_HAPPY: return "face_happy";
    case PANDA_STATE_SCREEN_FACE_CURIOUS: return "face_curious";
    case PANDA_STATE_SCREEN_FACE_SURPRISED: return "face_surprised";
    case PANDA_STATE_SCREEN_FACE_SAD: return "face_sad";
    case PANDA_STATE_SCREEN_FACE_SHY: return "face_shy";
    case PANDA_STATE_SCREEN_FACE_SLEEPY: return "face_sleepy";
    case PANDA_STATE_SCREEN_FACE_ANGRY: return "face_angry";
    default: return "unknown";
    }
}

const char *panda_state_device_name(panda_state_device_t device)
{
    switch (device) {
    case PANDA_STATE_DEVICE_BOOT: return "boot";
    case PANDA_STATE_DEVICE_READY: return "ready";
    case PANDA_STATE_DEVICE_SLEEP: return "sleep";
    case PANDA_STATE_DEVICE_LOW_BATTERY: return "low_battery";
    default: return "unknown";
    }
}

const char *panda_state_voice_name(panda_state_voice_t voice)
{
    switch (voice) {
    case PANDA_STATE_VOICE_DISCONNECTED: return "disconnected";
    case PANDA_STATE_VOICE_CONNECTING: return "connecting";
    case PANDA_STATE_VOICE_READY: return "ready";
    case PANDA_STATE_VOICE_LISTENING: return "listening";
    case PANDA_STATE_VOICE_THINKING: return "thinking";
    case PANDA_STATE_VOICE_SPEAKING: return "speaking";
    case PANDA_STATE_VOICE_CANCELLING: return "cancelling";
    default: return "unknown";
    }
}

const char *panda_state_emotion_name(panda_state_emotion_t emotion)
{
    switch (emotion) {
    case PANDA_STATE_EMOTION_NEUTRAL: return "neutral";
    case PANDA_STATE_EMOTION_HAPPY: return "happy";
    case PANDA_STATE_EMOTION_CURIOUS: return "curious";
    case PANDA_STATE_EMOTION_SURPRISED: return "surprised";
    case PANDA_STATE_EMOTION_SAD: return "sad";
    case PANDA_STATE_EMOTION_SHY: return "shy";
    case PANDA_STATE_EMOTION_SLEEPY: return "sleepy";
    case PANDA_STATE_EMOTION_ANGRY: return "angry";
    default: return "unknown";
    }
}

const char *panda_state_error_name(panda_state_error_t error)
{
    switch (error) {
    case PANDA_STATE_ERROR_NONE: return "none";
    case PANDA_STATE_ERROR_ASR: return "asr";
    case PANDA_STATE_ERROR_LLM: return "llm";
    case PANDA_STATE_ERROR_TTS: return "tts";
    case PANDA_STATE_ERROR_NETWORK: return "network";
    case PANDA_STATE_ERROR_AUDIO: return "audio";
    case PANDA_STATE_ERROR_AUTH: return "auth";
    case PANDA_STATE_ERROR_GENERIC: return "generic";
    default: return "unknown";
    }
}
