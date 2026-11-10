/*
 * Host-side UI state-machine simulator.
 *
 * This compiles the same panda_state.c that the firmware uses, drives it with
 * scripted events, and asserts the resulting screen/overlay state. It does not
 * render LVGL; the device build maps the resolved screen names to real screens.
 *
 * Run: make -C tools/ui_sim test
 */
#include "panda_state.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static panda_state_t s_state;
static uint32_t s_now;
static int s_checks;
static int s_failures;

static const char *event_name(panda_state_event_type_t type)
{
    switch (type) {
    case PANDA_STATE_EVT_BOOT: return "BOOT";
    case PANDA_STATE_EVT_READY: return "READY";
    case PANDA_STATE_EVT_WAKE: return "WAKE";
    case PANDA_STATE_EVT_SLEEP: return "SLEEP";
    case PANDA_STATE_EVT_PTT_DOWN: return "PTT_DOWN";
    case PANDA_STATE_EVT_PTT_UP: return "PTT_UP";
    case PANDA_STATE_EVT_INTERRUPT: return "INTERRUPT";
    case PANDA_STATE_EVT_VOICE_DISCONNECTED: return "VOICE_DISCONNECTED";
    case PANDA_STATE_EVT_VOICE_CONNECTING: return "VOICE_CONNECTING";
    case PANDA_STATE_EVT_VOICE_READY: return "VOICE_READY";
    case PANDA_STATE_EVT_VOICE_LISTENING: return "VOICE_LISTENING";
    case PANDA_STATE_EVT_VOICE_THINKING: return "VOICE_THINKING";
    case PANDA_STATE_EVT_VOICE_SPEAKING: return "VOICE_SPEAKING";
    case PANDA_STATE_EVT_VOICE_CANCELLING: return "VOICE_CANCELLING";
    case PANDA_STATE_EVT_VOICE_IDLE: return "VOICE_IDLE";
    case PANDA_STATE_EVT_SUBTITLE: return "SUBTITLE";
    case PANDA_STATE_EVT_EMOTION: return "EMOTION";
    case PANDA_STATE_EVT_WIFI: return "WIFI";
    case PANDA_STATE_EVT_BATTERY: return "BATTERY";
    case PANDA_STATE_EVT_ERROR: return "ERROR";
    case PANDA_STATE_EVT_CLEAR_ERROR: return "CLEAR_ERROR";
    default: return "?";
    }
}

static void print_view(const char *action)
{
    panda_state_view_t v;
    panda_state_view(&s_state, &v);
    printf("t=%6u  %-22s screen=%-14s device=%-10s voice=%-12s emotion=%-9s"
           " status=\"%s\" subtitle=\"%s\"\n",
           (unsigned)s_now, action,
           panda_state_screen_name(v.screen),
           panda_state_device_name(v.device),
           panda_state_voice_name(v.voice),
           panda_state_emotion_name(v.emotion),
           v.status ? v.status : "", v.subtitle ? v.subtitle : "");
}

static void check(bool ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static panda_state_view_t view(void)
{
    panda_state_view_t v;
    panda_state_view(&s_state, &v);
    return v;
}

static void expect_screen(panda_state_screen_t screen, const char *what)
{
    panda_state_view_t v = view();
    check(v.screen == screen, what);
}

static void dispatch(panda_state_event_type_t type, int value, const char *text)
{
    panda_state_event_t ev = {.type = type, .value = value, .text = text};
    panda_state_dispatch(&s_state, &ev, s_now);
    char label[64];
    snprintf(label, sizeof(label), "%s %d", event_name(type), value);
    print_view(label);
}

static void dispatch_wifi(bool connected, bool ap, bool connecting)
{
    panda_state_event_t ev = {
        .type = PANDA_STATE_EVT_WIFI,
        .flag = connected, .flag2 = ap, .flag3 = connecting,
    };
    panda_state_dispatch(&s_state, &ev, s_now);
    char label[64];
    snprintf(label, sizeof(label), "WIFI c=%d ap=%d ing=%d", connected, ap, connecting);
    print_view(label);
}

static void dispatch_battery(int pct, bool present, bool charging)
{
    panda_state_event_t ev = {
        .type = PANDA_STATE_EVT_BATTERY,
        .value = pct, .flag = present, .flag2 = charging,
    };
    panda_state_dispatch(&s_state, &ev, s_now);
    char label[64];
    snprintf(label, sizeof(label), "BATTERY %d%% present=%d chg=%d", pct, present, charging);
    print_view(label);
}

static void advance(uint32_t ms, const char *what)
{
    s_now += ms;
    panda_state_tick(&s_state, s_now);
    print_view(what);
}

int main(void)
{
    printf("Panda UI state-machine simulation\n");
    panda_state_init(&s_state, 0);
    print_view("init");
    expect_screen(PANDA_STATE_SCREEN_BOOT, "init shows boot");
    check(view().device == PANDA_STATE_DEVICE_BOOT, "init device boot");

    s_now = 100;
    dispatch(PANDA_STATE_EVT_READY, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_IDLE, "boot done -> idle");

    dispatch_wifi(true, false, false);
    expect_screen(PANDA_STATE_SCREEN_RECONNECTING, "wifi up -> reconnecting");
    dispatch(PANDA_STATE_EVT_VOICE_READY, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_IDLE, "voice ready -> idle");

    dispatch(PANDA_STATE_EVT_EMOTION, PANDA_STATE_EMOTION_HAPPY, NULL);
    expect_screen(PANDA_STATE_SCREEN_FACE_HAPPY, "emotion happy -> face screen");
    s_now += 3000;
    advance(0, "emotion timeout");
    expect_screen(PANDA_STATE_SCREEN_IDLE, "emotion expired -> idle");

    dispatch(PANDA_STATE_EVT_PTT_DOWN, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_LISTENING, "ptt down -> listening");
    dispatch(PANDA_STATE_EVT_SUBTITLE, 0, "你好，熊猫");
    dispatch(PANDA_STATE_EVT_PTT_UP, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_THINKING, "ptt up -> thinking");
    dispatch(PANDA_STATE_EVT_SUBTITLE, 0, "你好呀");
    dispatch(PANDA_STATE_EVT_VOICE_SPEAKING, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_SPEAKING, "tts -> speaking");
    dispatch(PANDA_STATE_EVT_VOICE_IDLE, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_IDLE, "response done -> idle");
    check(view().subtitle[0] == '\0', "idle clears subtitle");

    dispatch(PANDA_STATE_EVT_PTT_DOWN, 0, NULL);
    dispatch(PANDA_STATE_EVT_PTT_UP, 0, NULL);
    dispatch(PANDA_STATE_EVT_VOICE_SPEAKING, 0, NULL);
    dispatch(PANDA_STATE_EVT_INTERRUPT, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_LISTENING, "interrupt -> cancelling renders listening");
    advance(500, "cancel ack");
    expect_screen(PANDA_STATE_SCREEN_LISTENING, "cancelled -> listening");
    dispatch(PANDA_STATE_EVT_VOICE_IDLE, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_IDLE, "cancel turn -> idle");

    dispatch_wifi(false, false, false);
    expect_screen(PANDA_STATE_SCREEN_OFFLINE, "wifi down -> offline");
    check(view().voice == PANDA_STATE_VOICE_DISCONNECTED, "wifi down disconnects voice");
    dispatch_wifi(true, false, false);
    expect_screen(PANDA_STATE_SCREEN_RECONNECTING, "wifi recovered -> reconnecting");
    dispatch(PANDA_STATE_EVT_VOICE_READY, 0, NULL);

    dispatch(PANDA_STATE_EVT_ERROR, PANDA_STATE_ERROR_LLM, NULL);
    expect_screen(PANDA_STATE_SCREEN_ERROR_LLM, "llm error -> error screen");
    check(strcmp(view().status, "服务开小差了") == 0, "llm error status");
    advance(4100, "error timeout");
    expect_screen(PANDA_STATE_SCREEN_IDLE, "error expired -> idle");

    dispatch(PANDA_STATE_EVT_ERROR, PANDA_STATE_ERROR_ASR, NULL);
    expect_screen(PANDA_STATE_SCREEN_ERROR_ASR, "asr error -> asr screen");
    check(strcmp(view().status, "没听清，再说一次") == 0, "asr error status");
    advance(4100, "asr error timeout");
    expect_screen(PANDA_STATE_SCREEN_IDLE, "asr error expired -> idle");

    dispatch(PANDA_STATE_EVT_ERROR, PANDA_STATE_ERROR_TTS, NULL);
    expect_screen(PANDA_STATE_SCREEN_ERROR_TTS, "tts error -> tts screen");
    advance(4100, "tts error timeout");
    dispatch(PANDA_STATE_EVT_ERROR, PANDA_STATE_ERROR_AUTH, NULL);
    expect_screen(PANDA_STATE_SCREEN_ERROR_LLM, "auth error -> service screen");
    check(strcmp(view().status, "服务暂不可用") == 0, "auth error status");
    advance(4100, "auth error timeout");
    expect_screen(PANDA_STATE_SCREEN_IDLE, "auth error expired -> idle");

    dispatch(PANDA_STATE_EVT_ERROR, PANDA_STATE_ERROR_NETWORK, NULL);
    expect_screen(PANDA_STATE_SCREEN_OFFLINE, "network error -> offline screen");
    advance(4100, "network error timeout");
    expect_screen(PANDA_STATE_SCREEN_IDLE, "network error expired -> idle");

    dispatch_battery(8, true, false);
    expect_screen(PANDA_STATE_SCREEN_LOW_BATTERY, "battery 8% -> low battery");
    check(strcmp(view().status, "电量低") == 0, "low battery status");
    dispatch_battery(80, true, false);
    expect_screen(PANDA_STATE_SCREEN_IDLE, "battery recovered -> idle");
    dispatch_battery(10, true, false);
    expect_screen(PANDA_STATE_SCREEN_LOW_BATTERY, "explicit low -> low battery");
    dispatch(PANDA_STATE_EVT_READY, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_IDLE, "ready clears explicit low battery");
    dispatch_battery(80, true, false);

    dispatch(PANDA_STATE_EVT_SLEEP, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_SLEEP, "sleep screen");
    dispatch(PANDA_STATE_EVT_WAKE, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_WAKE, "wake screen");
    advance(1300, "wake timeout");
    expect_screen(PANDA_STATE_SCREEN_IDLE, "wake -> idle");

    dispatch(PANDA_STATE_EVT_PTT_DOWN, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_LISTENING, "ptt after wake -> listening");
    advance(16000, "listen timeout");
    expect_screen(PANDA_STATE_SCREEN_THINKING, "listen timeout -> thinking");
    dispatch(PANDA_STATE_EVT_VOICE_IDLE, 0, NULL);

    dispatch(PANDA_STATE_EVT_VOICE_CONNECTING, 0, NULL);
    expect_screen(PANDA_STATE_SCREEN_RECONNECTING, "voice connecting -> reconnecting");
    advance(21000, "connect timeout");
    expect_screen(PANDA_STATE_SCREEN_OFFLINE, "connect timeout -> offline");

    dispatch_wifi(true, false, false);
    dispatch(PANDA_STATE_EVT_VOICE_READY, 0, NULL);
    dispatch_battery(80, true, false);
    dispatch(PANDA_STATE_EVT_VOICE_SPEAKING, 0, NULL);
    dispatch_battery(5, true, false);
    expect_screen(PANDA_STATE_SCREEN_LOW_BATTERY, "low battery overrides speaking");
    dispatch_battery(80, true, false);
    expect_screen(PANDA_STATE_SCREEN_SPEAKING, "battery recovered restores speaking");

    panda_state_init(&s_state, s_now);
    dispatch(PANDA_STATE_EVT_READY, 0, NULL);
    dispatch_wifi(false, true, false);
    expect_screen(PANDA_STATE_SCREEN_IDLE, "provisioning AP -> idle");
    check(strcmp(view().status, "配网模式") == 0, "provisioning status");

    printf("\n%d checks, %d failure(s)\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
