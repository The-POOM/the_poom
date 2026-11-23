# Passive Wi-Fi host checks

Run from PowerShell with a native GCC-compatible compiler:

```powershell
./tests/host/run_passive_wifi_tests.ps1 -Compiler gcc
```

The runner accepts an absolute compiler path and writes test executables under
the ignored `build/host-passive-wifi` directory. It does not open a serial port
or use a radio.

- `test_sae_filter.c` exercises the production SAE Authentication-frame filter
  with truncated headers/bodies, incorrect frame types and algorithm values,
  protected/DS flags, fragments, HT Control, commit, confirm and status replies.
  The helper expects a frame length excluding FCS; the ESP-IDF capture path
  subtracts the FCS before calling it.
- `test_wifi_scan_cache.c` compiles the production scanner with minimal ESP-IDF
  mocks. It checks that initialization, scan and result-read failures cannot
  leave a previous target list selectable, and checks recovery and empty scans.

These checks validate parsing and cache behavior. They do not validate hardware
reception, generated PCAP files, or radio interoperability.
