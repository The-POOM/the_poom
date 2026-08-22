# streamlit_monitor

This tool gives you a live dashboard to observe if a target HTTP server becomes slow or unavailable while your ESP32 load test is running.

## Purpose

- Realtime target checks (`GET` request loop)
- Latency chart (ms)
- UP/DOWN status history
- Availability percentage
- Configurable timeout, probe interval, and history size

## Prerequisites

- Python 3.9+
- A target HTTP server

Example target server:

```bash
python3 -m http.server 8000
```

## Install

From repository root:

```bash
python3 -m pip install -r applications/poom_http_load_test/tools/streamlit_monitor/requirements.txt
```

## Run

```bash
streamlit run applications/poom_http_load_test/tools/streamlit_monitor/monitor.py
```

## Usage

1. Open the Streamlit URL shown in terminal (usually `http://localhost:8501`).
2. Set target URL in sidebar, for example:
   - `http://192.168.3.89:8000/`
3. Click `Start`.
4. Run your ESP32 load test and watch:
   - `State` (`UP`/`DOWN`)
   - `Availability`
   - `Latency`
   - Recent errors in the table
5. Click `Stop` to pause checks or `Clear` to reset history.

## POOM CLI Setup

Use POOM CLI to configure Wi-Fi and load-test target before starting the test.

```bash
# 1) Save Wi-Fi credentials used by POOM STA
cfg-wifi-set <ssid> <password>

# 2) Save target profile
cfg-load-target-set <host> <port> <path> [workers] [--scheme http|https]
# Example:
cfg-load-target-set 192.168.3.89 8000 / 8

# 3) Verify current saved config
cfg-load-get

# 4) Start/stop load test
cfg-load-start
cfg-load-stop
```

## Recommended Workflow With POOM Load Test

1. Start local target:

```bash
python3 -m http.server 8000
```

2. Configure target in CLI:

```bash
cfg-wifi-set MyWiFi MyPassword
cfg-load-target-set 192.168.3.89 8000 /
# For HTTPS target:
cfg-load-target-set example.com 443 / --scheme https
```

3. Start monitor:

```bash
streamlit run applications/poom_http_load_test/tools/streamlit_monitor/monitor.py
```

4. Start load from device and watch if/when the target degrades.
