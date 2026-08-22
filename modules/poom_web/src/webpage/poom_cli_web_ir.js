const irPathNode = document.getElementById("ir-path");
const irLayoutNode = document.getElementById("ir-layout");
const irStatusNode = document.getElementById("ir-status");
const irContentNode = document.getElementById("ir-content");
const irSubtitleNode = document.getElementById("ir-subtitle");

const pathParam = new URLSearchParams(window.location.search).get("path");
const requestedPath = normalizeRelativePath(pathParam);
const absolutePath = requestedPath ? `/sdcard/${requestedPath}` : "/sdcard";

const irState = {
  token: "",
  commands: [],
  sending: false,
};

const SLOT_ALIASES = {
  powerOn: ["power on", "poweron", "turn on", "on"],
  powerOff: ["power off", "poweroff", "turn off", "off"],
  power: ["power", "standby", "toggle power"],
  menu: ["menu", "settings"],
  info: ["info", "display", "epg"],
  up: ["up", "nav up", "cursor up", "dpad up"],
  left: ["left", "nav left", "cursor left", "dpad left"],
  ok: ["ok", "enter", "select", "confirm", "center"],
  right: ["right", "nav right", "cursor right", "dpad right"],
  down: ["down", "nav down", "cursor down", "dpad down"],
  back: ["back", "return", "exit"],
  home: ["home", "guide"],
  volUp: ["vol+", "volume+", "vol up", "volume up", "volumeup", "volup", "louder"],
  volDown: ["vol-", "volume-", "vol down", "volume down", "volumedown", "voldown", "quieter"],
  chUp: ["ch+", "channel+", "channel up", "channelup", "ch up", "chup", "program up"],
  chDown: ["ch-", "channel-", "channel down", "ch down", "channeldown", "chdown", "program down"],
  mute: ["mute", "silence"],
  prev: ["prev", "previous", "rewind", "skip back", "backward"],
  playPause: ["play pause", "playpause", "pause play"],
  play: ["play", "resume"],
  pause: ["pause"],
  stop: ["stop"],
  next: ["next", "forward", "fast forward", "skip next"],
  red: ["red", "r"],
  green: ["green", "g"],
  yellow: ["yellow", "y"],
  blue: ["blue", "b"],
  source: ["source", "input"],
  hdmi1: ["hdmi1", "hdmi 1"],
  hdmi2: ["hdmi2", "hdmi 2"],
  hdmi3: ["hdmi3", "hdmi 3"],
  av: ["av", "tv", "video"],
  usb: ["usb"],
  picture: ["picture"],
  sound: ["sound", "audio"],
  sleep: ["sleep"],
  eco: ["eco"],
  game: ["game"],
  cool: ["cool"],
  heat: ["heat"],
  fan: ["fan"],
  timer: ["timer"],
  mode: ["mode"],
};

const DIGIT_ALIASES = {
  0: ["0", "zero", "num0", "digit0"],
  1: ["1", "one", "num1", "digit1"],
  2: ["2", "two", "num2", "digit2"],
  3: ["3", "three", "num3", "digit3"],
  4: ["4", "four", "num4", "digit4"],
  5: ["5", "five", "num5", "digit5"],
  6: ["6", "six", "num6", "digit6"],
  7: ["7", "seven", "num7", "digit7"],
  8: ["8", "eight", "num8", "digit8"],
  9: ["9", "nine", "num9", "digit9"],
};

const GENERIC_SECTION_ORDER = [
  "Power",
  "Navigation",
  "Numbers",
  "Volume",
  "Media",
  "Inputs",
  "Modes",
  "Other Commands",
];

function getFileName(pathText) {
  const cleanPath = normalizeRelativePath(pathText);
  if (!cleanPath) {
    return "";
  }

  const parts = cleanPath.split("/").filter(Boolean);
  return parts.length > 0 ? parts[parts.length - 1] : cleanPath;
}

function normalizeRelativePath(pathText) {
  if (typeof pathText !== "string") {
    return "";
  }

  return pathText
    .trim()
    .replace(/^\/+/, "")
    .split("/")
    .filter((segment) => segment && segment !== "." && segment !== "..")
    .join("/");
}

function normalizeName(text) {
  return String(text || "")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, " ")
    .trim()
    .replace(/\s+/g, " ");
}

function compactName(text) {
  return normalizeName(text).replace(/\s+/g, "");
}

function commandMeta(command) {
  return {
    index: Number(command.index),
    name: String(command.name || ""),
    normalized: normalizeName(command.name),
    compact: compactName(command.name),
    tokens: normalizeName(command.name).split(" ").filter(Boolean),
  };
}

function matchesAlias(command, aliases) {
  if (!command || !Array.isArray(aliases)) {
    return false;
  }

  return aliases.some((alias) => {
    const aliasCompact = compactName(alias);
    if (!aliasCompact) {
      return false;
    }

    if (command.compact === aliasCompact || command.tokens.includes(aliasCompact)) {
      return true;
    }

    if (aliasCompact.length >= 4 && command.compact.includes(aliasCompact)) {
      return true;
    }

    return false;
  });
}

function takeCommand(commands, usedIndexes, aliases) {
  const found = commands.find((command) => !usedIndexes.has(command.index) && matchesAlias(command, aliases));
  if (!found) {
    return null;
  }

  usedIndexes.add(found.index);
  return found;
}

function countMatching(commands, aliasGroups) {
  const seen = new Set();

  aliasGroups.forEach((aliases) => {
    const found = commands.find((command) => !seen.has(command.index) && matchesAlias(command, aliases));
    if (found) {
      seen.add(found.index);
    }
  });

  return seen.size;
}

function detectTvLayout(commands) {
  const digitCount = countMatching(commands, Object.values(DIGIT_ALIASES));
  const navCount = countMatching(commands, [
    SLOT_ALIASES.up,
    SLOT_ALIASES.down,
    SLOT_ALIASES.left,
    SLOT_ALIASES.right,
    SLOT_ALIASES.ok,
  ]);
  const volumeCount = countMatching(commands, [
    SLOT_ALIASES.volUp,
    SLOT_ALIASES.volDown,
    SLOT_ALIASES.chUp,
    SLOT_ALIASES.chDown,
    SLOT_ALIASES.mute,
  ]);
  const tvKeyCount = countMatching(commands, [
    SLOT_ALIASES.menu,
    SLOT_ALIASES.info,
    SLOT_ALIASES.home,
    SLOT_ALIASES.back,
    SLOT_ALIASES.red,
    SLOT_ALIASES.green,
    SLOT_ALIASES.yellow,
    SLOT_ALIASES.blue,
  ]);

  return digitCount >= 6 || ((navCount >= 4) && (volumeCount >= 2)) || ((digitCount >= 3) && (navCount >= 3) && (tvKeyCount >= 2));
}

function statusMessage(message, isError = false) {
  irStatusNode.textContent = message;
  irStatusNode.classList.toggle("error", !!isError);
}

function layoutBadge(label) {
  if (!label) {
    irLayoutNode.classList.add("hidden");
    irLayoutNode.textContent = "";
    return;
  }

  irLayoutNode.textContent = label;
  irLayoutNode.classList.remove("hidden");
}

function createButton(command, label, extraClass = "") {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `ir-btn${extraClass ? ` ${extraClass}` : ""}`;
  button.textContent = label;
  button.title = command.name || label;
  button.dataset.commandIndex = String(command.index);
  button.addEventListener("click", () => {
    void sendCommand(command, label, button);
  });
  return button;
}

function appendButtons(container, buttons) {
  buttons.forEach((button) => {
    container.appendChild(button);
  });
}

function createDynamicRow(buttonSpecs) {
  const buttons = buttonSpecs.filter((spec) => !!spec.command)
    .map((spec) => createButton(spec.command, spec.label, spec.className || ""));

  if (buttons.length === 0) {
    return null;
  }

  const row = document.createElement("div");
  row.className = "ir-dynamic-row";
  if (buttons.length === 1) {
    row.classList.add("ir-row-single");
  } else if (buttons.length === 2) {
    row.classList.add("ir-row-double");
  } else {
    row.classList.add("ir-row-triple");
  }

  appendButtons(row, buttons);
  return row;
}

function renderTvRemote(commands) {
  const usedIndexes = new Set();
  const slots = {
    powerOn: takeCommand(commands, usedIndexes, SLOT_ALIASES.powerOn),
    power: takeCommand(commands, usedIndexes, SLOT_ALIASES.power),
    powerOff: takeCommand(commands, usedIndexes, SLOT_ALIASES.powerOff),
    menu: takeCommand(commands, usedIndexes, SLOT_ALIASES.menu),
    info: takeCommand(commands, usedIndexes, SLOT_ALIASES.info),
    up: takeCommand(commands, usedIndexes, SLOT_ALIASES.up),
    left: takeCommand(commands, usedIndexes, SLOT_ALIASES.left),
    ok: takeCommand(commands, usedIndexes, SLOT_ALIASES.ok),
    right: takeCommand(commands, usedIndexes, SLOT_ALIASES.right),
    down: takeCommand(commands, usedIndexes, SLOT_ALIASES.down),
    back: takeCommand(commands, usedIndexes, SLOT_ALIASES.back),
    home: takeCommand(commands, usedIndexes, SLOT_ALIASES.home),
    volUp: takeCommand(commands, usedIndexes, SLOT_ALIASES.volUp),
    volDown: takeCommand(commands, usedIndexes, SLOT_ALIASES.volDown),
    chUp: takeCommand(commands, usedIndexes, SLOT_ALIASES.chUp),
    chDown: takeCommand(commands, usedIndexes, SLOT_ALIASES.chDown),
    prev: takeCommand(commands, usedIndexes, SLOT_ALIASES.prev),
    playPause: takeCommand(commands, usedIndexes, SLOT_ALIASES.playPause) ||
      takeCommand(commands, usedIndexes, SLOT_ALIASES.play) ||
      takeCommand(commands, usedIndexes, SLOT_ALIASES.pause),
    stop: takeCommand(commands, usedIndexes, SLOT_ALIASES.stop),
    next: takeCommand(commands, usedIndexes, SLOT_ALIASES.next),
    red: takeCommand(commands, usedIndexes, SLOT_ALIASES.red),
    green: takeCommand(commands, usedIndexes, SLOT_ALIASES.green),
    yellow: takeCommand(commands, usedIndexes, SLOT_ALIASES.yellow),
    blue: takeCommand(commands, usedIndexes, SLOT_ALIASES.blue),
  };

  for (let digit = 1; digit <= 9; digit += 1) {
    slots[`digit${digit}`] = takeCommand(commands, usedIndexes, DIGIT_ALIASES[digit]);
  }
  slots.digit0 = takeCommand(commands, usedIndexes, DIGIT_ALIASES[0]);

  const remote = document.createElement("section");
  remote.className = "ir-remote";
  const stack = document.createElement("div");
  stack.className = "ir-remote-stack";

  const powerButtons = [
    { command: slots.powerOn || slots.power, label: slots.powerOn ? "Power On" : "Power", className: "ir-btn-primary" },
    { command: slots.powerOff, label: "Power Off", className: "ir-btn-primary" },
  ].filter((spec) => !!spec.command);

  if (powerButtons.length > 0) {
    const powerRow = document.createElement("div");
    powerRow.className = powerButtons.length === 1 ? "ir-dynamic-row ir-row-single" : "ir-row-2";
    appendButtons(powerRow, powerButtons.map((spec) => createButton(spec.command, spec.label, spec.className)));
    stack.appendChild(powerRow);
  }

  const digitSpecs = [];
  for (let digit = 1; digit <= 9; digit += 1) {
    if (slots[`digit${digit}`]) {
      digitSpecs.push({ command: slots[`digit${digit}`], label: String(digit) });
    }
  }
  if (slots.digit0) {
    digitSpecs.push({ command: slots.digit0, label: "0", zero: true });
  }

  if (digitSpecs.length > 0) {
    const digitGrid = document.createElement("div");
    digitGrid.className = "ir-number-grid";
    digitSpecs.forEach((spec) => {
      const button = createButton(spec.command, spec.label);
      if (spec.zero) {
        button.classList.add("ir-zero");
      }
      digitGrid.appendChild(button);
    });
    stack.appendChild(digitGrid);
  }

  const navTopRow = createDynamicRow([
    { command: slots.menu, label: "Menu", className: "ir-btn-small" },
    { command: slots.up, label: "Up", className: "ir-btn-small" },
    { command: slots.info, label: "Info", className: "ir-btn-small" },
  ]);
  if (navTopRow) {
    stack.appendChild(navTopRow);
  }

  const navMidRow = createDynamicRow([
    { command: slots.left, label: "Left" },
    { command: slots.ok, label: "OK", className: "ir-btn-ok" },
    { command: slots.right, label: "Right" },
  ]);
  if (navMidRow) {
    stack.appendChild(navMidRow);
  }

  const navBottomRow = createDynamicRow([
    { command: slots.back, label: "Back", className: "ir-btn-small" },
    { command: slots.down, label: "Down", className: "ir-btn-small" },
    { command: slots.home, label: "Home", className: "ir-btn-small" },
  ]);
  if (navBottomRow) {
    stack.appendChild(navBottomRow);
  }

  const pairButtons = [
    { command: slots.volUp, label: "Vol +", className: "ir-btn-accent" },
    { command: slots.chUp, label: "Ch +", className: "ir-btn-accent" },
    { command: slots.volDown, label: "Vol -", className: "ir-btn-accent" },
    { command: slots.chDown, label: "Ch -", className: "ir-btn-accent" },
  ].filter((spec) => !!spec.command);

  if (pairButtons.length > 0) {
    const pairGrid = document.createElement("div");
    pairGrid.className = pairButtons.length === 1 ? "ir-dynamic-row ir-row-single" : "ir-pair-grid";
    appendButtons(pairGrid, pairButtons.map((spec) => createButton(spec.command, spec.label, spec.className)));
    stack.appendChild(pairGrid);
  }

  const mediaButtons = [
    { command: slots.prev, label: "Prev" },
    { command: slots.playPause, label: "Play" },
    { command: slots.stop, label: "Stop" },
    { command: slots.next, label: "Next" },
  ].filter((spec) => !!spec.command);

  if (mediaButtons.length > 0) {
    const mediaGrid = document.createElement("div");
    mediaGrid.className = mediaButtons.length === 1 ? "ir-dynamic-row ir-row-single" :
      mediaButtons.length === 2 ? "ir-dynamic-row ir-row-double" :
      mediaButtons.length === 3 ? "ir-dynamic-row ir-row-triple" : "ir-media-grid";
    appendButtons(mediaGrid, mediaButtons.map((spec) => createButton(spec.command, spec.label)));
    stack.appendChild(mediaGrid);
  }

  const colorButtons = [
    { command: slots.red, label: "R", className: "ir-btn-color-red" },
    { command: slots.green, label: "G", className: "ir-btn-color-green" },
    { command: slots.yellow, label: "Y", className: "ir-btn-color-yellow" },
    { command: slots.blue, label: "B", className: "ir-btn-color-blue" },
  ].filter((spec) => !!spec.command);

  if (colorButtons.length > 0) {
    const colorGrid = document.createElement("div");
    colorGrid.className = colorButtons.length === 1 ? "ir-dynamic-row ir-row-single" :
      colorButtons.length === 2 ? "ir-dynamic-row ir-row-double" :
      colorButtons.length === 3 ? "ir-dynamic-row ir-row-triple" : "ir-color-grid";
    appendButtons(colorGrid, colorButtons.map((spec) => createButton(spec.command, spec.label, spec.className)));
    stack.appendChild(colorGrid);
  }

  remote.appendChild(stack);

  const extras = commands.filter((command) => !usedIndexes.has(command.index));
  return { remote, extras };
}

function genericSectionForCommand(command) {
  if (Object.values(DIGIT_ALIASES).some((aliases) => matchesAlias(command, aliases))) {
    return "Numbers";
  }
  if ([SLOT_ALIASES.powerOn, SLOT_ALIASES.powerOff, SLOT_ALIASES.power].some((aliases) => matchesAlias(command, aliases))) {
    return "Power";
  }
  if ([SLOT_ALIASES.up, SLOT_ALIASES.down, SLOT_ALIASES.left, SLOT_ALIASES.right, SLOT_ALIASES.ok,
    SLOT_ALIASES.menu, SLOT_ALIASES.info, SLOT_ALIASES.back, SLOT_ALIASES.home].some((aliases) => matchesAlias(command, aliases))) {
    return "Navigation";
  }
  if ([SLOT_ALIASES.volUp, SLOT_ALIASES.volDown, SLOT_ALIASES.chUp, SLOT_ALIASES.chDown, SLOT_ALIASES.mute]
    .some((aliases) => matchesAlias(command, aliases))) {
    return "Volume";
  }
  if ([SLOT_ALIASES.prev, SLOT_ALIASES.playPause, SLOT_ALIASES.play, SLOT_ALIASES.pause, SLOT_ALIASES.stop, SLOT_ALIASES.next]
    .some((aliases) => matchesAlias(command, aliases))) {
    return "Media";
  }
  if ([SLOT_ALIASES.source, SLOT_ALIASES.hdmi1, SLOT_ALIASES.hdmi2, SLOT_ALIASES.hdmi3, SLOT_ALIASES.av, SLOT_ALIASES.usb]
    .some((aliases) => matchesAlias(command, aliases))) {
    return "Inputs";
  }
  if ([SLOT_ALIASES.picture, SLOT_ALIASES.sound, SLOT_ALIASES.sleep, SLOT_ALIASES.eco,
    SLOT_ALIASES.game, SLOT_ALIASES.cool, SLOT_ALIASES.heat, SLOT_ALIASES.fan, SLOT_ALIASES.timer, SLOT_ALIASES.mode]
    .some((aliases) => matchesAlias(command, aliases))) {
    return "Modes";
  }
  return "Other Commands";
}

function labelForCommand(command) {
  if (!command) {
    return "Command";
  }

  if (matchesAlias(command, SLOT_ALIASES.powerOn)) return "Power On";
  if (matchesAlias(command, SLOT_ALIASES.powerOff)) return "Power Off";
  if (matchesAlias(command, SLOT_ALIASES.power)) return "Power";
  if (matchesAlias(command, SLOT_ALIASES.menu)) return "Menu";
  if (matchesAlias(command, SLOT_ALIASES.info)) return "Info";
  if (matchesAlias(command, SLOT_ALIASES.up)) return "Up";
  if (matchesAlias(command, SLOT_ALIASES.down)) return "Down";
  if (matchesAlias(command, SLOT_ALIASES.left)) return "Left";
  if (matchesAlias(command, SLOT_ALIASES.right)) return "Right";
  if (matchesAlias(command, SLOT_ALIASES.ok)) return "OK";
  if (matchesAlias(command, SLOT_ALIASES.back)) return "Back";
  if (matchesAlias(command, SLOT_ALIASES.home)) return "Home";
  if (matchesAlias(command, SLOT_ALIASES.volUp)) return "Vol +";
  if (matchesAlias(command, SLOT_ALIASES.volDown)) return "Vol -";
  if (matchesAlias(command, SLOT_ALIASES.chUp)) return "Ch +";
  if (matchesAlias(command, SLOT_ALIASES.chDown)) return "Ch -";
  if (matchesAlias(command, SLOT_ALIASES.mute)) return "Mute";
  if (matchesAlias(command, SLOT_ALIASES.prev)) return "Prev";
  if (matchesAlias(command, SLOT_ALIASES.playPause)) return "Play";
  if (matchesAlias(command, SLOT_ALIASES.play)) return "Play";
  if (matchesAlias(command, SLOT_ALIASES.pause)) return "Pause";
  if (matchesAlias(command, SLOT_ALIASES.stop)) return "Stop";
  if (matchesAlias(command, SLOT_ALIASES.next)) return "Next";
  if (matchesAlias(command, SLOT_ALIASES.red)) return "Red";
  if (matchesAlias(command, SLOT_ALIASES.green)) return "Green";
  if (matchesAlias(command, SLOT_ALIASES.yellow)) return "Yellow";
  if (matchesAlias(command, SLOT_ALIASES.blue)) return "Blue";

  const digitEntry = Object.entries(DIGIT_ALIASES).find(([, aliases]) => matchesAlias(command, aliases));
  if (digitEntry) {
    return digitEntry[0];
  }

  return command.name || `Command ${command.index}`;
}

function commandSortKey(command) {
  const digitEntry = Object.entries(DIGIT_ALIASES).find(([, aliases]) => matchesAlias(command, aliases));
  if (digitEntry) {
    return `0-${digitEntry[0].padStart(2, "0")}`;
  }

  return `1-${labelForCommand(command).toLowerCase()}`;
}

function renderGenericRemote(commands) {
  const grouped = new Map();
  GENERIC_SECTION_ORDER.forEach((section) => grouped.set(section, []));

  commands.forEach((command) => {
    grouped.get(genericSectionForCommand(command)).push(command);
  });

  const fragment = document.createDocumentFragment();

  GENERIC_SECTION_ORDER.forEach((sectionName) => {
    const items = (grouped.get(sectionName) || []).slice().sort((a, b) =>
      commandSortKey(a).localeCompare(commandSortKey(b)));
    if (items.length === 0) {
      return;
    }

    const section = document.createElement("section");
    section.className = "ir-section";

    const title = document.createElement("h2");
    title.className = "ir-section-title";
    title.textContent = sectionName;
    section.appendChild(title);

    const grid = document.createElement("div");
    grid.className = sectionName === "Numbers" ? "ir-number-grid" :
      ((sectionName === "Power") || (sectionName === "Volume")) ? "ir-pair-grid" : "ir-chip-grid";

    items.forEach((command) => {
      const button = createButton(command, labelForCommand(command));
      if ((sectionName === "Power") && (items.length === 1)) {
        button.classList.add("ir-span-2");
      }
      grid.appendChild(button);
    });

    section.appendChild(grid);
    fragment.appendChild(section);
  });

  return fragment;
}

function updateButtons(disabled) {
  irContentNode.querySelectorAll("button").forEach((button) => {
    button.disabled = disabled;
  });
}

async function closeSession(useBeacon = false) {
  if (!irState.token) {
    return;
  }

  const token = irState.token;
  irState.token = "";

  const url = `/api/ir/close?token=${encodeURIComponent(token)}`;
  if (useBeacon && navigator.sendBeacon) {
    try {
      navigator.sendBeacon(url, "");
      return;
    } catch (error) {
      // Ignore unload-time close failures.
    }
  }

  try {
    await fetch(url, { method: "POST", keepalive: useBeacon });
  } catch (error) {
    // Best effort cleanup.
  }
}

async function sendCommand(command, label, buttonNode) {
  if (!command || !irState.token || irState.sending) {
    return;
  }

  irState.sending = true;
  updateButtons(true);
  statusMessage(`Sending ${label}...`);
  buttonNode?.classList.add("ir-btn-pressed");

  try {
    const response = await fetch(`/api/ir/send?token=${encodeURIComponent(irState.token)}&index=${encodeURIComponent(command.index)}`,
      { method: "POST" });
    const text = await response.text();
    let payload = null;

    try {
      payload = JSON.parse(text);
    } catch (error) {
      payload = null;
    }

    if (!response.ok) {
      throw new Error((payload && payload.error) || text || `HTTP ${response.status}`);
    }

    statusMessage(`Sent: ${label}`);
  } catch (error) {
    statusMessage(`IR send failed: ${error.message}`, true);
  } finally {
    irState.sending = false;
    updateButtons(false);
    window.setTimeout(() => {
      buttonNode?.classList.remove("ir-btn-pressed");
    }, 140);
  }
}

function renderRemote(commands) {
  irContentNode.innerHTML = "";
  const useTvLayout = detectTvLayout(commands);

  irSubtitleNode.textContent = "";
  layoutBadge(useTvLayout ? "TV layout" : "Generic layout");

  if (useTvLayout) {
    const tvLayout = renderTvRemote(commands);
    irContentNode.appendChild(tvLayout.remote);

    if (tvLayout.extras.length > 0) {
      const extraSection = document.createElement("section");
      extraSection.className = "ir-section";

      const title = document.createElement("h2");
      title.className = "ir-section-title";
      title.textContent = "Other Commands";
      extraSection.appendChild(title);

      const grid = document.createElement("div");
      grid.className = "ir-chip-grid";
      tvLayout.extras.forEach((command) => {
        grid.appendChild(createButton(command, command.name || `Cmd ${command.index}`, "ir-btn-muted"));
      });
      extraSection.appendChild(grid);
      irContentNode.appendChild(extraSection);
    }
  } else {
    irContentNode.appendChild(renderGenericRemote(commands));
  }

  statusMessage(`IR ready: ${commands.length} command(s) loaded.`);
}

async function loadRemote() {
  const fileName = getFileName(requestedPath);
  irPathNode.textContent = fileName || absolutePath;
  irPathNode.title = absolutePath;

  if (!requestedPath) {
    statusMessage("Missing IR file path.", true);
    irContentNode.innerHTML = "<div class=\"ir-error-card\">No .ir file was provided.</div>";
    return;
  }

  try {
    const response = await fetch(`/api/ir/open?path=${encodeURIComponent(requestedPath)}`, { method: "POST" });
    const text = await response.text();
    let payload = null;

    try {
      payload = JSON.parse(text);
    } catch (error) {
      payload = null;
    }

    if (!response.ok || !payload || !payload.ok) {
      throw new Error((payload && payload.error) || text || `HTTP ${response.status}`);
    }

    irState.token = String(payload.token || "");
    irState.commands = Array.isArray(payload.commands) ? payload.commands.map(commandMeta) : [];

    if (irState.commands.length === 0) {
      statusMessage("No parsed IR commands were found in this file.", true);
      irContentNode.innerHTML = "<div class=\"ir-error-card\">The file loaded, but there are no usable commands.</div>";
      return;
    }

    renderRemote(irState.commands);
  } catch (error) {
    statusMessage(`IR unavailable: ${error.message}`, true);
    irContentNode.innerHTML = "<div class=\"ir-error-card\">Failed to open the IR remote for this file.</div>";
  }
}

window.addEventListener("pagehide", () => {
  void closeSession(true);
});

window.addEventListener("beforeunload", () => {
  void closeSession(true);
});

void loadRemote();
