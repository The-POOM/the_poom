let socket = null;
let reconnectTimer = null;
let hasConnected = false;
let wsEnabled = true;
let pageUnloading = false;
let isUploading = false;
let toneEnabled = false;
let midiEnabled = false;
const FILE_ROOT_PATH = "/sdcard";
let currentDir = "";

const popup = document.getElementById("status-popup");
const output = document.getElementById("output");
const commandInput = document.getElementById("command");
const historyEl = document.getElementById("history");
const filesBtn = document.getElementById("files-btn");
const toneBtn = document.getElementById("tone-btn");
const midiBtn = document.getElementById("midi-btn");
const filePanel = document.getElementById("file-panel");
const filePanelOverlay = document.getElementById("file-panel-overlay");
const filePanelClose = document.getElementById("file-panel-close");
const fileUpBtn = document.getElementById("file-up-btn");
const fileDropzone = document.getElementById("file-dropzone");
const fileInput = document.getElementById("file-input");
const fileList = document.getElementById("file-list");
const fileSpace = document.getElementById("file-space");
const fileCurrentPath = document.getElementById("file-current-path");
const fileViewerOverlay = document.getElementById("file-viewer-overlay");
const fileViewer = document.getElementById("file-viewer");
const fileViewerClose = document.getElementById("file-viewer-close");
const fileViewerTitle = document.getElementById("file-viewer-title");
const fileViewerPath = document.getElementById("file-viewer-path");
const fileViewerContent = document.getElementById("file-viewer-content");

let viewerOpen = false;

function bootBanner() {
  const banner = [
    "██████╗  ██████╗  ██████╗ ███╗   ███╗",
    "██╔══██╗██╔═══██╗██╔═══██╗████╗ ████║",
    "██████╔╝██║   ██║██║   ██║██╔████╔██║",
    "██╔═══╝ ██║   ██║██║   ██║██║╚██╔╝██║",
    "██║     ╚██████╔╝╚██████╔╝██║ ╚═╝ ██║",
    "╚═╝      ╚═════╝  ╚═════╝ ╚═╝     ╚═╝",
    "",
    "PENTEST · PLAY · CREATE",
    "",
  ].join("\n");

  output.value = banner;
  output.scrollTop = 0;
  output.scrollLeft = 0;
}

function appendOutput(text) {
  output.value += text;
  output.scrollTop = output.scrollHeight;
}

function connectSocket() {
  if (!wsEnabled || pageUnloading) {
    return;
  }

  socket = new WebSocket(`ws://${window.location.host}/ws`);

  socket.onopen = () => {
    hasConnected = true;
    popup.classList.add("hidden");
  };

  socket.onmessage = (event) => {
    appendOutput(`${event.data}`);
  };

  socket.onclose = () => {
    socket = null;
    if (!wsEnabled || pageUnloading) {
      return;
    }
    popup.classList.remove("hidden");
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
    }
    reconnectTimer = setTimeout(connectSocket, 1000);
  };

  socket.onerror = () => {
    if (wsEnabled && !pageUnloading && !hasConnected) {
      popup.classList.remove("hidden");
    }
  };
}

function shutdownPageResources() {
  pageUnloading = true;

  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }

  if (socket) {
    try {
      socket.onclose = null;
      socket.close();
    } catch (error) {
      // Ignore close errors during unload.
    }
    socket = null;
  }
}

async function detectTransport() {
  try {
    const response = await fetch("/capabilities", { cache: "no-store" });
    if (!response.ok) {
      wsEnabled = false;
      toneEnabled = false;
      midiEnabled = false;
      toneBtn?.classList.add("hidden");
      midiBtn?.classList.add("hidden");
      popup.classList.add("hidden");
      return;
    }
    const capabilities = await response.json();
    wsEnabled = !!capabilities.ws;
    toneEnabled = !!capabilities.tone;
    midiEnabled = !!capabilities.midi;
    toneBtn?.classList.toggle("hidden", !toneEnabled);
    midiBtn?.classList.toggle("hidden", !midiEnabled);
    if (!wsEnabled) {
      popup.classList.add("hidden");
    }
  } catch (error) {
    wsEnabled = false;
    toneEnabled = false;
    midiEnabled = false;
    toneBtn?.classList.add("hidden");
    midiBtn?.classList.add("hidden");
    popup.classList.add("hidden");
  }
}

function addToHistory(command) {
  if (!command || command.length < 2) {
    return;
  }
  const item = document.createElement("button");
  item.textContent = command.length > 18 ? `${command.slice(0, 15)}...` : command;
  item.title = command;
  item.onclick = () => {
    commandInput.value = command;
    commandInput.focus();
  };
  historyEl.prepend(item);
}

function sendCommand() {
  const command = commandInput.value.trim();
  if (!command) {
    return;
  }
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(`${command}\n`);
  } else {
    fetch("/command", {
      method: "POST",
      headers: { "Content-Type": "text/plain" },
      body: `${command}\n`,
    }).then((response) => response.text())
      .then((text) => {
        if (text && text.length > 0) {
          appendOutput(text.endsWith("\n") ? text : `${text}\n`);
        }
      })
      .catch(() => {
        popup.classList.remove("hidden");
      });
  }
  appendOutput(`> ${command}\n`);
  addToHistory(command);
  commandInput.value = "";
}

function formatBytes(bytes) {
  const value = Number(bytes);
  if (!Number.isFinite(value) || value < 1024) {
    return `${Math.max(0, value || 0)} B`;
  }
  if (value < 1024 * 1024) {
    return `${(value / 1024).toFixed(1)} KB`;
  }
  return `${(value / (1024 * 1024)).toFixed(1)} MB`;
}

function normalizeDirPath(dirPath) {
  if (typeof dirPath !== "string") {
    return "";
  }
  const compact = dirPath.trim();
  if (!compact || compact === "/") {
    return "";
  }
  return compact.split("/").filter(Boolean).join("/");
}

function buildRelativePath(name) {
  const cleanName = String(name || "").trim();
  if (!cleanName) {
    return normalizeDirPath(currentDir);
  }
  return currentDir ? `${currentDir}/${cleanName}` : cleanName;
}

function buildAbsolutePath(relativePath) {
  const cleanPath = normalizeDirPath(relativePath);
  return cleanPath ? `${FILE_ROOT_PATH}/${cleanPath}` : FILE_ROOT_PATH;
}

function getFileNameFromRelativePath(relativePath) {
  const cleanPath = normalizeDirPath(relativePath);
  if (!cleanPath) {
    return "download.bin";
  }
  const segments = cleanPath.split("/").filter(Boolean);
  return segments.length > 0 ? segments[segments.length - 1] : "download.bin";
}

async function downloadFile(relativePath, absolutePath) {
  const response = await fetch(`/files/download?path=${encodeURIComponent(relativePath)}`);
  if (!response.ok) {
    const text = await response.text();
    throw new Error(text || `HTTP ${response.status}`);
  }

  const blob = await response.blob();
  const link = document.createElement("a");
  const blobUrl = URL.createObjectURL(blob);

  link.href = blobUrl;
  link.download = getFileNameFromRelativePath(relativePath);
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(blobUrl);

  appendOutput(`[FILES] Downloaded: ${absolutePath}\n`);
}

function isViewableTextFile(name) {
  const lower = String(name || "").toLowerCase();
  return lower.endsWith(".txt") || lower.endsWith(".csv") || lower.endsWith(".ir") || lower.endsWith(".nfc") ||
    lower.endsWith(".tone") || lower.endsWith(".json");
}

function isIrFile(name) {
  return String(name || "").toLowerCase().endsWith(".ir");
}

function openFileViewer(title, absolutePath) {
  viewerOpen = true;
  fileViewerTitle.textContent = title || "View file";
  fileViewerPath.textContent = absolutePath ? `Path: ${absolutePath}` : "";
  fileViewerContent.value = "Loading...\n";
  fileViewerOverlay.classList.remove("hidden");
  fileViewer.classList.remove("hidden");
}

function closeFileViewer() {
  viewerOpen = false;
  fileViewerOverlay.classList.add("hidden");
  fileViewer.classList.add("hidden");
  fileViewerTitle.textContent = "View file";
  fileViewerPath.textContent = "";
  fileViewerContent.value = "";
}

function openToneGenerator() {
  if (!toneEnabled) {
    return;
  }
  window.open("/tone", "_blank", "noopener,noreferrer");
}

function openIrRemote(relativePath, absolutePath) {
  window.location.assign(`/ir?path=${encodeURIComponent(relativePath)}`);
  appendOutput(`[IR] Opened remote page: ${absolutePath}\n`);
}

async function viewFile(relativePath, absolutePath, name) {
  openFileViewer(`View: ${name}`, absolutePath);

  try {
    const response = await fetch(`/files/view?path=${encodeURIComponent(relativePath)}`, { cache: "no-store" });
    if (!response.ok) {
      const text = await response.text();
      throw new Error(text || `HTTP ${response.status}`);
    }

    const text = await response.text();
    fileViewerContent.value = text || "";
    fileViewerContent.scrollTop = 0;
    fileViewerContent.scrollLeft = 0;
    appendOutput(`[FILES] Viewed: ${absolutePath}\n`);
  } catch (error) {
    fileViewerContent.value = `View failed: ${error.message}\n`;
  }
}

toneBtn?.addEventListener("click", openToneGenerator);
midiBtn?.addEventListener("click", () => {
  if (!midiEnabled) {
    return;
  }
  window.open("/midi", "_blank", "noopener,noreferrer");
});

function updateCurrentPathUI() {
  fileCurrentPath.textContent = `Path: ${buildAbsolutePath(currentDir)}`;
  fileUpBtn.disabled = (currentDir === "");
}

function setDropzoneDisabled(disabled) {
  if (disabled) {
    fileDropzone.classList.add("disabled");
    fileInput.disabled = true;
    return;
  }
  fileDropzone.classList.remove("disabled");
  fileInput.disabled = false;
}

function openFilePanel() {
  currentDir = "";
  updateCurrentPathUI();
  filePanelOverlay.classList.remove("hidden");
  filePanel.classList.remove("hidden");
  refreshFileList();
}

function closeFilePanel() {
  if (isUploading) {
    return;
  }
  if (viewerOpen) {
    closeFileViewer();
  }
  filePanelOverlay.classList.add("hidden");
  filePanel.classList.add("hidden");
}

function renderFileList(entries) {
  fileList.innerHTML = "";

  if (!Array.isArray(entries) || entries.length === 0) {
    fileList.innerHTML = "<div class=\"file-empty\">No files found.</div>";
    return;
  }

  entries
    .slice()
    .sort((a, b) => {
      const aDir = !!a.is_dir;
      const bDir = !!b.is_dir;
      if (aDir !== bDir) {
        return aDir ? -1 : 1;
      }
      return String(a.name || "").localeCompare(String(b.name || ""));
    })
    .forEach((entry) => {
      const name = String(entry.name || "");
      const relativePath = normalizeDirPath(String(entry.path || buildRelativePath(name)));
      const absolutePath = buildAbsolutePath(relativePath);
      const isDir = !!entry.is_dir;
      const size = Number(entry.size || 0);

      const item = document.createElement("article");
      item.className = "file-item";

      const top = document.createElement("div");
      top.className = "file-item-top";

      const icon = document.createElement("div");
      icon.className = "file-icon";
      icon.textContent = isDir ? "DIR" : "FILE";

      const nameNode = document.createElement("div");
      nameNode.className = "file-name";
      nameNode.title = name;
      nameNode.textContent = name;

      const metaNode = document.createElement("div");
      metaNode.className = "file-meta";
      metaNode.textContent = isDir ? "Folder" : formatBytes(size);

      const pathNode = document.createElement("div");
      pathNode.className = "file-path";
      pathNode.title = absolutePath;
      pathNode.textContent = absolutePath;

      top.appendChild(icon);
      top.appendChild(nameNode);
      top.appendChild(metaNode);
      top.appendChild(pathNode);

      const actions = document.createElement("div");
      actions.className = "file-actions";

      if (isDir) {
        const openBtn = document.createElement("button");
        openBtn.type = "button";
        openBtn.className = "file-action-btn";
        openBtn.textContent = "Open";
        openBtn.addEventListener("click", () => {
          currentDir = relativePath;
          updateCurrentPathUI();
          refreshFileList();
        });
        actions.appendChild(openBtn);
      } else {
        if (isIrFile(name)) {
          const remoteBtn = document.createElement("button");
          remoteBtn.type = "button";
          remoteBtn.className = "file-action-btn";
          remoteBtn.textContent = "Remote";
          remoteBtn.addEventListener("click", () => {
            openIrRemote(relativePath, absolutePath);
          });
          actions.appendChild(remoteBtn);
        } else if (isViewableTextFile(name)) {
          const viewBtn = document.createElement("button");
          viewBtn.type = "button";
          viewBtn.className = "file-action-btn";
          viewBtn.textContent = "View";
          viewBtn.addEventListener("click", async () => {
            appendOutput(`[FILES] View request: ${absolutePath}\n`);
            await viewFile(relativePath, absolutePath, name);
          });
          actions.appendChild(viewBtn);
        }

        const downloadBtn = document.createElement("button");
        downloadBtn.type = "button";
        downloadBtn.className = "file-action-btn";
        downloadBtn.textContent = "Dwnld";
        downloadBtn.addEventListener("click", async () => {
          appendOutput(`[FILES] Download request: ${absolutePath}\n`);
          try {
            await downloadFile(relativePath, absolutePath);
          } catch (error) {
            window.alert(`Download failed: ${error.message}`);
          }
        });

        const deleteBtn = document.createElement("button");
        deleteBtn.type = "button";
        deleteBtn.className = "file-action-btn danger";
        deleteBtn.textContent = "Del";
        deleteBtn.addEventListener("click", async () => {
          const confirmDelete = window.confirm(`Delete "${name}"?`);
          if (!confirmDelete) {
            return;
          }
          const response = await fetch(`/files/delete?path=${encodeURIComponent(relativePath)}`, {
            method: "DELETE",
          });
          if (!response.ok) {
            const text = await response.text();
            window.alert(`Delete failed: ${text || response.status}`);
            return;
          }
          appendOutput(`[FILES] Deleted: ${absolutePath}\n`);
          refreshFileList();
        });

        actions.appendChild(downloadBtn);
        actions.appendChild(deleteBtn);
      }

      item.appendChild(top);
      item.appendChild(actions);
      fileList.appendChild(item);
    });
}

async function refreshFileList() {
  fileList.innerHTML = "<div class=\"file-empty\">Loading...</div>";
  fileSpace.textContent = "Loading...";

  try {
    const query = currentDir ? `?dir=${encodeURIComponent(currentDir)}` : "";
    const response = await fetch(`/files/list${query}`, { cache: "no-store" });
    if (!response.ok) {
      const text = await response.text();
      fileList.innerHTML = `<div class="file-empty">${text || "Failed to load file list."}</div>`;
      fileSpace.textContent = "SD card unavailable";
      return;
    }

    const payload = await response.json();
    currentDir = normalizeDirPath(String(payload.dir || ""));
    updateCurrentPathUI();
    const entries = Array.isArray(payload.entries) ? payload.entries : [];
    const totalBytes = entries.reduce((sum, item) => sum + Number(item.size || 0), 0);
    const dirCount = entries.filter((item) => !!item.is_dir).length;
    const fileCount = entries.length - dirCount;

    fileSpace.textContent = `Path: ${buildAbsolutePath(currentDir)} | ${dirCount} dir(s), ${fileCount} file(s) - ${formatBytes(totalBytes)}`;
    renderFileList(entries);
  } catch (error) {
    fileList.innerHTML = "<div class=\"file-empty\">Failed to load files.</div>";
    fileSpace.textContent = "SD card unavailable";
  }
}

async function uploadFile(file) {
  if (!file) {
    return;
  }

  isUploading = true;
  setDropzoneDisabled(true);
  {
    const uploadRelativePath = buildRelativePath(file.name);
    fileSpace.textContent = `Uploading ${file.name} to ${buildAbsolutePath(uploadRelativePath)}...`;
  }

  try {
    const uploadRelativePath = buildRelativePath(file.name);
    const response = await fetch(`/files/upload?path=${encodeURIComponent(uploadRelativePath)}`, {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: file,
    });

    if (!response.ok) {
      const text = await response.text();
      window.alert(`Upload failed: ${text || response.status}`);
      return;
    }

    appendOutput(`[FILES] Uploaded to: ${buildAbsolutePath(uploadRelativePath)}\n`);
    await refreshFileList();
  } catch (error) {
    window.alert("Upload failed: network error");
  } finally {
    isUploading = false;
    setDropzoneDisabled(false);
  }
}

function bindFilePanelEvents() {
  filesBtn.addEventListener("click", openFilePanel);
  filePanelClose.addEventListener("click", closeFilePanel);
  filePanelOverlay.addEventListener("click", closeFilePanel);
  fileViewerClose.addEventListener("click", closeFileViewer);
  fileViewerOverlay.addEventListener("click", closeFileViewer);
  fileUpBtn.addEventListener("click", () => {
    if (currentDir === "") {
      return;
    }
    const segments = currentDir.split("/").filter(Boolean);
    segments.pop();
    currentDir = segments.join("/");
    updateCurrentPathUI();
    refreshFileList();
  });

  fileInput.addEventListener("change", async (event) => {
    const file = event.target.files && event.target.files[0];
    if (!file) {
      return;
    }
    await uploadFile(file);
    fileInput.value = "";
  });

  fileDropzone.addEventListener("dragover", (event) => {
    if (isUploading) {
      return;
    }
    event.preventDefault();
    fileDropzone.classList.add("drag");
  });

  fileDropzone.addEventListener("dragleave", (event) => {
    event.preventDefault();
    fileDropzone.classList.remove("drag");
  });

  fileDropzone.addEventListener("drop", async (event) => {
    if (isUploading) {
      return;
    }
    event.preventDefault();
    fileDropzone.classList.remove("drag");
    const files = event.dataTransfer ? event.dataTransfer.files : null;
    if (!files || files.length === 0) {
      return;
    }
    await uploadFile(files[0]);
  });
}

document.getElementById("send-btn").addEventListener("click", sendCommand);
commandInput.addEventListener("keydown", (event) => {
  if (event.key === "Enter") {
    event.preventDefault();
    sendCommand();
  }
});

bindFilePanelEvents();
updateCurrentPathUI();
bootBanner();
window.addEventListener("pagehide", () => {
  shutdownPageResources();
});
window.addEventListener("beforeunload", shutdownPageResources);
detectTransport().then(() => {
  if (wsEnabled) {
    connectSocket();
  }
});
