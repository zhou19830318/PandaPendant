/*
 * Browser driver for the Panda AI state machine demo.
 *
 * The state rules live in state_machine.js, a literal port of panda_state.c.
 * This file only schedules events, renders the 360x360 screen, and builds the
 * inspector/log. It deliberately does not add any state rule of its own.
 */
(function () {
  "use strict";

  const P = window.PandaState;
  if (!P) {
    document.body.innerHTML = "<p style='padding:24px;color:#fff;font:14px sans-serif'>state_machine.js 未加载</p>";
    return;
  }

  const PAGE_DB = window.PANDA_PAGES || { groups: [], pages: {} };
  const EVT = P.EVT;
  const A = (name) => "assets/" + name + ".png";
  const $ = (id) => document.getElementById(id);
  const BEAT = 220;
  const STEP_GAP = 400;

  /* Every screen resolved by panda_state.c, in enum order. The generated
   * file and the image name are taken from the SquareLine export. */
  const SCREEN_INFO = [
    {
      title: "Boot 启动",
      file: "ui_wakeup.c",
      face: "face_01_idle",
      tip: null,
      note: "screen_for(BOOT) 复用 ui_wakeup；状态文字由 panda_ui 显示为“启动中...”。",
    },
    {
      title: "Wake 唤醒",
      file: "ui_wakeup.c",
      face: "face_01_idle",
      tip: null,
      note: "WAKE 后停留 1.2s；期间 voice/emotion 被 wake 优先级覆盖。",
    },
    {
      title: "Idle 待机",
      file: "ui_home.c",
      face: "face_01_idle",
      tip: null,
      note: "无状态文字、无情绪、无错误时的默认屏。",
    },
    {
      title: "Listening 聆听",
      file: "ui_listening.c",
      face: "face_07_listen",
      tip: null,
      waves: true,
      note: "9 根波纹条；初始高度来自生成代码，之后由 panda_ui_set_audio_level() 更新。",
    },
    {
      title: "Thinking 思考",
      file: "ui_thinking.c",
      face: "face_03_curious",
      tip: null,
      dots: true,
      note: "3 个点；第二个点 opacity=90/255 ≒ 35.3%，位置与生成代码一致。",
    },
    {
      title: "Speaking 说话",
      file: "ui_speaking.c",
      face: "face_08_speak",
      tip: null,
      note: "SPEAKING 屏本身没有额外装饰；状态文字为空。",
    },
    {
      title: "Offline 离线",
      file: "ui_network_lost.c",
      face: "face_19_abnormal",
      tip: { icon: "icon_wifi_off", text: "网络已断开" },
      note: "offline && !wifi_ap 时显示；status 为“网络未连接”。",
    },
    {
      title: "Reconnecting 重连",
      file: "ui_reconnecting.c",
      face: "face_03_curious",
      tip: { icon: "icon_refresh_cw", text: "重新连接中" },
      note: "VOICE_CONNECTING 时显示；连接超时 20s 后转为 DISCONNECTED。",
    },
    {
      title: "ASR 错误",
      file: "ui_asr_failed.c",
      face: "face_12_confused",
      tip: { icon: "icon_mic_off", text: "没听清" },
      note: "error=ASR；错误屏持续 4s，status 为“没听清，再说一次”。",
    },
    {
      title: "LLM 错误",
      file: "ui_llm_error.c",
      face: "face_12_confused",
      tip: { icon: "icon_cloud_off", text: "暂时无法回答" },
      note: "error=LLM；AUTH 与 GENERIC 也复用 ui_llm_error。",
    },
    {
      title: "TTS 错误",
      file: "ui_tts_error.c",
      face: "face_11_sad",
      tip: { icon: "icon_volume_x", text: "播放失败" },
      note: "error=TTS；AUDIO 也复用 ui_tts_error。",
    },
    {
      title: "低电",
      file: "ui_battery_low.c",
      face: "face_18_tired",
      tip: { icon: "icon_battery_low", text: "电量低 15%" },
      note: "电量 <=10% 且未充电时显示；优先级高于 error/wake/voice。",
    },
    {
      title: "Sleep 睡眠",
      file: "ui_face_sleep.c",
      face: "face_10_sleep",
      tip: null,
      note: "DEVICE_SLEEP 时显示，优先级最高；状态文字为空。",
    },
    {
      title: "Happy",
      file: "ui_face_happy.c",
      face: "face_02_happy_wink",
      tip: null,
      note: "EMOTION_HAPPY；非 NEUTRAL 情绪持续 2.5s。",
    },
    {
      title: "Curious",
      file: "ui_face_curious.c",
      face: "face_03_curious",
      tip: null,
      note: "EMOTION_CURIOUS；与 reconnecting/thinking 共用 face_03_curious。",
    },
    {
      title: "Surprised",
      file: "ui_face_surprised.c",
      face: "face_04_surprised",
      tip: null,
      note: "EMOTION_SURPRISED；持续 2.5s。",
    },
    {
      title: "Sad",
      file: "ui_face_sad.c",
      face: "face_11_sad",
      tip: null,
      note: "EMOTION_SAD；与 TTS 错误屏共用 face_11_sad。",
    },
    {
      title: "Shy",
      file: "ui_face_shy.c",
      face: "face_14_shy",
      tip: null,
      note: "EMOTION_SHY；持续 2.5s。",
    },
    {
      title: "Sleepy",
      file: "ui_face_sleepy.c",
      face: "face_09_sleepy",
      tip: null,
      note: "EMOTION_SLEEPY；持续 2.5s。",
    },
    {
      title: "Angry",
      file: "ui_face_angry.c",
      face: "face_13_angry",
      tip: null,
      note: "EMOTION_ANGRY；持续 2.5s。",
    },
  ];

  const faceEl = $("face");
  const sceneEl = $("scene");
  const screenEl = $("screen");
  const wavesEl = $("waves");
  const dotsEl = $("dots");
  const tipEl = $("tip");
  const tipIconEl = $("tipIcon");
  const tipTextEl = $("tipText");
  const waveEls = Array.prototype.slice.call(document.querySelectorAll("#waves .wave"));
  const statusPill = $("statusPill");
  const subtitlePill = $("subtitlePill");
  const toastPill = $("toastPill");
  const flowStatusEl = $("flowStatus");
  const stepLabelEl = $("stepLabel");
  const progressMetaEl = $("progressMeta");
  const progressBarEl = $("progressBar");
  const playBtn = $("playBtn");
  const freezeBtn = $("freezeBtn");
  const resetBtn = $("resetBtn");
  const speedSelect = $("speed");
  const roundToggle = $("roundToggle");
  const audioRange = $("audioRange");
  const audioVal = $("audioVal");
  const onlyScreens = $("onlyScreens");
  const logList = $("logList");
  const mapList = $("mapList");
  const pageScene = $("pageScene");
  const pageBadge = $("pageBadge");
  const backLive = $("backLive");
  const pageGroups = $("pageGroups");

  const WAVE_X = [-44, -33, -22, -11, 0, 11, 22, 33, 44];
  const BASE_WAVES = [10, 18, 28, 16, 34, 20, 30, 14, 8];

  let st = P.createState(0);
  let now = 0;
  let lastTickAt = 0;
  let lastFrame = 0;
  let flowIndex = 0;
  let flowRunning = false;
  let flowComplete = false;
  let flowStarted = false;
  let flowAborted = false;
  let paused = false;
  let lastStep = -1;
  let speed = 4;
  let logs = [];
  let override = null;
  let toast = null;
  let currentScreen = null;
  let waveHeights = BASE_WAVES.slice();
  let preview = null;
  let renderedPreviewKey = null;

  const timeline = buildTimeline();

  /* ------------------------------------------------------------------ */
  /* Timeline: the same event sequence as tools/ui_sim/sim_main.c, with  */
  /* a 220 ms beat between events so each screen is visible.             */
  /* ------------------------------------------------------------------ */

  function buildTimeline() {
    const steps = [
      "启动 / 设备就绪",
      "WiFi 连接 / 语音就绪",
      "情绪表情与 2.5s 超时",
      "一轮完整对话",
      "用户打断（迟到的 listening 竞态）",
      "断网 / 重连",
      "五类错误屏（各 4s）",
      "低电优先级与 READY 清除",
      "睡眠与唤醒",
      "聆听 / 思考 / 连接超时链",
      "配网 SoftAP",
    ];
    const items = [];
    let t = 0;
    let step = 0;

    const ev = (type, payload, delay) => {
      t += delay === undefined ? BEAT : delay;
      items.push({ at: Math.round(t), type: type, payload: payload || {}, step: step });
    };
    const gap = (ms) => { t += ms; };
    const next = (idx) => { step = idx; t += STEP_GAP; };

    items.push({ at: 0, type: EVT.BOOT, payload: {}, step: 0 });
    ev(EVT.READY, {}, 100);

    next(1);
    ev(EVT.WIFI, { flag: true, flag2: false, flag3: false });
    ev(EVT.VOICE_READY, {});

    next(2);
    ev(EVT.EMOTION, { value: P.EMOTION.HAPPY }, 300);
    gap(3000);

    next(3);
    ev(EVT.PTT_DOWN, {}, 300);
    ev(EVT.SUBTITLE, { text: "今天天气怎么样？" });
    ev(EVT.PTT_UP, {});
    ev(EVT.SUBTITLE, { text: "今天多云，24°C" });
    ev(EVT.VOICE_THINKING, {});
    ev(EVT.VOICE_SPEAKING, {}, 300);
    ev(EVT.VOICE_IDLE, {}, 700);

    next(4);
    ev(EVT.PTT_DOWN, {}, 300);
    ev(EVT.PTT_UP, {});
    ev(EVT.VOICE_THINKING, {}, 300);
    ev(EVT.VOICE_SPEAKING, {});
    ev(EVT.INTERRUPT, {}, 400);
    gap(500);
    ev(EVT.VOICE_IDLE, {}, 200);

    next(5);
    ev(EVT.WIFI, { flag: false, flag2: false, flag3: false }, 300);
    ev(EVT.WIFI, { flag: true, flag2: false, flag3: false }, 400);
    ev(EVT.VOICE_READY, {}, 300);

    next(6);
    ev(EVT.ERROR, { value: P.ERROR.LLM }, 300);
    gap(4100);
    ev(EVT.ERROR, { value: P.ERROR.ASR });
    gap(4100);
    ev(EVT.ERROR, { value: P.ERROR.TTS });
    gap(4100);
    ev(EVT.ERROR, { value: P.ERROR.AUTH });
    gap(4100);
    ev(EVT.ERROR, { value: P.ERROR.NETWORK });
    gap(4100);

    next(7);
    ev(EVT.BATTERY, { value: 8, flag: true, flag2: false }, 300);
    ev(EVT.BATTERY, { value: 80, flag: true, flag2: false }, 500);
    ev(EVT.BATTERY, { value: 10, flag: true, flag2: false }, 500);
    ev(EVT.READY, {}, 400);
    ev(EVT.BATTERY, { value: 80, flag: true, flag2: false }, 400);

    next(8);
    ev(EVT.SLEEP, {}, 400);
    ev(EVT.WAKE, {}, 500);
    gap(1300);

    next(9);
    ev(EVT.PTT_DOWN, {}, 300);
    gap(16000);
    ev(EVT.VOICE_IDLE, {}, 250);
    ev(EVT.VOICE_CONNECTING, {}, 300);
    gap(21000);
    ev(EVT.WIFI, { flag: true, flag2: false, flag3: false }, 250);
    ev(EVT.VOICE_READY, {}, 250);
    ev(EVT.BATTERY, { value: 80, flag: true, flag2: false }, 250);
    ev(EVT.VOICE_SPEAKING, {}, 300);
    ev(EVT.BATTERY, { value: 5, flag: true, flag2: false }, 500);
    ev(EVT.BATTERY, { value: 80, flag: true, flag2: false }, 500);
    ev(EVT.VOICE_IDLE, {}, 500);

    next(10);
    ev(EVT.BOOT, {}, 300);
    ev(EVT.READY, {}, 250);
    ev(EVT.WIFI, { flag: false, flag2: true, flag3: false }, 400);

    return { steps: steps, items: items, total: items[items.length - 1].at };
  }

  /* ------------------------------------------------------------------ */
  /* Clock and event loop                                                */
  /* ------------------------------------------------------------------ */

  function advanceWorld(target) {
    target = Math.floor(target);
    let guard = 0;
    while (guard++ < 50000) {
      const nextEvent = flowRunning && flowIndex < timeline.items.length
        ? timeline.items[flowIndex].at
        : Infinity;
      const nextTick = lastTickAt + 50;

      if (nextTick <= target && nextTick <= nextEvent) {
        lastTickAt = nextTick;
        P.tick(st, nextTick);
        continue;
      }
      if (nextEvent <= target) {
        const item = timeline.items[flowIndex++];
        lastStep = item.step;
        P.dispatch(st, {
          type: item.type,
          value: item.payload.value,
          flag: item.payload.flag,
          flag2: item.payload.flag2,
          flag3: item.payload.flag3,
          text: item.payload.text,
        }, item.at);
        logEvent(item.at, item.type, item.payload);
        continue;
      }
      break;
    }
  }

  function frame(ts) {
    if (!lastFrame) lastFrame = ts;
    const dt = Math.min(80, Math.max(0, ts - lastFrame));
    lastFrame = ts;

    if (!paused) {
      now += dt * speed;
      advanceWorld(now);
      if (flowRunning && flowIndex >= timeline.items.length && now >= timeline.total + 800) {
        flowRunning = false;
        flowComplete = true;
        addLog(st.now_ms, "system", "FLOW", "自动演示完成");
      }
    }

    render();
    requestAnimationFrame(frame);
  }

  /* ------------------------------------------------------------------ */
  /* Rendering                                                           */
  /* ------------------------------------------------------------------ */

  function render() {
    const v = P.view(st);

    if (override && now >= override.until) override = null;
    if (toast && now >= toast.until) toast = null;

    if (preview) {
      sceneEl.classList.add("hidden");
      pageScene.classList.remove("hidden");
      pageBadge.classList.remove("hidden");
      backLive.classList.remove("hidden");
      statusPill.classList.add("hidden");
      subtitlePill.classList.add("hidden");
      tipEl.classList.add("hidden");
      renderPreview();
    } else {
      sceneEl.classList.remove("hidden");
      pageScene.classList.add("hidden");
      pageBadge.classList.add("hidden");
      backLive.classList.add("hidden");
      updateScreen(v);
      updateOverlays(v);
    }
    updatePageButtons();
    updateInspector(v);
    updateProgress();
    updatePlayButton();
    updateFreezeButton();
  }

  function previewKey(p) {
    if (!p) return "";
    return p.kind === "screen" ? "screen:" + p.screen : "page:" + p.id;
  }

  function previewPageInfo() {
    if (preview.kind === "screen") {
      const info = SCREEN_INFO[preview.screen];
      return { kind: "screen", title: info.title, file: info.file,
               face: info.face, tip: info.tip, note: info.note };
    }
    const page = PAGE_DB.pages[preview.id] || {};
    return { kind: page.kind || "layout", title: page.title || preview.id,
             file: page.file || "", face: page.face || "",
             objects: page.objects || [], note: page.note || "" };
  }

  function setPreview(next) {
    if (flowRunning) {
      flowRunning = false;
      addLog(now, "system", "FLOW", "打开页面预览，自动演示停止");
    }
    preview = next;
    renderedPreviewKey = null;
    render();
    if (next) {
      const p = previewPageInfo();
      addLog(now, "system", "PREVIEW", p.title + " (" + p.file + ")");
    } else {
      addLog(now, "system", "PREVIEW", "返回状态机实时画面");
    }
  }

  function renderPreview() {
    const key = previewKey(preview);
    if (key === renderedPreviewKey) return;
    renderedPreviewKey = key;

    pageScene.replaceChildren();
    pageScene.style.background = "#000";

    if (preview.kind === "screen") {
      const info = SCREEN_INFO[preview.screen];
      buildStaticScreen(info);
      pageBadge.textContent = "静态屏 · " + P.SCREEN_NAMES[preview.screen];
      return;
    }

    const page = PAGE_DB.pages[preview.id];
    if (!page) {
      const msg = document.createElement("div");
      msg.className = "page-label";
      msg.style.cssText = "left:20px;top:160px;width:320px;height:40px;color:#fff";
      msg.textContent = "缺少页面数据：" + preview.id;
      pageScene.appendChild(msg);
      pageBadge.textContent = "页面缺失";
      return;
    }

    if (page.kind === "face") {
      buildFacePage(page);
      pageBadge.textContent = "表情 · " + page.face;
    } else {
      buildLayoutPage(page);
      pageBadge.textContent = "页面 · " + page.objects.length + " 个对象";
    }
  }

  function buildStaticScreen(info) {
    const face = document.createElement("img");
    face.className = "page-img";
    face.src = A(info.face);
    face.alt = info.face;
    face.style.cssText = "position:absolute;left:0;top:0;width:360px;height:360px";
    pageScene.appendChild(face);

    if (info.waves) {
      const waves = document.createElement("div");
      waves.className = "waves";
      waveHeights.forEach(function (h, i) {
        const bar = document.createElement("i");
        bar.className = "wave";
        bar.style.height = h + "px";
        bar.style.left = (180 + WAVE_X[i] - 3) + "px";
        bar.style.top = (180 + 142 - h / 2) + "px";
        waves.appendChild(bar);
      });
      pageScene.appendChild(waves);
    }

    if (info.dots) {
      const dots = document.createElement("div");
      dots.className = "dots";
      for (let i = 0; i < 3; i++) {
        const dot = document.createElement("i");
        dot.className = "dot";
        dots.appendChild(dot);
      }
      pageScene.appendChild(dots);
    }

    if (info.tip) {
      const tip = document.createElement("div");
      tip.className = "tip";
      const icon = document.createElement("img");
      icon.src = A(info.tip.icon);
      icon.alt = info.tip.icon;
      const text = document.createElement("span");
      text.textContent = info.tip.text;
      tip.appendChild(icon);
      tip.appendChild(text);
      pageScene.appendChild(tip);
    }
  }

  function buildFacePage(page) {
    const face = document.createElement("img");
    face.className = "page-node page-img";
    face.src = A(page.face);
    face.alt = page.face;
    face.style.cssText = "position:absolute;left:0;top:0;width:360px;height:360px";
    pageScene.appendChild(face);
  }

  function rgba(hex, alpha) {
    if (!hex) return "transparent";
    const m = /^#?([0-9a-f]{6})$/i.exec(hex);
    if (!m) return hex;
    const n = parseInt(m[1], 16);
    return "rgba(" + ((n >> 16) & 255) + "," + ((n >> 8) & 255) + "," + (n & 255) +
      "," + (alpha == null ? 1 : alpha) + ")";
  }

  function buildLayoutPage(page) {
    pageScene.style.background = page.bg || "#000";
    pageScene.style.fontFamily = '"PingFang SC", "Microsoft YaHei", "Noto Sans SC", sans-serif';

    page.objects.forEach(function (obj, index) {
      if (obj.hidden) return;

      if (obj.type === "label") {
        const el = document.createElement("div");
        el.className = "page-node page-label";
        el.style.left = obj.x + "px";
        el.style.top = obj.y + "px";
        el.style.width = obj.w + "px";
        el.style.height = obj.h + "px";
        el.style.color = obj.color || "#fff";
        el.style.fontSize = (obj.font || 16) + "px";
        el.style.opacity = obj.opacity == null ? "1" : String(obj.opacity);
        el.style.zIndex = String(index + 1);
        const span = document.createElement("span");
        span.style.display = "block";
        span.style.width = "100%";
        span.style.textAlign = obj.align || "left";
        span.textContent = obj.text || "";
        el.appendChild(span);
        if (obj.link) addPageLink(el, obj.link, obj.name);
        pageScene.appendChild(el);
        return;
      }

      if (obj.type === "image") {
        const el = document.createElement("img");
        el.className = "page-node page-img";
        el.style.left = obj.x + "px";
        el.style.top = obj.y + "px";
        el.style.width = obj.w + "px";
        el.style.height = obj.h + "px";
        el.style.zIndex = String(index + 1);
        el.src = A(obj.img);
        el.alt = obj.name;
        if (obj.opacity != null) el.style.opacity = String(obj.opacity);
        if (obj.link) addPageLink(el, obj.link, obj.name);
        pageScene.appendChild(el);
        return;
      }

      const el = document.createElement("div");
      el.className = "page-node";
      el.style.left = obj.x + "px";
      el.style.top = obj.y + "px";
      el.style.width = obj.w + "px";
      el.style.height = obj.h + "px";
      el.style.background = rgba(obj.bg, obj.bgOpacity);
      el.style.borderRadius = (obj.radius || 0) + "px";
      el.style.opacity = obj.opacity == null ? "1" : String(obj.opacity);
      el.style.zIndex = String(index + 1);
      if (obj.link) addPageLink(el, obj.link, obj.name);
      pageScene.appendChild(el);
    });
  }

  function addPageLink(el, link, name) {
    el.classList.add("page-link");
    el.dataset.link = link;
    el.title = name + " → " + link;
  }

  function buildPageGallery() {
    pageGroups.replaceChildren();

    const liveGroup = document.createElement("div");
    liveGroup.className = "page-group";
    const liveTitle = document.createElement("h3");
    liveTitle.textContent = "实时画面";
    liveGroup.appendChild(liveTitle);
    const liveGrid = document.createElement("div");
    liveGrid.className = "page-grid";
    const liveBtn = document.createElement("button");
    liveBtn.dataset.kind = "live";
    liveBtn.textContent = "状态机实时画面";
    const liveSmall = document.createElement("small");
    liveSmall.textContent = "按当前三维状态解析";
    liveBtn.appendChild(liveSmall);
    liveGrid.appendChild(liveBtn);
    liveGroup.appendChild(liveGrid);
    pageGroups.appendChild(liveGroup);

    const screenGroup = document.createElement("div");
    screenGroup.className = "page-group";
    const screenTitle = document.createElement("h3");
    screenTitle.textContent = "状态屏 · 20 个生成页面";
    screenGroup.appendChild(screenTitle);
    const screenGrid = document.createElement("div");
    screenGrid.className = "page-grid";
    SCREEN_INFO.forEach(function (info, i) {
      const btn = document.createElement("button");
      btn.dataset.kind = "screen";
      btn.dataset.screen = String(i);
      btn.textContent = info.title;
      const small = document.createElement("small");
      small.textContent = P.SCREEN_NAMES[i] + " · " + info.file;
      btn.appendChild(small);
      screenGrid.appendChild(btn);
    });
    screenGroup.appendChild(screenGrid);

    PAGE_DB.groups.forEach(function (group) {
      const box = document.createElement("div");
      box.className = "page-group";
      const title = document.createElement("h3");
      title.textContent = group.title + " · " + group.pages.length + " 页";
      box.appendChild(title);
      const grid = document.createElement("div");
      grid.className = "page-grid";
      group.pages.forEach(function (id) {
        const page = PAGE_DB.pages[id];
        if (!page) return;
        const btn = document.createElement("button");
        btn.dataset.kind = "page";
        btn.dataset.page = id;
        btn.textContent = page.title;
        const small = document.createElement("small");
        small.textContent = page.file;
        btn.appendChild(small);
        grid.appendChild(btn);
      });
      box.appendChild(grid);
      pageGroups.appendChild(box);
    });

    pageGroups.appendChild(screenGroup);
  }

  function updatePageButtons() {
    const key = previewKey(preview);
    pageGroups.querySelectorAll("button[data-kind]").forEach(function (btn) {
      const kind = btn.dataset.kind;
      const btnKey = kind === "live" ? "" : (kind === "screen"
        ? "screen:" + btn.dataset.screen
        : "page:" + btn.dataset.page);
      btn.classList.toggle("active", btnKey === key);
    });
  }

  function followPageLink(link) {
    if (link === "live") {
      setPreview(null);
      return;
    }
    if (link.indexOf("screen:") === 0) {
      const name = link.slice(7);
      const idx = P.SCREEN_NAMES.indexOf(name);
      if (idx >= 0) setPreview({ kind: "screen", screen: idx });
      return;
    }
    if (PAGE_DB.pages[link]) setPreview({ kind: "page", id: link });
  }

  function updateScreen(v) {
    const info = SCREEN_INFO[v.screen];
    const prev = currentScreen;

    if (prev !== v.screen) {
      currentScreen = v.screen;
      faceEl.src = A(info.face);
      faceEl.alt = info.face;
      sceneEl.classList.remove("fade");
      void sceneEl.offsetWidth;
      sceneEl.classList.add("fade");
      if (prev !== null) {
        addLog(st.now_ms, "screen", "SCREEN",
          P.SCREEN_NAMES[prev] + " → " + P.SCREEN_NAMES[v.screen]);
      }
    }

    wavesEl.classList.toggle("hidden", !info.waves);
    dotsEl.classList.toggle("hidden", !info.dots);
    if (info.tip) {
      tipEl.classList.remove("hidden");
      tipIconEl.src = A(info.tip.icon);
      tipIconEl.alt = info.tip.icon;
      tipTextEl.textContent = info.tip.text;
    } else {
      tipEl.classList.add("hidden");
    }
    if (info.waves) layoutWaves();
  }

  function layoutWaves() {
    for (let i = 0; i < waveEls.length; i++) {
      const h = waveHeights[i];
      waveEls[i].style.height = h + "px";
      waveEls[i].style.left = (180 + WAVE_X[i] - 3) + "px";
      waveEls[i].style.top = (180 + 142 - h / 2) + "px";
    }
  }

  function updateOverlays(v) {
    const info = SCREEN_INFO[v.screen] || {};
    let statusText = v.status || "";
    /* Screens with their own bottom tip (errors/offline/low battery) hide
     * the status pill in firmware so the two do not overlap. */
    if (info.tip || v.subtitle) statusText = "";
    if (override) statusText = override.text;

    if (statusText) {
      statusPill.textContent = statusText;
      statusPill.classList.remove("hidden");
    } else {
      statusPill.classList.add("hidden");
    }

    if (v.subtitle) {
      subtitlePill.textContent = v.subtitle;
      subtitlePill.classList.remove("hidden");
    } else {
      subtitlePill.classList.add("hidden");
    }

    if (toast) {
      toastPill.textContent = toast.text;
      toastPill.classList.remove("hidden");
    } else {
      toastPill.classList.add("hidden");
    }
  }

  function updateInspector(v) {
    const info = SCREEN_INFO[v.screen];

    $("chipScreen").textContent = P.SCREEN_NAMES[v.screen];
    $("chipDevice").textContent = P.DEVICE_NAMES[v.device];
    $("chipVoice").textContent = P.VOICE_NAMES[v.voice];
    $("chipEmotion").textContent = P.EMOTION_NAMES[v.emotion];
    $("chipError").textContent = v.error !== P.ERROR.NONE ? P.ERROR_NAMES[v.error] : "none";
    $("chipErrorWrap").classList.toggle("on", v.error !== P.ERROR.NONE);

    setKV("kvBooted", v.booted ? "true" : "false", v.booted ? "ok" : "");
    setKV("kvPtt", v.ptt_active ? "true" : "false", v.ptt_active ? "warn" : "");
    setKV("kvWifi", wifiSummary(v), v.wifi_connected ? "ok" : (v.offline ? "bad" : ""));
    setKV("kvBattery", batterySummary(v), batteryClass(v));
    setKV("kvOffline", v.offline ? "true" : "false", v.offline ? "bad" : "");
    setKV("kvRevision", String(v.revision), "");

    if (preview) {
      const p = previewPageInfo();
      $("currentTitle").textContent = p.title;
      $("currentEnum").textContent = preview.kind === "screen"
        ? "PANDA_STATE_SCREEN_" + P.SCREEN_NAMES[preview.screen].toUpperCase()
        : (p.kind === "face" ? "PANDA_STATE_SCREEN_FACE_*" : "STATIC_PAGE");
      $("currentFile").textContent = p.file;
      $("currentFace").textContent = preview.kind === "screen" || p.kind === "face"
        ? p.face + ".png"
        : p.objects.length + " 个生成对象";
      $("currentTip").textContent = p.kind === "screen" && p.tip
        ? p.tip.text + " / " + p.tip.icon + ".png"
        : "不叠加";
      $("currentNow").textContent = String(st.now_ms) + " ms";
      $("currentNote").textContent = p.note;
    } else {
      $("currentTitle").textContent = info.title;
      $("currentEnum").textContent = "PANDA_STATE_SCREEN_" + P.SCREEN_NAMES[v.screen].toUpperCase();
      $("currentFile").textContent = info.file;
      $("currentFace").textContent = info.face + ".png";
      $("currentTip").textContent = info.tip
        ? info.tip.text + " / " + info.tip.icon + ".png"
        : "无";
      $("currentNow").textContent = String(st.now_ms) + " ms";
      $("currentNote").textContent = info.note;
    }

    renderTimers(v);
  }

  function setKV(id, text, cls) {
    const el = $(id);
    el.textContent = text;
    el.classList.remove("ok", "warn", "bad");
    if (cls) el.classList.add(cls);
  }

  function wifiSummary(v) {
    const bits = [];
    if (v.wifi_connected) bits.push("STA");
    if (v.wifi_ap) bits.push("AP");
    if (v.wifi_connecting) bits.push("connecting");
    if (v.offline) bits.push("offline");
    return bits.length ? bits.join(" + ") : "idle";
  }

  function batterySummary(v) {
    if (v.battery_percent < 0) return v.battery_present ? "present / --" : "unknown";
    return v.battery_percent + "% " + (v.charging ? "charging" : "discharging") +
      (v.battery_present ? "" : " / no pack");
  }

  function batteryClass(v) {
    if (!v.battery_present || v.battery_percent < 0) return "";
    if (!v.charging && v.battery_percent <= 10) return "bad";
    if (v.charging) return "warn";
    return "ok";
  }

  function renderTimers(v) {
    const rows = [
      ["wake", v.wake_until],
      ["emotion", v.emotion_until],
      ["error", v.error_until],
      ["connecting", v.connecting_until],
      ["listening", v.listening_until],
      ["thinking", v.thinking_until],
      ["cancelling", v.cancelling_until],
    ].filter((row) => row[1] > 0);

    const frag = document.createDocumentFragment();
    if (!rows.length) {
      const span = document.createElement("span");
      span.className = "muted";
      span.textContent = "无活动超时";
      frag.appendChild(span);
    } else {
      rows.forEach((row) => {
        const el = document.createElement("div");
        el.className = "timer";
        const label = document.createElement("span");
        label.textContent = row[0];
        const value = document.createElement("b");
        value.textContent = (Math.max(0, row[1] - now) / 1000).toFixed(1) + "s";
        el.appendChild(label);
        el.appendChild(value);
        frag.appendChild(el);
      });
    }
    $("timers").replaceChildren(frag);
  }

  function updateProgress() {
    const shownNow = Math.min(now, timeline.total);
    const pct = flowStarted ? Math.min(100, (shownNow / timeline.total) * 100) : 0;
    progressBarEl.style.width = pct + "%";
    progressMetaEl.textContent = flowStarted
      ? pct.toFixed(0) + "% · " + fmtTime(shownNow) + " / " + fmtTime(timeline.total)
      : "0%";

    if (flowStarted && lastStep >= 0) {
      stepLabelEl.textContent = "步骤 " + (lastStep + 1) + "/" + timeline.steps.length +
        " · " + timeline.steps[lastStep];
    } else {
      stepLabelEl.textContent = "未开始";
    }

    let state = "idle";
    let text = "未开始";
    if (flowRunning && paused) {
      state = "pause";
      text = "已暂停";
    } else if (flowRunning) {
      state = "run";
      text = "播放中 " + speed + "×";
    } else if (flowComplete) {
      state = "done";
      text = "已完成";
    } else if (flowAborted) {
      state = "pause";
      text = "手动接管";
    } else if (flowStarted) {
      state = "pause";
      text = "已停止";
    }
    flowStatusEl.dataset.state = state;
    flowStatusEl.textContent = text;
  }

  function updatePlayButton() {
    if (flowRunning) {
      playBtn.textContent = paused ? "▶ 继续" : "⏸ 暂停";
    } else if (flowStarted) {
      playBtn.textContent = "↻ 重新演示";
    } else {
      playBtn.textContent = "▶ 播放完整流程";
    }
  }

  function updateFreezeButton() {
    freezeBtn.textContent = paused ? "▶ 继续时钟" : "⏸ 冻结时钟";
    freezeBtn.classList.toggle("primary", paused);
  }

  /* ------------------------------------------------------------------ */
  /* Log                                                                 */
  /* ------------------------------------------------------------------ */

  function addLog(at, kind, tag, text) {
    logs.push({
      at: Math.max(0, Math.round(at)),
      kind: kind,
      tag: tag,
      text: text || "",
    });
    if (logs.length > 600) logs.splice(0, logs.length - 600);
    renderLog();
  }

  function logEvent(at, type, payload) {
    const name = P.EVENT_NAMES[type];
    switch (type) {
      case EVT.SUBTITLE:
        addLog(at, "event", "SUBTITLE", '"' + (payload.text || "") + '"');
        break;
      case EVT.EMOTION:
        addLog(at, "event", "EMOTION", P.EMOTION_NAMES[payload.value]);
        break;
      case EVT.WIFI:
        addLog(at, "event", "WIFI",
          "connected=" + (payload.flag ? 1 : 0) +
          " ap=" + (payload.flag2 ? 1 : 0) +
          " connecting=" + (payload.flag3 ? 1 : 0));
        break;
      case EVT.BATTERY:
        addLog(at, "event", "BATTERY",
          payload.value + "% present=" + (payload.flag ? 1 : 0) +
          " charging=" + (payload.flag2 ? 1 : 0));
        break;
      case EVT.ERROR:
        addLog(at, "event", "ERROR", P.ERROR_NAMES[payload.value]);
        break;
      default:
        addLog(at, "event", name, "");
    }
  }

  function renderLog() {
    const frag = document.createDocumentFragment();
    const only = onlyScreens.checked;
    let shown = 0;

    for (let i = logs.length - 1; i >= 0; i--) {
      const entry = logs[i];
      if (only && entry.kind !== "screen" && entry.kind !== "system") continue;
      shown++;

      const row = document.createElement("div");
      row.className = "log-row kind-" + entry.kind;

      const time = document.createElement("span");
      time.className = "t";
      time.textContent = fmtTime(entry.at);

      const tag = document.createElement("span");
      tag.className = "tag";
      tag.textContent = entry.tag;

      const msg = document.createElement("span");
      msg.className = "msg";
      msg.textContent = entry.text;

      row.appendChild(time);
      row.appendChild(tag);
      row.appendChild(msg);
      frag.appendChild(row);
    }

    if (!shown) {
      const empty = document.createElement("div");
      empty.className = "log-row";
      empty.style.color = "#6f7c92";
      empty.textContent = "暂无事件，点击“播放完整流程”或注入事件。";
      frag.appendChild(empty);
    }
    logList.replaceChildren(frag);
  }

  function fmtTime(ms) {
    const s = Math.max(0, ms) / 1000;
    if (s < 60) return s.toFixed(2) + "s";
    const m = Math.floor(s / 60);
    const rest = s - m * 60;
    return m + ":" + rest.toFixed(2).padStart(5, "0");
  }

  /* ------------------------------------------------------------------ */
  /* Flow control                                                        */
  /* ------------------------------------------------------------------ */

  function resetSim(clearLog) {
    st = P.createState(0);
    now = 0;
    lastTickAt = 0;
    lastFrame = 0;
    flowIndex = 0;
    flowRunning = false;
    flowComplete = false;
    flowStarted = false;
    flowAborted = false;
    paused = false;
    lastStep = -1;
    override = null;
    toast = null;
    currentScreen = null;
    preview = null;
    renderedPreviewKey = null;
    applyAudioLevel(50);
    if (clearLog) logs = [];
    renderLog();
    render();
  }

  function startFlow() {
    resetSim(true);
    flowStarted = true;
    flowRunning = true;
    flowComplete = false;
    flowAborted = false;
    paused = false;
    addLog(0, "system", "FLOW", "开始自动演示 · " + speed + "×");
    advanceWorld(0);
    render();
  }

  function manual(ev, tag, text) {
    if (flowRunning) {
      flowRunning = false;
      addLog(st.now_ms, "system", "FLOW", "手动接管，自动演示停止");
    }
    if (flowStarted) {
      flowComplete = false;
      flowAborted = true;
    }

    if (preview) {
      preview = null;
      renderedPreviewKey = null;
    }

    const t = Math.max(Math.ceil(now), st.now_ms) + 1;
    P.dispatch(st, ev, t);
    now = t;
    addLog(t, "event", tag, text || "");
    render();
  }

  function skip(ms) {
    now += ms;
    advanceWorld(now);
    render();
  }

  function applyAudioLevel(level) {
    level = Math.max(0, Math.min(100, Math.round(Number(level) || 0)));
    const scale = 40 + Math.floor((level * 120) / 100);
    waveHeights = BASE_WAVES.map((base) => {
      let h = Math.floor((base * scale) / 100);
      if (h < 6) h = 6;
      if (h > 72) h = 72;
      return h;
    });
    audioRange.value = String(level);
    audioVal.textContent = String(level);
    layoutWaves();
  }

  /* ------------------------------------------------------------------ */
  /* Manual controls                                                     */
  /* ------------------------------------------------------------------ */

  const VOICE_EVENTS = [
    EVT.VOICE_DISCONNECTED,
    EVT.VOICE_CONNECTING,
    EVT.VOICE_READY,
    EVT.VOICE_LISTENING,
    EVT.VOICE_THINKING,
    EVT.VOICE_SPEAKING,
    EVT.VOICE_CANCELLING,
  ];

  function onAction(btn) {
    const act = btn.dataset.act;
    switch (act) {
      case "boot":
        manual({ type: EVT.BOOT }, "BOOT");
        break;
      case "ready":
        manual({ type: EVT.READY }, "READY");
        break;
      case "wake":
        manual({ type: EVT.WAKE }, "WAKE");
        break;
      case "sleep":
        manual({ type: EVT.SLEEP }, "SLEEP");
        break;
      case "ptt-down":
        manual({ type: EVT.PTT_DOWN }, "PTT_DOWN");
        break;
      case "ptt-up":
        manual({ type: EVT.PTT_UP }, "PTT_UP");
        break;
      case "interrupt":
        manual({ type: EVT.INTERRUPT }, "INTERRUPT");
        break;
      case "voice": {
        const value = Number(btn.dataset.value);
        if (value === 7) {
          manual({ type: EVT.VOICE_IDLE }, "VOICE_IDLE");
        } else {
          const type = VOICE_EVENTS[value];
          manual({ type: type }, P.EVENT_NAMES[type]);
        }
        break;
      }
      case "wifi": {
        const connected = btn.dataset.connected === "1";
        const ap = btn.dataset.ap === "1";
        const connecting = btn.dataset.connecting === "1";
        manual({
          type: EVT.WIFI,
          flag: connected,
          flag2: ap,
          flag3: connecting,
        }, "WIFI",
        "connected=" + (connected ? 1 : 0) +
        " ap=" + (ap ? 1 : 0) +
        " connecting=" + (connecting ? 1 : 0));
        break;
      }
      case "battery": {
        const pct = Number(btn.dataset.pct);
        const present = btn.dataset.present === "1";
        const charging = btn.dataset.charging === "1";
        manual({
          type: EVT.BATTERY,
          value: pct,
          flag: present,
          flag2: charging,
        }, "BATTERY",
        pct + "% present=" + (present ? 1 : 0) +
        " charging=" + (charging ? 1 : 0));
        break;
      }
      case "emotion": {
        const value = Number(btn.dataset.value);
        manual({ type: EVT.EMOTION, value: value }, "EMOTION", P.EMOTION_NAMES[value]);
        break;
      }
      case "error": {
        const value = Number(btn.dataset.value);
        if (value === 0) {
          manual({ type: EVT.CLEAR_ERROR, value: 0 }, "CLEAR_ERROR");
        } else {
          manual({ type: EVT.ERROR, value: value }, "ERROR", P.ERROR_NAMES[value]);
        }
        break;
      }
      default:
        break;
    }
  }

  document.addEventListener("click", function (e) {
    const btn = e.target.closest("button[data-act]");
    if (btn) onAction(btn);
  });

  playBtn.addEventListener("click", function () {
    if (flowRunning) {
      paused = !paused;
      lastFrame = 0;
      render();
    } else {
      startFlow();
    }
  });

  freezeBtn.addEventListener("click", function () {
    paused = !paused;
    lastFrame = 0;
    addLog(st.now_ms, "system", "CLOCK", paused ? "时钟已冻结" : "时钟继续");
    render();
  });

  resetBtn.addEventListener("click", function () {
    resetSim(true);
    addLog(0, "system", "RESET", "回到 BOOT，时钟归零");
    render();
  });

  speedSelect.addEventListener("change", function () {
    speed = Number(speedSelect.value) || 1;
    addLog(st.now_ms, "system", "SPEED", speed + "×");
    render();
  });

  roundToggle.addEventListener("change", function () {
    screenEl.classList.toggle("round", roundToggle.checked);
  });

  $("skip1").addEventListener("click", function () { skip(1000); });
  $("skip5").addEventListener("click", function () { skip(5000); });

  audioRange.addEventListener("input", function () {
    applyAudioLevel(audioRange.value);
  });
  audioRange.addEventListener("change", function () {
    addLog(st.now_ms, "system", "AUDIO", "level=" + audioRange.value);
  });

  $("subtitleBtn").addEventListener("click", function () {
    const text = $("subtitleInput").value.trim();
    manual({ type: EVT.SUBTITLE, text: text }, "SUBTITLE", text ? '"' + text + '"' : "(clear)");
  });

  $("statusBtn").addEventListener("click", function () {
    const text = $("statusInput").value.trim();
    if (text) {
      override = { text: text, until: now + 5000 };
      addLog(now, "system", "STATUS", '"' + text + '" · 5s');
    } else {
      override = null;
      addLog(now, "system", "STATUS", "覆盖已清除");
    }
    render();
  });

  $("toastBtn").addEventListener("click", function () {
    const text = $("toastInput").value.trim();
    if (text) {
      toast = { text: text, until: now + 2000 };
      addLog(now, "system", "TOAST", '"' + text + '" · 2s');
    } else {
      toast = null;
      addLog(now, "system", "TOAST", "toast 已清除");
    }
    render();
  });

  onlyScreens.addEventListener("change", renderLog);

  $("clearLog").addEventListener("click", function () {
    logs = [];
    renderLog();
  });

  document.querySelectorAll(".tabs button").forEach(function (btn) {
    btn.addEventListener("click", function () {
      const tab = btn.dataset.tab;
      document.querySelectorAll(".tabs button").forEach(function (other) {
        other.classList.toggle("active", other === btn);
      });
      $("logPane").classList.toggle("hidden", tab !== "log");
      $("mapPane").classList.toggle("hidden", tab !== "map");
      $("pagesPane").classList.toggle("hidden", tab !== "pages");
    });
  });

  backLive.addEventListener("click", function () {
    setPreview(null);
  });

  pageGroups.addEventListener("click", function (e) {
    const btn = e.target.closest("button[data-kind]");
    if (!btn) return;
    if (btn.dataset.kind === "live") setPreview(null);
    else if (btn.dataset.kind === "screen") {
      setPreview({ kind: "screen", screen: Number(btn.dataset.screen) });
    } else if (btn.dataset.kind === "page") {
      setPreview({ kind: "page", id: btn.dataset.page });
    }
  });

  pageScene.addEventListener("click", function (e) {
    const node = e.target.closest("[data-link]");
    if (!node) return;
    followPageLink(node.dataset.link);
  });

  /* ------------------------------------------------------------------ */
  /* Screen mapping table                                                */
  /* ------------------------------------------------------------------ */

  function buildMap() {
    const frag = document.createDocumentFragment();

    const head = document.createElement("div");
    head.className = "map-row head";
    ["图像", "SCREEN", "生成文件", "tip / 说明"].forEach(function (label) {
      const cell = document.createElement("div");
      cell.textContent = label;
      head.appendChild(cell);
    });
    frag.appendChild(head);

    SCREEN_INFO.forEach(function (info, index) {
      const row = document.createElement("div");
      row.className = "map-row";

      const img = document.createElement("img");
      img.src = A(info.face);
      img.alt = info.face;

      const enumCell = document.createElement("div");
      enumCell.className = "enum";
      enumCell.textContent = P.SCREEN_NAMES[index];

      const fileCell = document.createElement("div");
      fileCell.className = "file";
      fileCell.textContent = info.file;

      const desc = document.createElement("div");
      desc.className = "desc";
      const badge = info.waves ? "波纹 9 · " : (info.dots ? "点 3 · " : "");
      desc.textContent = badge +
        (info.tip ? info.tip.text + " (" + info.tip.icon + ")" : "无 tip") +
        " — " + info.note;

      row.appendChild(img);
      row.appendChild(enumCell);
      row.appendChild(fileCell);
      row.appendChild(desc);
      frag.appendChild(row);
    });

    mapList.replaceChildren(frag);
  }

  /* ------------------------------------------------------------------ */
  /* Boot                                                                */
  /* ------------------------------------------------------------------ */

  // Preload all 19 face images so a screen switch never shows a blank frame.
  SCREEN_INFO.forEach(function (info) {
    const img = new Image();
    img.src = A(info.face);
  });

  buildMap();
  buildPageGallery();
  resetSim(true);
  addLog(0, "system", "BOOT", "页面就绪 · 点击播放");
  requestAnimationFrame(frame);
})();
