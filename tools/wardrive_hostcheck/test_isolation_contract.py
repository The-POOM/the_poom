"""The integration must not silently change stock boot/config/radio helpers."""
from pathlib import Path
import re
import subprocess

BASE = "3533ee027efcf06a610265f3c4b4be8c73548fb5"
protected = ["sdkconfig.defaults", "sdkconfig.ci", "sdkconfig.esp32c5.ram_optimization",
             "sdkconfig.openthread", "sdkconfig.zigbee", "partitions.csv",
             "main", "modules/poom_ble_scan", "modules/poom_ble_gatt_client",
             "modules/poom_wifi_ctrl"]
subprocess.run(["git", "diff", "--exit-code", BASE, "--", *protected], check=True)
sources = [p for p in Path("modules/poom_wardrive").glob("*.c") if p.name != "wardrive_clock_scope.c"]
sources += list(Path("applications/poom_app_pack/src").glob("wardrive_ui_*.c"))
sources += [Path("applications/poom_app_pack/src/menu_wardrive.c")]
for p in sources:
    text = p.read_text()
    assert not re.search(r"\b(?:poom_ble_scan_\w+|poom_ble_gatt_client_\w+|poom_wifi_ctrl_\w+|poom_wifi_scanner_\w+|settimeofday|poom_game_store_sync_time)\s*\(", text), p
    assert "__attribute__((constructor" not in text, p
print("Isolation contract: stock boot/config/partitions/radio helpers unchanged; no shared radio calls or unscoped clock mutation")

scope = Path("modules/poom_wardrive/wardrive_clock_scope.c").read_text()
assert "esp_sntp_enabled()" in scope and "saved_wall_us + elapsed" in scope
jobs = Path("applications/poom_app_pack/src/wardrive_ui_jobs.c").read_text()
assert jobs.index("wd_clock_scope_end(&j->clock_scope)") < jobs.index("atomic_store(&j->done,true)")
assert ".crt_bundle_attach=esp_crt_bundle_attach" in Path("modules/poom_wardrive/wardrive_upload.c").read_text()
print("TLS clock scope: restoration precedes job completion; platform bundle verification remains enabled")

assert "heap_caps_register_failed_alloc_callback(" not in Path("modules/poom_wardrive/wardrive_upload.c").read_text()
