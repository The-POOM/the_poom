const els = {
  key: document.getElementById("key"),
  scale: document.getElementById("scale"),
  tempo: document.getElementById("tempo"),
  octave: document.getElementById("octave"),
  channel: document.getElementById("channel"),
  program: document.getElementById("program"),
  pattern: document.getElementById("pattern"),
  duration: document.getElementById("duration"),
  loop: document.getElementById("loop"),
  progression: document.getElementById("progression"),
  json: document.getElementById("json"),
  status: document.getElementById("status"),
  btnGenerate: document.getElementById("btn-generate"),
  btnSend: document.getElementById("btn-send"),
  btnStop: document.getElementById("btn-stop"),
  btnSave: document.getElementById("btn-save"),
  btnSaveSd: document.getElementById("btn-save-sd"),
  btnPreview: document.getElementById("btn-preview"),
};

const STORAGE_KEY = "poom_midi_harmony_v1";

function setStatus(text) {
  els.status.textContent = text;
}

function parseProgression(text) {
  const raw = text.split(",").map((s) => s.trim()).filter(Boolean);
  return raw.map((ch) => ({ chord: ch, duration_beats: Number(els.duration.value || 1) }));
}

function buildJson() {
  const obj = {
    type: "poom_midi_harmony",
    version: 1,
    tempo_bpm: Number(els.tempo.value || 120),
    key: els.key.value,
    scale: els.scale.value,
    octave: Number(els.octave.value || 4),
    channel: Number(els.channel.value || 0),
    program: Number(els.program.value || 0),
    loop: !!els.loop.checked,
    pattern: els.pattern.value,
    steps: parseProgression(els.progression.value),
  };
  return obj;
}

function generate() {
  const obj = buildJson();
  const text = JSON.stringify(obj, null, 2);
  els.json.value = text;
  localStorage.setItem(STORAGE_KEY, text);
  setStatus("JSON generated.");
}

async function sendToPoom() {
  const body = els.json.value.trim() || JSON.stringify(buildJson());
  setStatus("Sending...");
  try {
    const res = await fetch("/api/midi_harmony", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body,
    });
    const text = await res.text();
    setStatus(res.ok ? `OK: ${text}` : `ERR: ${text}`);
  } catch (e) {
    setStatus(`Send failed: ${e}`);
  }
}

async function stopOnPoom() {
  setStatus("Stopping...");
  try {
    const res = await fetch("/api/midi_harmony/stop", { method: "POST" });
    const text = await res.text();
    setStatus(res.ok ? `OK: ${text}` : `ERR: ${text}`);
  } catch (e) {
    setStatus(`Stop failed: ${e}`);
  }
}

function downloadJson() {
  const text = els.json.value.trim() || JSON.stringify(buildJson(), null, 2);
  const blob = new Blob([text], { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "poom_midi_harmony.json";
  document.body.appendChild(a);
  a.click();
  a.remove();
  setStatus("Downloaded JSON.");
}

function defaultSaveName() {
  const key = String(els.key.value || "C").replace("#", "s");
  const scale = String(els.scale.value || "major");
  const prog = String(els.progression.value || "I,V,vi,IV").replace(/[^0-9a-zA-Z,_-]+/g, "").slice(0, 18);
  return `harmony_${key}_${scale}_${prog || "prog"}`;
}

async function saveToSd() {
  const suggested = defaultSaveName();
  const rawName = window.prompt("Save name (no slashes). Will be saved as .json on SD:", suggested);
  const name = String(rawName || "").trim();
  if (!name) {
    setStatus("Save canceled.");
    return;
  }

  const body = els.json.value.trim() || JSON.stringify(buildJson());
  setStatus("Saving to SD...");
  try {
    const res = await fetch(`/api/midi_harmony/save?name=${encodeURIComponent(name)}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body,
    });
    const text = await res.text();
    setStatus(res.ok ? `Saved: ${text}` : `ERR: ${text}`);
  } catch (e) {
    setStatus(`Save failed: ${e}`);
  }
}

// Minimal Web MIDI preview: plays the first step chord repeatedly.
async function previewWebMidi() {
  if (!navigator.requestMIDIAccess) {
    setStatus("Web MIDI not supported; using audio preview.");
    return previewAudio();
  }
  setStatus("Requesting MIDI access...");
  const access = await navigator.requestMIDIAccess();
  const outputs = Array.from(access.outputs.values());
  if (outputs.length === 0) {
    setStatus("No MIDI outputs available.");
    return;
  }
  const out = outputs[0];
  const cfg = buildJson();
  setStatus(`Preview on: ${out.name}`);

  const channel = Math.max(0, Math.min(15, cfg.channel | 0));
  const statusPc = 0xC0 | channel;
  out.send([statusPc, cfg.program & 0x7f]);

  const tempoMs = Math.max(200, Math.floor(60000 / Math.max(20, cfg.tempo_bpm)));
  const step = cfg.steps[0] || { chord: "I", duration_beats: 1 };
  const dur = tempoMs * Math.max(1, step.duration_beats | 0);

  // Simplified: just play a fixed triad around middle C based on key; real chord mapping happens on POOM.
  const keyMap = { "C":0,"C#":1,"D":2,"D#":3,"E":4,"F":5,"F#":6,"G":7,"G#":8,"A":9,"A#":10,"B":11 };
  const base = 12 * (cfg.octave + 1) + (keyMap[cfg.key] ?? 0);
  const notes = [base, base + 4, base + 7].map((n) => Math.max(0, Math.min(127, n)));

  const on = 0x90 | channel;
  const off = 0x80 | channel;

  notes.forEach((n) => out.send([on, n, 96]));
  setTimeout(() => notes.forEach((n) => out.send([off, n, 0])), Math.max(40, dur - 40));
  setTimeout(() => setStatus("Preview done."), dur + 20);
}

let audioCtx = null;
let audioStopFn = null;

function keySemitone(key) {
  const keyMap = { "C":0,"C#":1,"D":2,"D#":3,"E":4,"F":5,"F#":6,"G":7,"G#":8,"A":9,"A#":10,"B":11 };
  return keyMap[key] ?? 0;
}

function romanToDegree(roman) {
  const r = String(roman || "").trim();
  const up = r.toUpperCase();
  if (up === "I") return 0;
  if (up === "II") return 1;
  if (up === "III") return 2;
  if (up === "IV") return 3;
  if (up === "V") return 4;
  if (up === "VI") return 5;
  if (up === "VII") return 6;
  return 0;
}

function scaleSemitones(scale) {
  if (scale === "minor") return [0, 2, 3, 5, 7, 8, 10];
  if (scale === "pentatonic_major") return [0, 2, 4, 7, 9];
  if (scale === "pentatonic_minor") return [0, 3, 5, 7, 10];
  return [0, 2, 4, 5, 7, 9, 11]; // major
}

function buildTriadNotes(cfg, degreeIdx) {
  const base = 12 * (cfg.octave + 1) + keySemitone(cfg.key);
  const semis = scaleSemitones(cfg.scale);
  const len = semis.length;

  const d0 = degreeIdx % len;
  const d1 = (d0 + 2) % len;
  const d2 = (d0 + 4) % len;

  const n0 = Math.max(0, Math.min(127, base + semis[d0]));
  const n1 = Math.max(0, Math.min(127, base + semis[d1]));
  const n2 = Math.max(0, Math.min(127, base + semis[d2]));
  return [n0, n1, n2];
}

function midiToFreq(note) {
  return 440 * Math.pow(2, (note - 69) / 12);
}

function ensureAudio() {
  if (!audioCtx) {
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  }
  return audioCtx;
}

function stopAudioPreview() {
  if (audioStopFn) {
    audioStopFn();
    audioStopFn = null;
  }
}

function previewAudio() {
  stopAudioPreview();

  const cfg = buildJson();
  const steps = cfg.steps || [];
  if (steps.length === 0) {
    setStatus("No steps to preview.");
    return;
  }

  const ctx = ensureAudio();
  ctx.resume?.();

  const beatMs = Math.max(50, Math.floor(60000 / Math.max(20, cfg.tempo_bpm || 120)));
  const startAt = ctx.currentTime + 0.05;

  const master = ctx.createGain();
  master.gain.value = 0.12;
  master.connect(ctx.destination);

  const scheduled = [];
  const noteOn = (t, note, lenSec, gain) => {
    const osc = ctx.createOscillator();
    const g = ctx.createGain();
    const f = midiToFreq(note);
    osc.type = "triangle";
    osc.frequency.setValueAtTime(f, t);
    g.gain.setValueAtTime(0.0001, t);
    g.gain.exponentialRampToValueAtTime(Math.max(0.0001, gain), t + 0.01);
    g.gain.exponentialRampToValueAtTime(0.0001, t + Math.max(0.02, lenSec));
    osc.connect(g);
    g.connect(master);
    osc.start(t);
    osc.stop(t + Math.max(0.03, lenSec + 0.03));
    scheduled.push(osc, g);
  };

  let t = startAt;
  for (const st of steps) {
    const durBeats = Math.max(1, Number(st.duration_beats || cfg.duration_beats || 1));
    const stepSec = (beatMs * durBeats) / 1000;

    const deg = romanToDegree(st.chord);
    const triad = buildTriadNotes(cfg, deg);

    if (cfg.pattern === "arpeggio_up" || cfg.pattern === "arpeggio_down") {
      const order = cfg.pattern === "arpeggio_down" ? [2, 1, 0] : [0, 1, 2];
      const slice = Math.max(0.06, stepSec / 3);
      for (let i = 0; i < 3; i++) {
        noteOn(t + i * slice, triad[order[i]], slice * 0.7, 0.55);
      }
    } else if (cfg.pattern === "bass_chord") {
      noteOn(t, triad[0], Math.max(0.06, stepSec * 0.45), 0.6);
      const holdAt = t + stepSec * 0.45;
      for (let i = 0; i < 3; i++) {
        noteOn(holdAt, triad[i], Math.max(0.06, stepSec * 0.5), 0.45);
      }
    } else {
      for (let i = 0; i < 3; i++) {
        noteOn(t, triad[i], Math.max(0.06, stepSec * 0.9), 0.45);
      }
    }

    t += stepSec;
  }

  const doneInMs = Math.max(1, Math.floor((t - startAt) * 1000));
  const timer = setTimeout(() => setStatus("Audio preview done."), doneInMs + 30);
  audioStopFn = () => {
    clearTimeout(timer);
    try { master.disconnect(); } catch {}
    setStatus("Audio preview stopped.");
  };

  setStatus("Audio preview playing...");
}

els.btnSaveSd?.addEventListener("click", saveToSd);

els.btnGenerate.addEventListener("click", generate);
els.btnSend.addEventListener("click", sendToPoom);
els.btnStop.addEventListener("click", () => { stopAudioPreview(); stopOnPoom(); });
els.btnSave.addEventListener("click", downloadJson);
els.btnPreview.addEventListener("click", () => previewWebMidi().catch((e) => setStatus(`Preview failed: ${e}`)));

// Load last JSON if present.
const saved = localStorage.getItem(STORAGE_KEY);
if (saved) {
  els.json.value = saved;
}
generate();
