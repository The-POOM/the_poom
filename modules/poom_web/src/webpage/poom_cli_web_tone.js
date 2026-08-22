const PPQ = 480;

const KEY_W = 62;
const NOTE_H = 12;
const RESIZE_EDGE_PX = 8;

const DEFAULT_MIN_NOTE = 48; // C4
const DEFAULT_MAX_NOTE = 84; // C7

const state = {
  bpm: 120,
  quantize: 8,
  name: "",

  minNote: DEFAULT_MIN_NOTE,
  maxNote: DEFAULT_MAX_NOTE,
  scrollTick: 0,
  pxPerTick: 0.14,

  notes: [],
  nextId: 1,
  selectedId: null,
  drag: null,

  playheadActive: false,
  playheadTick: 0,
  playheadStartMs: 0,
  playheadDurationMs: 0,
  playheadRafId: 0,

  timelineMaxTick: 0,
};

const $ = (id) => document.getElementById(id);

const el = {
  canvas: $("tg-roll"),
  stat: $("tg-stat"),
  status: $("tg-status"),
  bpm: $("tg-bpm"),
  quantize: $("tg-quantize"),
  name: $("tg-name"),

  btnNew: $("tg-new"),
  btnLoad: $("tg-load"),
  btnPlayDevice: $("tg-play-device"),
  btnSaveSd: $("tg-save-sd"),

  timelineTrack: $("tg-timeline-track"),
  timelineThumb: $("tg-timeline-thumb"),

  loadOverlay: $("tg-load-overlay"),
  loadModal: $("tg-load-modal"),
  loadClose: $("tg-load-close"),
  loadList: $("tg-load-list"),
};

function setStatus(text) {
  el.status.textContent = String(text || "").trim() || "Ready";
}

function clamp(v, a, b) {
  return Math.max(a, Math.min(b, v));
}

function ticksPerGrid() {
  return (PPQ * 4) / (state.quantize || 8);
}

function tickToMs(ticks) {
  const msPerTick = (60000 / state.bpm) / PPQ;
  return Math.round(ticks * msPerTick);
}

function msPerTick() {
  return (60000 / state.bpm) / PPQ;
}

function visibleTicksForWidth(w) {
  return Math.max(1, Math.floor((Math.max(0, w - KEY_W)) / state.pxPerTick));
}

function songEndTick() {
  let end = 0;
  for (const n of state.notes) {
    if (n.endTick > end) end = n.endTick;
  }
  return end;
}

function ensureTimelineMax_(visibleTicks) {
  const minLen = PPQ * 4 * 4; // 4 bars baseline
  const end = Math.max(songEndTick(), state.scrollTick + visibleTicks, minLen);
  state.timelineMaxTick = Math.max(state.timelineMaxTick || 0, end);
}

function clampScrollTick_(visibleTicks) {
  ensureTimelineMax_(visibleTicks);
  const maxScroll = Math.max(0, state.timelineMaxTick - visibleTicks);
  state.scrollTick = clamp(state.scrollTick, 0, maxScroll);
}

function updateTimelineUI_(visibleTicks) {
  if (!el.timelineThumb || !el.timelineTrack) return;
  ensureTimelineMax_(visibleTicks);

  const maxTick = Math.max(1, state.timelineMaxTick);
  const maxScroll = Math.max(0, maxTick - visibleTicks);
  const widthFrac = clamp(visibleTicks / maxTick, 0.06, 1);
  const leftFrac = (maxScroll > 0) ? (clamp(state.scrollTick / maxScroll, 0, 1) * (1 - widthFrac)) : 0;

  el.timelineThumb.style.width = `${(widthFrac * 100).toFixed(3)}%`;
  el.timelineThumb.style.left = `${(leftFrac * 100).toFixed(3)}%`;
}

function midiNoteToFreq(noteNumber) {
  return Math.round(440 * Math.pow(2, (noteNumber - 69) / 12));
}

function midiNoteName(noteNumber) {
  const names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
  const octave = Math.floor(noteNumber / 12);
  return `${names[noteNumber % 12]}${octave}`;
}

function normalizedName() {
  const raw = (state.name || "").trim();
  if (!raw) return "tone";
  // Keep it filename-friendly.
  const safe = raw.replace(/[^a-zA-Z0-9_-]/g, "_").replace(/_+/g, "_").slice(0, 24);
  return safe || "tone";
}

function sortAndResolveMonophonic(notes) {
  const sorted = notes.slice().sort((a, b) => {
    if (a.startTick !== b.startTick) return a.startTick - b.startTick;
    return b.noteNumber - a.noteNumber; // highest pitch wins on ties
  });

  const out = [];
  for (const n of sorted) {
    if (n.endTick <= n.startTick) continue;
    if (out.length === 0) {
      out.push({ ...n });
      continue;
    }

    const prev = out[out.length - 1];
    if (n.startTick < prev.endTick) {
      // Truncate the previous note to keep monophonic output.
      prev.endTick = n.startTick;
      if (prev.endTick <= prev.startTick) {
        out.pop();
      }
    }
    out.push({ ...n });
  }

  return out;
}

function buildToneJson() {
  const resolved = sortAndResolveMonophonic(state.notes);
  const events = [];
  let lastTick = 0;

  for (const note of resolved) {
    if (note.startTick > lastTick) {
      const restTicks = note.startTick - lastTick;
      const restMs = tickToMs(restTicks);
      if (restMs > 0) {
        events.push([0, restMs]);
      }
    }

    const durMs = tickToMs(note.endTick - note.startTick);
    const freq = clamp(midiNoteToFreq(note.noteNumber), 16, 32767);
    if (durMs > 0) {
      events.push([freq, durMs]);
    }
    lastTick = note.endTick;
  }

  return {
    version: 1,
    name: normalizedName(),
    pause_ms: 0,
    events,
  };
}

function updateOutputs() {
  el.stat.textContent = `Notes: ${state.notes.length}`;
}

function toneDurationMs(tone) {
  if (!tone || !Array.isArray(tone.events)) return 0;
  let total = 0;
  for (const ev of tone.events) {
    const dur = Array.isArray(ev) ? Number(ev[1]) : 0;
    if (Number.isFinite(dur) && dur > 0) total += dur;
  }
  const pause = Number(tone.pause_ms || 0);
  if (Number.isFinite(pause) && pause > 0 && tone.events.length > 1) {
    total += pause * (tone.events.length - 1);
  }
  return Math.max(0, Math.round(total));
}

function stopPlayhead() {
  state.playheadActive = false;
  state.playheadTick = 0;
  state.playheadStartMs = 0;
  state.playheadDurationMs = 0;
  if (state.playheadRafId) {
    cancelAnimationFrame(state.playheadRafId);
    state.playheadRafId = 0;
  }
}

function startPlayhead(durationMs) {
  stopPlayhead();
  const dur = Math.max(0, Math.round(durationMs || 0));
  if (dur <= 0) {
    render();
    return;
  }

  state.playheadActive = true;
  state.playheadStartMs = performance.now();
  state.playheadDurationMs = dur;
  state.playheadTick = 0;

  const step = () => {
    if (!state.playheadActive) return;
    const elapsed = performance.now() - state.playheadStartMs;
    if (elapsed >= state.playheadDurationMs) {
      stopPlayhead();
      render();
      return;
    }

    state.playheadTick = Math.max(0, elapsed / msPerTick());
    render();
    state.playheadRafId = requestAnimationFrame(step);
  };

  state.playheadRafId = requestAnimationFrame(step);
}

function canvasResize() {
  const dpr = window.devicePixelRatio || 1;
  const rect = el.canvas.getBoundingClientRect();
  el.canvas.width = Math.floor(rect.width * dpr);
  el.canvas.height = Math.floor(rect.height * dpr);
  const ctx = el.canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  render();
}

function visibleNotesCount() {
  return Math.floor(el.canvas.getBoundingClientRect().height / NOTE_H);
}

function yToNote(y) {
  const idx = Math.floor(y / NOTE_H);
  const note = state.maxNote - idx;
  return clamp(note, 0, 127);
}

function noteToY(noteNumber) {
  return (state.maxNote - noteNumber) * NOTE_H;
}

function xToTick(x) {
  const px = x - KEY_W;
  const t = state.scrollTick + Math.round(px / state.pxPerTick);
  const q = ticksPerGrid();
  return Math.max(0, Math.round(t / q) * q);
}

function tickToX(tick) {
  return KEY_W + (tick - state.scrollTick) * state.pxPerTick;
}

function hitTest(x, y) {
  for (let i = state.notes.length - 1; i >= 0; i--) {
    const n = state.notes[i];
    const nx = tickToX(n.startTick);
    const ny = noteToY(n.noteNumber);
    const nw = (n.endTick - n.startTick) * state.pxPerTick;
    const nh = NOTE_H - 1;
    if (x >= nx && x <= nx + nw && y >= ny && y <= ny + nh) {
      const edge = (nx + nw) - x;
      return { note: n, resize: edge <= RESIZE_EDGE_PX };
    }
  }
  return null;
}

function drawGrid(ctx, w, h) {
  ctx.fillStyle = "rgba(0,0,0,0.0)";
  ctx.clearRect(0, 0, w, h);

  // Background.
  ctx.fillStyle = "rgba(6, 8, 16, 0.35)";
  ctx.fillRect(0, 0, w, h);

  // Keyboard area.
  ctx.fillStyle = "rgba(0,0,0,0.35)";
  ctx.fillRect(0, 0, KEY_W, h);

  const totalRows = Math.ceil(h / NOTE_H);
  for (let r = 0; r < totalRows; r++) {
    const note = state.maxNote - r;
    const y = r * NOTE_H;
    const isBlack = [1, 3, 6, 8, 10].includes(note % 12);
    ctx.fillStyle = isBlack ? "rgba(0,0,0,0.24)" : "rgba(255,255,255,0.03)";
    ctx.fillRect(KEY_W, y, w - KEY_W, NOTE_H);

    // Keyboard key shading.
    ctx.fillStyle = isBlack ? "rgba(0,0,0,0.45)" : "rgba(255,255,255,0.06)";
    ctx.fillRect(0, y, KEY_W, NOTE_H);

    // Note labels (C only).
    if (note % 12 === 0) {
      ctx.fillStyle = "rgba(255,255,255,0.75)";
      ctx.font = "10px ui-sans-serif, system-ui";
      ctx.fillText(midiNoteName(note), 6, y + 10);
    }
  }

  // Vertical grid.
  const q = ticksPerGrid();
  const pxW = w - KEY_W;
  const ticksVisible = Math.ceil(pxW / state.pxPerTick);
  const startTick = Math.max(0, state.scrollTick - (state.scrollTick % q));

  for (let t = startTick; t <= state.scrollTick + ticksVisible + q; t += q) {
    const x = tickToX(t);
    const isBar = (t % (PPQ * 4) === 0);
    ctx.strokeStyle = isBar ? "rgba(255,255,255,0.22)" : "rgba(255,255,255,0.08)";
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, h);
    ctx.stroke();
  }

  // Keyboard separator.
  ctx.strokeStyle = "rgba(255,255,255,0.22)";
  ctx.beginPath();
  ctx.moveTo(KEY_W + 0.5, 0);
  ctx.lineTo(KEY_W + 0.5, h);
  ctx.stroke();
}

function drawNotes(ctx) {
  for (const n of state.notes) {
    const x = tickToX(n.startTick);
    const y = noteToY(n.noteNumber);
    const w = Math.max(2, (n.endTick - n.startTick) * state.pxPerTick);
    const h = NOTE_H - 2;

    const selected = n.id === state.selectedId;
    ctx.fillStyle = selected ? "rgba(25,230,197,0.85)" : "rgba(211,77,255,0.75)";
    ctx.fillRect(x + 0.5, y + 1, w - 1, h);

    ctx.strokeStyle = selected ? "rgba(255,255,255,0.9)" : "rgba(255,255,255,0.25)";
    ctx.strokeRect(x + 0.5, y + 1, w - 1, h);
  }
}

function drawPlayhead(ctx, w, h) {
  if (!state.playheadActive) return;

  const x = tickToX(state.playheadTick);
  if (x < (KEY_W - 40) || x > (w + 40)) {
    return;
  }

  ctx.save();
  ctx.strokeStyle = "rgba(255, 60, 90, 0.95)";
  ctx.lineWidth = 1.5;
  ctx.shadowColor = "rgba(255, 60, 90, 0.35)";
  ctx.shadowBlur = 6;
  ctx.beginPath();
  ctx.moveTo(x + 0.5, 0);
  ctx.lineTo(x + 0.5, h);
  ctx.stroke();
  ctx.restore();
}

function render() {
  const ctx = el.canvas.getContext("2d");
  const rect = el.canvas.getBoundingClientRect();
  const w = rect.width;
  const h = rect.height;
  const visibleTicks = visibleTicksForWidth(w);
  clampScrollTick_(visibleTicks);

  // Keep note range aligned with view height.
  const rows = visibleNotesCount();
  if (state.maxNote - state.minNote + 1 !== rows) {
    state.maxNote = state.minNote + rows - 1;
  }

  drawGrid(ctx, w, h);
  drawNotes(ctx);
  drawPlayhead(ctx, w, h);
  updateTimelineUI_(visibleTicks);
}

function addNoteAt(x, y) {
  const noteNumber = yToNote(y);
  const startTick = xToTick(x);
  const endTick = startTick + ticksPerGrid();

  const note = {
    id: state.nextId++,
    noteNumber,
    startTick,
    endTick,
  };
  state.notes.push(note);
  state.selectedId = note.id;
  updateOutputs();
  render();
}

function deleteNote(note) {
  state.notes = state.notes.filter((n) => n.id !== note.id);
  if (state.selectedId === note.id) state.selectedId = null;
  updateOutputs();
  render();
}

function pointerPos(ev) {
  const rect = el.canvas.getBoundingClientRect();
  return {
    x: ev.clientX - rect.left,
    y: ev.clientY - rect.top,
  };
}

function onPointerDown(ev) {
  const p = pointerPos(ev);
  const hit = hitTest(p.x, p.y);

  if (ev.button === 2) { // right click
    if (hit && hit.note) {
      deleteNote(hit.note);
    }
    return;
  }

  if (hit && hit.note) {
    state.selectedId = hit.note.id;
    state.drag = {
      id: hit.note.id,
      mode: hit.resize ? "resize" : "move",
      startX: p.x,
      startY: p.y,
      grabOffsetTick: hit.note.startTick - xToTick(p.x),
      lenTicks: Math.max(ticksPerGrid(), hit.note.endTick - hit.note.startTick),
      noteNumber: hit.note.noteNumber,
    };
    render();
    return;
  }

  addNoteAt(p.x, p.y);
  const rect = el.canvas.getBoundingClientRect();
  const w = rect.width;
  const edge = 22;
  if (p.x > (w - edge)) {
    state.scrollTick += ticksPerGrid() * 4;
    render();
  }
}

function onPointerMove(ev) {
  if (!state.drag) return;
  const p = pointerPos(ev);
  const note = state.notes.find((n) => n.id === state.drag.id);
  if (!note) return;

  const rect = el.canvas.getBoundingClientRect();
  const w = rect.width;
  const edge = 22;
  const q = ticksPerGrid();
  if (p.x > (w - edge)) {
    state.scrollTick += q * 2;
  } else if (p.x < (KEY_W + edge)) {
    state.scrollTick = Math.max(0, state.scrollTick - q * 2);
  }

  if (state.drag.mode === "move") {
    const dy = p.y - state.drag.startY;
    const base = xToTick(p.x) + state.drag.grabOffsetTick;
    note.startTick = Math.max(0, Math.round(base / q) * q);
    note.endTick = note.startTick + state.drag.lenTicks;

    const noteDelta = Math.round(dy / NOTE_H);
    note.noteNumber = clamp(state.drag.noteNumber - noteDelta, 0, 127);
  } else {
    const newEnd = Math.round(xToTick(p.x) / q) * q;
    note.endTick = Math.max(note.startTick + q, newEnd);
  }

  updateOutputs();
  render();
}

function onPointerUp() {
  state.drag = null;
}

function onWheel(ev) {
  ev.preventDefault();
  const delta = Math.sign(ev.deltaY);
  if (ev.shiftKey) {
    const q = ticksPerGrid();
    state.scrollTick = Math.max(0, state.scrollTick + delta * q * 2);
  } else {
    state.minNote = clamp(state.minNote + delta, 0, 127);
  }
  render();
}

async function postJson(url, payload) {
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  const text = await res.text();
  if (!res.ok) {
    throw new Error(text || `HTTP ${res.status}`);
  }
  try {
    return JSON.parse(text);
  } catch (_) {
    return { ok: true, text };
  }
}

async function playOnDevice() {
  const tone = buildToneJson();
  startPlayhead(toneDurationMs(tone));
  setStatus("Sending to device...");
  const resp = await postJson("/tone/play", tone);
  setStatus(resp && resp.ok ? "Playing on POOM" : "Sent");
}

async function saveToSd() {
  const tone = buildToneJson();
  setStatus("Saving to SD...");
  const resp = await postJson("/tone/save", tone);
  if (resp && resp.ok && resp.path) {
    setStatus(`Saved: ${resp.path}`);
  } else {
    setStatus("Saved");
  }
}

function formatBytes(size) {
  const n = Number(size || 0);
  if (!Number.isFinite(n) || n <= 0) return "0 B";
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}

function freqToMidi(freq) {
  const f = Number(freq || 0);
  if (!Number.isFinite(f) || f <= 0) return 0;
  const n = Math.round(69 + 12 * Math.log2(f / 440));
  return clamp(n, 0, 127);
}

function parseToneText(content) {
  const text = String(content || "");
  const trimmed = text.trimStart();
  if (!trimmed) throw new Error("empty file");

  // Backward compatibility: accept old JSON .tone files.
  if (trimmed.startsWith("{")) {
    const obj = JSON.parse(trimmed);
    if (!obj || !Array.isArray(obj.events)) throw new Error("invalid JSON tone");
    return {
      name: String(obj.name || "tone"),
      pause_ms: Number(obj.pause_ms || 0) || 0,
      events: obj.events,
    };
  }

  let name = "tone";
  let pause = 0;
  const events = [];
  let inEvents = false;
  const lines = text.split(/\r?\n/);

  for (const rawLine of lines) {
    let line = rawLine.trim();
    if (!line || line.startsWith("#")) continue;
    if (/^POOM(TONE|[-_ ]TONE)/i.test(line)) continue;

    const mName = line.match(/^name\s*[:=]\s*(.+)$/i);
    if (mName) {
      name = mName[1].trim().slice(0, 48) || name;
      continue;
    }
    const mPause = line.match(/^pause(_ms)?\s*[:=]\s*(\d+)/i);
    if (mPause) {
      pause = clamp(Number(mPause[2]) || 0, 0, 10000);
      continue;
    }
    if (/^events\s*[:=]?\s*$/i.test(line)) {
      inEvents = true;
      continue;
    }

    // Event line: "freq dur" or "freq,dur"
    line = line.replace(/[;,]+/g, " ");
    const parts = line.split(/\s+/).filter(Boolean);
    if (parts.length < 2) continue;
    const freq = Number(parts[0]);
    const dur = Number(parts[1]);
    if (!Number.isFinite(dur) || dur <= 0) continue;
    if (!inEvents) {
      // Allow event lines even without an explicit "events:" header.
      inEvents = true;
    }
    events.push([Number.isFinite(freq) ? freq : 0, dur]);
  }

  if (events.length === 0) throw new Error("no events");
  return { name, pause_ms: pause, events };
}

function loadToneIntoEditor(tone) {
  const q = ticksPerGrid();
  const tickMs = msPerTick();
  let tick = 0;
  const notes = [];

  for (const ev of tone.events || []) {
    if (!Array.isArray(ev) || ev.length < 2) continue;
    const freq = Number(ev[0] || 0);
    const durMs = Number(ev[1] || 0);
    if (!Number.isFinite(durMs) || durMs <= 0) continue;

    let durTicks = Math.round(durMs / tickMs);
    durTicks = Math.max(q, Math.round(durTicks / q) * q);

    if (freq > 0) {
      notes.push({
        id: notes.length + 1,
        noteNumber: freqToMidi(freq),
        startTick: tick,
        endTick: tick + durTicks,
      });
    }
    tick += durTicks;
  }

  state.notes = notes;
  state.nextId = notes.length + 1;
  state.selectedId = notes.length ? notes[0].id : null;
  state.scrollTick = 0;
  state.timelineMaxTick = 0;

  const nm = (tone.name || "").trim();
  state.name = nm;
  if (el.name) el.name.value = nm;

  stopPlayhead();
  updateOutputs();
  render();
}

function openLoadModal() {
  el.loadOverlay?.classList.remove("hidden");
  el.loadModal?.classList.remove("hidden");
}

function closeLoadModal() {
  el.loadOverlay?.classList.add("hidden");
  el.loadModal?.classList.add("hidden");
}

async function refreshSdToneList() {
  if (!el.loadList) return;
  el.loadList.textContent = "Loading...";
  const res = await fetch("/files/list?dir=tones", { cache: "no-store" });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error((data && data.error) || `HTTP ${res.status}`);

  const entries = Array.isArray(data.entries) ? data.entries : [];
  const tones = entries
    .filter((e) => e && !e.is_dir && typeof e.name === "string" && e.name.toLowerCase().endsWith(".tone"))
    .sort((a, b) => String(a.name).localeCompare(String(b.name)));

  el.loadList.innerHTML = "";
  if (tones.length === 0) {
    el.loadList.textContent = "No .tone files found.";
    return;
  }

  for (const t of tones) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "tg-file-item";
    btn.innerHTML = `<div class="tg-file-name"></div><div class="tg-file-meta"></div>`;
    btn.querySelector(".tg-file-name").textContent = t.name;
    btn.querySelector(".tg-file-meta").textContent = formatBytes(t.size);
    btn.addEventListener("click", async () => {
      try {
        setStatus(`Loading: ${t.name}`);
        const r = await fetch(`/files/view?path=${encodeURIComponent(t.path)}`, { cache: "no-store" });
        if (!r.ok) throw new Error(await r.text());
        const txt = await r.text();
        const tone = parseToneText(txt);
        loadToneIntoEditor(tone);
        setStatus(`Loaded: ${t.name}`);
        closeLoadModal();
      } catch (e) {
        setStatus(`ERR: ${e.message}`);
      }
    });
    el.loadList.appendChild(btn);
  }
}

function bindTimeline() {
  if (!el.timelineTrack || !el.timelineThumb) return;

  const getInfo = () => {
    const rect = el.timelineTrack.getBoundingClientRect();
    const canvasRect = el.canvas.getBoundingClientRect();
    const visibleTicks = visibleTicksForWidth(canvasRect.width);
    ensureTimelineMax_(visibleTicks);
    return { rect, visibleTicks, maxTick: Math.max(1, state.timelineMaxTick) };
  };

  let dragging = false;

  const setFromFrac = (frac, visibleTicks, maxTick) => {
    const maxScroll = Math.max(0, maxTick - visibleTicks);
    state.scrollTick = clamp(Math.round(frac * maxScroll), 0, maxScroll);
    render();
  };

  el.timelineTrack.addEventListener("pointerdown", (ev) => {
    const { rect, visibleTicks, maxTick } = getInfo();
    const x = clamp(ev.clientX - rect.left, 0, rect.width);
    const frac = rect.width > 0 ? x / rect.width : 0;
    setFromFrac(frac, visibleTicks, maxTick);
    dragging = true;
    el.timelineTrack.setPointerCapture(ev.pointerId);
  });

  el.timelineTrack.addEventListener("pointermove", (ev) => {
    if (!dragging) return;
    const { rect, visibleTicks, maxTick } = getInfo();
    const x = clamp(ev.clientX - rect.left, 0, rect.width);
    const frac = rect.width > 0 ? x / rect.width : 0;
    setFromFrac(frac, visibleTicks, maxTick);
  });

  const end = () => { dragging = false; };
  el.timelineTrack.addEventListener("pointerup", end);
  el.timelineTrack.addEventListener("pointercancel", end);
}

function resetSong() {
  state.notes = [];
  state.selectedId = null;
  state.scrollTick = 0;
  state.timelineMaxTick = 0;
  state.minNote = DEFAULT_MIN_NOTE;
  state.maxNote = DEFAULT_MAX_NOTE;
  stopPlayhead();
  updateOutputs();
  render();
}

function bindUi() {
  el.bpm.addEventListener("change", () => {
    const v = Number(el.bpm.value);
    state.bpm = clamp(Number.isFinite(v) ? v : 120, 30, 3000);
    el.bpm.value = String(state.bpm);
    updateOutputs();
  });
  el.quantize.addEventListener("change", () => {
    const v = Number(el.quantize.value);
    state.quantize = clamp(Number.isFinite(v) ? v : 8, 4, 32);
    updateOutputs();
    render();
  });
  el.name.addEventListener("input", () => {
    state.name = el.name.value;
    updateOutputs();
  });

  el.btnNew.addEventListener("click", resetSong);
  el.btnLoad?.addEventListener("click", () => {
    openLoadModal();
    refreshSdToneList().catch((e) => setStatus(`ERR: ${e.message}`));
  });
  el.btnPlayDevice.addEventListener("click", () => playOnDevice().catch((e) => setStatus(`ERR: ${e.message}`)));
  el.btnSaveSd.addEventListener("click", () => saveToSd().catch((e) => setStatus(`ERR: ${e.message}`)));

  el.loadOverlay?.addEventListener("click", closeLoadModal);
  el.loadClose?.addEventListener("click", closeLoadModal);

  el.canvas.addEventListener("contextmenu", (ev) => ev.preventDefault());
  el.canvas.addEventListener("pointerdown", onPointerDown);
  window.addEventListener("pointermove", onPointerMove);
  window.addEventListener("pointerup", onPointerUp);
  el.canvas.addEventListener("wheel", onWheel, { passive: false });

  window.addEventListener("keydown", (ev) => {
    if (ev.key === "Delete" || ev.key === "Backspace") {
      const note = state.notes.find((n) => n.id === state.selectedId);
      if (note) {
        deleteNote(note);
        setStatus("Deleted");
      }
    }
    if (ev.key === "Home") {
      state.scrollTick = 0;
      render();
    }
  });
}

function init() {
  state.bpm = clamp(Number(el.bpm.value) || 120, 30, 3000);
  state.quantize = clamp(Number(el.quantize.value) || 8, 4, 32);
  state.name = el.name.value || "";

  bindUi();
  bindTimeline();
  updateOutputs();
  canvasResize();
  window.addEventListener("resize", canvasResize);
  render();
}

init();
