/*
 * JavaScript port of panda_ai/components/panda_state/panda_state.c.
 * Keep this file in lock-step with the C implementation: the HTML demo runs
 * exactly these rules, and tools/ui_sim/web/test_state.js replays the C
 * simulator's 46 assertions against it.
 */
(function (global) {
  "use strict";

  const WAKE_MS = 1200;
  const EMOTION_MS = 2500;
  const ERROR_MS = 4000;
  const CONNECTING_MS = 20000;
  const LISTENING_MS = 15000;
  const THINKING_MS = 20000;
  const CANCELLING_MS = 400;

  const DEVICE = {
    BOOT: 0, READY: 1, SLEEP: 2, LOW_BATTERY: 3,
  };
  const VOICE = {
    DISCONNECTED: 0, CONNECTING: 1, READY: 2, LISTENING: 3,
    THINKING: 4, SPEAKING: 5, CANCELLING: 6,
  };
  const EMOTION = {
    NEUTRAL: 0, HAPPY: 1, CURIOUS: 2, SURPRISED: 3,
    SAD: 4, SHY: 5, SLEEPY: 6, ANGRY: 7,
  };
  const ERROR = {
    NONE: 0, ASR: 1, LLM: 2, TTS: 3, NETWORK: 4,
    AUDIO: 5, AUTH: 6, GENERIC: 7,
  };
  const SCREEN = {
    BOOT: 0, WAKE: 1, IDLE: 2, LISTENING: 3, THINKING: 4,
    SPEAKING: 5, OFFLINE: 6, RECONNECTING: 7, ERROR_ASR: 8,
    ERROR_LLM: 9, ERROR_TTS: 10, LOW_BATTERY: 11, SLEEP: 12,
    FACE_HAPPY: 13, FACE_CURIOUS: 14, FACE_SURPRISED: 15,
    FACE_SAD: 16, FACE_SHY: 17, FACE_SLEEPY: 18, FACE_ANGRY: 19,
  };
  const EVT = {
    BOOT: 0, READY: 1, WAKE: 2, SLEEP: 3, PTT_DOWN: 4, PTT_UP: 5,
    INTERRUPT: 6, VOICE_DISCONNECTED: 7, VOICE_CONNECTING: 8,
    VOICE_READY: 9, VOICE_LISTENING: 10, VOICE_THINKING: 11,
    VOICE_SPEAKING: 12, VOICE_CANCELLING: 13, VOICE_IDLE: 14,
    SUBTITLE: 15, EMOTION: 16, WIFI: 17, BATTERY: 18,
    ERROR: 19, CLEAR_ERROR: 20,
  };

  const SCREEN_NAMES = [
    "boot", "wake", "idle", "listening", "thinking", "speaking", "offline",
    "reconnecting", "error_asr", "error_llm", "error_tts", "low_battery",
    "sleep", "face_happy", "face_curious", "face_surprised", "face_sad",
    "face_shy", "face_sleepy", "face_angry",
  ];
  const DEVICE_NAMES = ["boot", "ready", "sleep", "low_battery"];
  const VOICE_NAMES = [
    "disconnected", "connecting", "ready", "listening", "thinking",
    "speaking", "cancelling",
  ];
  const EMOTION_NAMES = [
    "neutral", "happy", "curious", "surprised", "sad", "shy", "sleepy", "angry",
  ];
  const ERROR_NAMES = [
    "none", "asr", "llm", "tts", "network", "audio", "auth", "generic",
  ];
  const EVENT_NAMES = [
    "BOOT", "READY", "WAKE", "SLEEP", "PTT_DOWN", "PTT_UP", "INTERRUPT",
    "VOICE_DISCONNECTED", "VOICE_CONNECTING", "VOICE_READY", "VOICE_LISTENING",
    "VOICE_THINKING", "VOICE_SPEAKING", "VOICE_CANCELLING", "VOICE_IDLE",
    "SUBTITLE", "EMOTION", "WIFI", "BATTERY", "ERROR", "CLEAR_ERROR",
  ];

  function lowBattery(st) {
    return st.battery_present && st.battery_percent >= 0 &&
      st.battery_percent <= 10 && !st.charging;
  }

  function readyOrLow(st) {
    st.device = lowBattery(st) ? DEVICE.LOW_BATTERY : DEVICE.READY;
  }

  function errorScreen(error) {
    if (error === ERROR.ASR) return SCREEN.ERROR_ASR;
    if (error === ERROR.TTS || error === ERROR.AUDIO) return SCREEN.ERROR_TTS;
    if (error === ERROR.NETWORK) return SCREEN.OFFLINE;
    return SCREEN.ERROR_LLM;
  }

  function resolveScreen(st) {
    if (!st.booted) return SCREEN.BOOT;
    if (st.device === DEVICE.SLEEP) return SCREEN.SLEEP;
    if (st.device === DEVICE.LOW_BATTERY) return SCREEN.LOW_BATTERY;
    if (st.error_active) return errorScreen(st.error);
    if (st.wake_until !== 0 && st.now_ms < st.wake_until) return SCREEN.WAKE;
    if (st.offline && !st.wifi_ap) return SCREEN.OFFLINE;
    switch (st.voice) {
      case VOICE.CONNECTING: return SCREEN.RECONNECTING;
      case VOICE.LISTENING: return SCREEN.LISTENING;
      case VOICE.THINKING: return SCREEN.THINKING;
      case VOICE.SPEAKING: return SCREEN.SPEAKING;
      case VOICE.CANCELLING: return SCREEN.LISTENING;
      default: break;
    }
    if (st.emotion !== EMOTION.NEUTRAL &&
        st.emotion_until !== 0 && st.now_ms < st.emotion_until) {
      switch (st.emotion) {
        case EMOTION.HAPPY: return SCREEN.FACE_HAPPY;
        case EMOTION.CURIOUS: return SCREEN.FACE_CURIOUS;
        case EMOTION.SURPRISED: return SCREEN.FACE_SURPRISED;
        case EMOTION.SAD: return SCREEN.FACE_SAD;
        case EMOTION.SHY: return SCREEN.FACE_SHY;
        case EMOTION.SLEEPY: return SCREEN.FACE_SLEEPY;
        case EMOTION.ANGRY: return SCREEN.FACE_ANGRY;
        default: break;
      }
    }
    return SCREEN.IDLE;
  }

  function statusText(st) {
    if (!st.booted) return "启动中...";
    if (st.device === DEVICE.SLEEP) return "";
    if (st.device === DEVICE.LOW_BATTERY) return "电量低";
    if (st.error_active) {
      switch (st.error) {
        case ERROR.ASR: return "没听清，再说一次";
        case ERROR.LLM: return "服务开小差了";
        case ERROR.TTS: return "播放出错了";
        case ERROR.AUDIO: return "音频出错了";
        case ERROR.NETWORK: return "网络异常";
        case ERROR.AUTH: return "服务暂不可用";
        default: return "出错了";
      }
    }
    if (st.offline && !st.wifi_ap) return "网络未连接";
    switch (st.voice) {
      case VOICE.CONNECTING: return "连接中...";
      case VOICE.LISTENING: return "在听...";
      case VOICE.THINKING: return "思考中...";
      case VOICE.SPEAKING: return "";
      default: break;
    }
    if (st.wifi_ap && !st.wifi_connected) return "配网模式";
    return "";
  }

  function init(st, now) {
    st.device = DEVICE.BOOT;
    st.voice = VOICE.DISCONNECTED;
    st.emotion = EMOTION.NEUTRAL;
    st.error = ERROR.NONE;
    st.booted = false;
    st.offline = false;
    st.ptt_active = false;
    st.wifi_connected = false;
    st.wifi_ap = false;
    st.wifi_connecting = false;
    st.battery_present = false;
    st.charging = false;
    st.battery_percent = -1;
    st.error_active = false;
    st.wake_until = 0;
    st.emotion_until = 0;
    st.error_until = 0;
    st.connecting_until = 0;
    st.listening_until = 0;
    st.thinking_until = 0;
    st.cancelling_until = 0;
    st.now_ms = Math.floor(now);
    st.revision = 1;
    st.subtitle = "";
  }

  function dispatch(st, ev, now) {
    st.now_ms = Math.floor(now);
    switch (ev.type) {
      case EVT.BOOT:
        init(st, now);
        return;
      case EVT.READY:
        st.booted = true;
        st.device = DEVICE.READY;
        st.wake_until = 0;
        break;
      case EVT.WAKE:
        st.booted = true;
        readyOrLow(st);
        st.ptt_active = false;
        st.error_active = false;
        st.error = ERROR.NONE;
        st.wake_until = st.now_ms + WAKE_MS;
        st.subtitle = "";
        break;
      case EVT.SLEEP:
        st.device = DEVICE.SLEEP;
        /* Presentation only: the socket/Wi-Fi are untouched. */
        st.voice = VOICE.READY;
        st.ptt_active = false;
        st.error_active = false;
        st.error = ERROR.NONE;
        st.wake_until = 0;
        st.connecting_until = 0;
        st.listening_until = 0;
        st.thinking_until = 0;
        st.cancelling_until = 0;
        st.subtitle = "";
        break;
      case EVT.PTT_DOWN:
        st.booted = true;
        if (st.device === DEVICE.SLEEP) readyOrLow(st);
        st.error_active = false;
        st.error = ERROR.NONE;
        st.ptt_active = true;
        st.subtitle = "";
        if (st.voice === VOICE.DISCONNECTED || st.voice === VOICE.CONNECTING) {
          st.voice = VOICE.CONNECTING;
          st.connecting_until = st.now_ms + CONNECTING_MS;
        } else {
          st.voice = VOICE.LISTENING;
          st.listening_until = st.now_ms + LISTENING_MS;
        }
        break;
      case EVT.PTT_UP:
        st.ptt_active = false;
        st.listening_until = 0;
        if (st.voice === VOICE.LISTENING || st.voice === VOICE.CANCELLING) {
          st.voice = VOICE.THINKING;
          st.thinking_until = st.now_ms + THINKING_MS;
        }
        break;
      case EVT.INTERRUPT:
        if (st.voice === VOICE.SPEAKING || st.voice === VOICE.THINKING ||
            st.voice === VOICE.CONNECTING) {
          st.voice = VOICE.CANCELLING;
          st.cancelling_until = st.now_ms + CANCELLING_MS;
          st.ptt_active = true;
          st.error_active = false;
          st.subtitle = "";
        }
        break;
      case EVT.VOICE_DISCONNECTED:
        st.voice = VOICE.DISCONNECTED;
        st.offline = true;
        st.ptt_active = false;
        st.connecting_until = 0;
        st.listening_until = 0;
        st.thinking_until = 0;
        st.subtitle = "";
        break;
      case EVT.VOICE_CONNECTING:
        st.voice = VOICE.CONNECTING;
        st.offline = false;
        st.connecting_until = st.now_ms + CONNECTING_MS;
        st.subtitle = "";
        break;
      case EVT.VOICE_READY:
        st.voice = VOICE.READY;
        st.offline = false;
        st.connecting_until = 0;
        break;
      case EVT.VOICE_LISTENING:
        st.voice = VOICE.LISTENING;
        st.offline = false;
        st.ptt_active = true;
        st.listening_until = st.now_ms + LISTENING_MS;
        st.connecting_until = 0;
        st.subtitle = "";
        break;
      case EVT.VOICE_THINKING:
        st.voice = VOICE.THINKING;
        st.ptt_active = false;
        st.listening_until = 0;
        st.thinking_until = st.now_ms + THINKING_MS;
        break;
      case EVT.VOICE_SPEAKING:
        st.voice = VOICE.SPEAKING;
        st.ptt_active = false;
        st.listening_until = 0;
        st.thinking_until = 0;
        break;
      case EVT.VOICE_CANCELLING:
        st.voice = VOICE.CANCELLING;
        st.cancelling_until = st.now_ms + CANCELLING_MS;
        break;
      case EVT.VOICE_IDLE:
        st.voice = VOICE.READY;
        st.offline = false;
        st.ptt_active = false;
        st.error_active = false;
        st.error = ERROR.NONE;
        st.connecting_until = 0;
        st.listening_until = 0;
        st.thinking_until = 0;
        st.cancelling_until = 0;
        st.subtitle = "";
        break;
      case EVT.SUBTITLE:
        st.subtitle = ev.text ? String(ev.text) : "";
        break;
      case EVT.EMOTION:
        st.emotion = ev.value;
        st.emotion_until = st.now_ms +
          (ev.value === EMOTION.NEUTRAL ? 0 : EMOTION_MS);
        break;
      case EVT.WIFI:
        st.wifi_connected = !!ev.flag;
        st.wifi_ap = !!ev.flag2;
        st.wifi_connecting = !!ev.flag3;
        if (st.wifi_connected) {
          st.offline = false;
          if (st.voice === VOICE.DISCONNECTED) {
            st.voice = VOICE.CONNECTING;
            st.connecting_until = st.now_ms + CONNECTING_MS;
          }
        } else if (st.wifi_connecting) {
          st.offline = false;
          if (st.voice === VOICE.DISCONNECTED) {
            st.voice = VOICE.CONNECTING;
            st.connecting_until = st.now_ms + CONNECTING_MS;
          }
        } else if (st.wifi_ap) {
          st.offline = false;
          st.voice = VOICE.DISCONNECTED;
          st.ptt_active = false;
        } else {
          st.offline = true;
          st.voice = VOICE.DISCONNECTED;
          st.ptt_active = false;
          st.connecting_until = 0;
          st.subtitle = "";
        }
        break;
      case EVT.BATTERY:
        st.battery_present = !!ev.flag;
        st.charging = !!ev.flag2;
        if (ev.flag && ev.value >= 0) {
          st.battery_percent = ev.value;
          if (st.charging) {
            if (st.device === DEVICE.LOW_BATTERY) st.device = DEVICE.READY;
          } else if (ev.value <= 10) {
            st.device = DEVICE.LOW_BATTERY;
          } else if (ev.value > 15 && st.device === DEVICE.LOW_BATTERY) {
            st.device = DEVICE.READY;
          }
        } else if (!ev.flag && st.device === DEVICE.LOW_BATTERY) {
          /* No ADC/pack information: keep the explicit low-battery state. */
        }
        break;
      case EVT.ERROR:
        st.error = ev.value;
        st.error_active = true;
        st.error_until = st.now_ms + ERROR_MS;
        st.ptt_active = false;
        st.listening_until = 0;
        st.thinking_until = 0;
        st.voice = VOICE.READY;
        st.subtitle = "";
        break;
      case EVT.CLEAR_ERROR:
        st.error_active = false;
        st.error = ERROR.NONE;
        st.error_until = 0;
        break;
      default:
        break;
    }
    st.revision++;
  }

  function tick(st, now) {
    st.now_ms = Math.floor(now);

    if (st.connecting_until !== 0 && st.now_ms >= st.connecting_until) {
      st.connecting_until = 0;
      if (st.voice === VOICE.CONNECTING) {
        st.voice = VOICE.DISCONNECTED;
        st.offline = true;
      }
    }
    if (st.cancelling_until !== 0 && st.now_ms >= st.cancelling_until) {
      st.cancelling_until = 0;
      if (st.voice === VOICE.CANCELLING) {
        st.voice = VOICE.LISTENING;
        st.ptt_active = true;
        st.listening_until = st.now_ms + LISTENING_MS;
      }
    }
    if (st.listening_until !== 0 && st.now_ms >= st.listening_until) {
      st.listening_until = 0;
      if (st.voice === VOICE.LISTENING) {
        st.voice = VOICE.THINKING;
        st.ptt_active = false;
        st.thinking_until = st.now_ms + THINKING_MS;
      }
    }
    if (st.thinking_until !== 0 && st.now_ms >= st.thinking_until) {
      st.thinking_until = 0;
      if (st.voice === VOICE.THINKING) {
        st.voice = VOICE.READY;
        st.error = ERROR.LLM;
        st.error_active = true;
        st.error_until = st.now_ms + ERROR_MS;
      }
    }
    if (st.error_active && st.error_until !== 0 && st.now_ms >= st.error_until) {
      st.error_active = false;
      st.error = ERROR.NONE;
      st.error_until = 0;
    }
    if (st.emotion_until !== 0 && st.now_ms >= st.emotion_until) {
      st.emotion_until = 0;
      st.emotion = EMOTION.NEUTRAL;
    }
    if (st.wake_until !== 0 && st.now_ms >= st.wake_until) {
      st.wake_until = 0;
    }
    st.revision++;
  }

  function view(st) {
    return {
      device: st.device,
      voice: st.voice,
      emotion: st.emotion,
      error: st.error,
      screen: resolveScreen(st),
      status: statusText(st),
      subtitle: st.subtitle,
      battery_percent: st.battery_percent,
      battery_present: st.battery_present,
      charging: st.charging,
      wifi_connected: st.wifi_connected,
      wifi_ap: st.wifi_ap,
      wifi_connecting: st.wifi_connecting,
      offline: st.offline,
      ptt_active: st.ptt_active,
      revision: st.revision,
      booted: st.booted,
      wake_until: st.wake_until,
      emotion_until: st.emotion_until,
      error_until: st.error_until,
      connecting_until: st.connecting_until,
      listening_until: st.listening_until,
      thinking_until: st.thinking_until,
      cancelling_until: st.cancelling_until,
      now_ms: st.now_ms,
    };
  }

  function createState(now) {
    const st = {};
    init(st, now || 0);
    return st;
  }

  const api = {
    WAKE_MS, EMOTION_MS, ERROR_MS, CONNECTING_MS, LISTENING_MS,
    THINKING_MS, CANCELLING_MS,
    DEVICE, VOICE, EMOTION, ERROR, SCREEN, EVT,
    SCREEN_NAMES, DEVICE_NAMES, VOICE_NAMES, EMOTION_NAMES, ERROR_NAMES,
    EVENT_NAMES,
    init, dispatch, tick, view, createState, resolveScreen, statusText,
  };

  if (typeof module !== "undefined" && module.exports) module.exports = api;
  global.PandaState = api;
})(typeof window !== "undefined" ? window : globalThis);
