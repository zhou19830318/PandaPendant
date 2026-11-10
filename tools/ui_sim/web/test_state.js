#!/usr/bin/env node
/* Replays tools/ui_sim/sim_main.c against the JS port used by the HTML demo. */
"use strict";

const P = require("./state_machine.js");
const {
  DEVICE, VOICE, EMOTION, ERROR, SCREEN, EVT, SCREEN_NAMES,
} = P;

let state = P.createState(0);
let now = 0;
let checks = 0;
let failures = 0;

function check(ok, what) {
  checks++;
  if (!ok) {
    failures++;
    console.log("  FAIL: " + what);
  }
}

function screen() {
  return P.view(state).screen;
}

function expectScreen(want, what) {
  check(screen() === want, what + " (got " + SCREEN_NAMES[screen()] + ")");
}

function dispatch(type, value, text) {
  P.dispatch(state, { type, value, text }, now);
}

function dispatchWifi(connected, ap, connecting) {
  P.dispatch(state, {
    type: EVT.WIFI, flag: connected, flag2: ap, flag3: connecting,
  }, now);
}

function dispatchBattery(percent, present, charging) {
  P.dispatch(state, {
    type: EVT.BATTERY, value: percent, flag: present, flag2: charging,
  }, now);
}

function advance(ms) {
  now += ms;
  P.tick(state, now);
}

function main() {
  console.log("Panda UI state-machine simulation (JS port)");
  P.init(state, 0);
  expectScreen(SCREEN.BOOT, "init shows boot");
  check(P.view(state).device === DEVICE.BOOT, "init device boot");

  now = 100;
  dispatch(EVT.READY, 0, null);
  expectScreen(SCREEN.IDLE, "boot done -> idle");

  dispatchWifi(true, false, false);
  expectScreen(SCREEN.RECONNECTING, "wifi up -> reconnecting");
  dispatch(EVT.VOICE_READY, 0, null);
  expectScreen(SCREEN.IDLE, "voice ready -> idle");

  dispatch(EVT.EMOTION, EMOTION.HAPPY, null);
  expectScreen(SCREEN.FACE_HAPPY, "emotion happy -> face screen");
  now += 3000;
  advance(0);
  expectScreen(SCREEN.IDLE, "emotion expired -> idle");

  dispatch(EVT.PTT_DOWN, 0, null);
  expectScreen(SCREEN.LISTENING, "ptt down -> listening");
  dispatch(EVT.SUBTITLE, 0, "你好，熊猫");
  dispatch(EVT.PTT_UP, 0, null);
  expectScreen(SCREEN.THINKING, "ptt up -> thinking");
  dispatch(EVT.SUBTITLE, 0, "你好呀");
  dispatch(EVT.VOICE_SPEAKING, 0, null);
  expectScreen(SCREEN.SPEAKING, "tts -> speaking");
  dispatch(EVT.VOICE_IDLE, 0, null);
  expectScreen(SCREEN.IDLE, "response done -> idle");
  check(P.view(state).subtitle.length === 0, "idle clears subtitle");

  dispatch(EVT.PTT_DOWN, 0, null);
  dispatch(EVT.PTT_UP, 0, null);
  dispatch(EVT.VOICE_SPEAKING, 0, null);
  dispatch(EVT.INTERRUPT, 0, null);
  expectScreen(SCREEN.LISTENING, "interrupt -> cancelling renders listening");
  advance(500);
  expectScreen(SCREEN.LISTENING, "cancelled -> listening");
  dispatch(EVT.VOICE_IDLE, 0, null);
  expectScreen(SCREEN.IDLE, "cancel turn -> idle");

  dispatchWifi(false, false, false);
  expectScreen(SCREEN.OFFLINE, "wifi down -> offline");
  check(P.view(state).voice === VOICE.DISCONNECTED, "wifi down disconnects voice");
  dispatchWifi(true, false, false);
  expectScreen(SCREEN.RECONNECTING, "wifi recovered -> reconnecting");
  dispatch(EVT.VOICE_READY, 0, null);

  dispatch(EVT.ERROR, ERROR.LLM, null);
  expectScreen(SCREEN.ERROR_LLM, "llm error -> error screen");
  check(P.view(state).status === "服务开小差了", "llm error status");
  advance(4100);
  expectScreen(SCREEN.IDLE, "error expired -> idle");

  dispatch(EVT.ERROR, ERROR.ASR, null);
  expectScreen(SCREEN.ERROR_ASR, "asr error -> asr screen");
  check(P.view(state).status === "没听清，再说一次", "asr error status");
  advance(4100);
  expectScreen(SCREEN.IDLE, "asr error expired -> idle");

  dispatch(EVT.ERROR, ERROR.TTS, null);
  expectScreen(SCREEN.ERROR_TTS, "tts error -> tts screen");
  advance(4100);
  dispatch(EVT.ERROR, ERROR.AUTH, null);
  expectScreen(SCREEN.ERROR_LLM, "auth error -> service screen");
  check(P.view(state).status === "服务暂不可用", "auth error status");
  advance(4100);
  expectScreen(SCREEN.IDLE, "auth error expired -> idle");

  dispatch(EVT.ERROR, ERROR.NETWORK, null);
  expectScreen(SCREEN.OFFLINE, "network error -> offline screen");
  advance(4100);
  expectScreen(SCREEN.IDLE, "network error expired -> idle");

  dispatchBattery(8, true, false);
  expectScreen(SCREEN.LOW_BATTERY, "battery 8% -> low battery");
  check(P.view(state).status === "电量低", "low battery status");
  dispatchBattery(80, true, false);
  expectScreen(SCREEN.IDLE, "battery recovered -> idle");
  dispatchBattery(10, true, false);
  expectScreen(SCREEN.LOW_BATTERY, "explicit low -> low battery");
  dispatch(EVT.READY, 0, null);
  expectScreen(SCREEN.IDLE, "ready clears explicit low battery");
  dispatchBattery(80, true, false);

  dispatch(EVT.SLEEP, 0, null);
  expectScreen(SCREEN.SLEEP, "sleep screen");
  dispatch(EVT.WAKE, 0, null);
  expectScreen(SCREEN.WAKE, "wake screen");
  advance(1300);
  expectScreen(SCREEN.IDLE, "wake -> idle");

  dispatch(EVT.PTT_DOWN, 0, null);
  expectScreen(SCREEN.LISTENING, "ptt after wake -> listening");
  advance(16000);
  expectScreen(SCREEN.THINKING, "listen timeout -> thinking");
  dispatch(EVT.VOICE_IDLE, 0, null);

  dispatch(EVT.VOICE_CONNECTING, 0, null);
  expectScreen(SCREEN.RECONNECTING, "voice connecting -> reconnecting");
  advance(21000);
  expectScreen(SCREEN.OFFLINE, "connect timeout -> offline");

  dispatchWifi(true, false, false);
  dispatch(EVT.VOICE_READY, 0, null);
  dispatchBattery(80, true, false);
  dispatch(EVT.VOICE_SPEAKING, 0, null);
  dispatchBattery(5, true, false);
  expectScreen(SCREEN.LOW_BATTERY, "low battery overrides speaking");
  dispatchBattery(80, true, false);
  expectScreen(SCREEN.SPEAKING, "battery recovered restores speaking");

  P.init(state, now);
  dispatch(EVT.READY, 0, null);
  dispatchWifi(false, true, false);
  expectScreen(SCREEN.IDLE, "provisioning AP -> idle");
  check(P.view(state).status === "配网模式", "provisioning status");

  console.log("\n" + checks + " checks, " + failures + " failure(s)");
  process.exit(failures === 0 ? 0 : 1);
}

main();
