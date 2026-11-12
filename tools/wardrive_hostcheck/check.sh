#!/bin/sh
# Synthetic offline tests. No WiGLE requests or credentials. Run at repo root.
set -eu
TASK_TMP=$(mktemp -d "${TMPDIR:-/tmp}/poom-wardrive.XXXXXX")
trap 'rm -rf "$TASK_TMP"' EXIT INT TERM
CC=${CC:-cc}
SAN=${SAN:--fsanitize=address,undefined}
INC="-Imodules/poom_gps/include -Imodules/poom_wardrive/include"
COMMON="-std=gnu11 -O1 -g -Wall -Wextra -Werror $SAN"
$CC $COMMON $INC -DCONFIG_IDF_TARGET_ESP32C5=1 -Iapplications/poom_app_pack/include -Iapplications/poom_app_pack/src -Imodules/poom_secrets_store/include -Itools/wardrive_hostcheck/wifi_stubs -Itools/wardrive_hostcheck/radio_stubs -Itools/wardrive_hostcheck/stubs tools/wardrive_hostcheck/test_wifi_picker.c applications/poom_app_pack/src/wardrive_ui_wifi.c -o "$TASK_TMP/wifi-picker"
"$TASK_TMP/wifi-picker"
$CC $COMMON $INC modules/poom_wardrive/test/test_timezone.c modules/poom_wardrive/wardrive_timezone.c modules/poom_wardrive/wardrive_format.c -lm -o "$TASK_TMP/timezone"
"$TASK_TMP/timezone"
python3 tools/wardrive_hostcheck/test_timezone_oracle.py "$TASK_TMP/timezone"
$CC $COMMON $INC modules/poom_gps/test/test_gps.c modules/poom_gps/poom_nmea.c modules/poom_gps/poom_gps_service.c -lm -o "$TASK_TMP/gps"
"$TASK_TMP/gps"
$CC $COMMON $INC modules/poom_wardrive/test/test_distance.c modules/poom_wardrive/wardrive_distance.c modules/poom_gps/poom_nmea.c -lm -o "$TASK_TMP/distance"
"$TASK_TMP/distance"
$CC $COMMON $INC -Idrivers/i2c/include modules/poom_gps/test/test_xa1110.c modules/poom_gps/poom_nmea.c modules/poom_gps/poom_gps_service.c modules/poom_gps/poom_gps_xa1110.c -lm -o "$TASK_TMP/xa1110"
"$TASK_TMP/xa1110"
$CC $COMMON $INC modules/poom_wardrive/test/test_storage.c modules/poom_wardrive/wardrive_timezone.c modules/poom_wardrive/wardrive_format.c modules/poom_wardrive/wardrive_storage.c modules/poom_wardrive/wardrive_clock.c modules/poom_wardrive/wardrive_export.c modules/poom_wardrive/wardrive_archive.c modules/poom_wardrive/wardrive_files.c tools/wardrive_hostcheck/fat_rename.c -Wl,--wrap=rename -lm -o "$TASK_TMP/storage"
mkdir "$TASK_TMP/data"
"$TASK_TMP/storage" "$TASK_TMP/data"
python3 - "$TASK_TMP/data/sessions/synthetic" <<'PY'
import csv,sys,tarfile,json
from pathlib import Path
p=Path(sys.argv[1])
with (p/'wigle.csv').open(newline='') as f:
    rows=list(csv.reader(f))
assert rows[0][0]=='WigleWifi-1.6'
assert len(rows[1])==14
assert all(len(r)==14 for r in rows[2:])
assert rows[2][1]=='A,"B\nC'
assert rows[2][5]=='5180' and rows[2][10]==''
assert rows[5][-1]=='BLE' and rows[5][4]=='0' and rows[5][5]=='' and rows[5][12]=='76'
assert not list(p.glob('*.tmp'))
assert json.loads((p/'recovery.json').read_text())['tail_damaged']
with tarfile.open(p/'session.tar') as t:
    assert set(t.getnames())=={'session.json','observations.wd','wigle.csv','export.json'}
    assert t.extractfile('wigle.csv').read()==(p/'wigle.csv').read_bytes()
print('Independent Python CSV v1.6 and TAR validation passed')
PY
$CC $COMMON $INC modules/poom_wardrive/test/test_session.c modules/poom_wardrive/wardrive_timezone.c modules/poom_gps/poom_nmea.c modules/poom_wardrive/wardrive_format.c modules/poom_wardrive/wardrive_storage.c modules/poom_wardrive/wardrive_clock.c modules/poom_wardrive/wardrive_export.c modules/poom_wardrive/wardrive_files.c tools/wardrive_hostcheck/fat_rename.c -Wl,--wrap=rename -lm -o "$TASK_TMP/session"
"$TASK_TMP/session" "$TASK_TMP/data"
python3 - "$TASK_TMP/data/sessions/gps-transitions" <<'PY'
import csv,json,sys
from pathlib import Path
p=Path(sys.argv[1])
with (p/'wigle.csv').open(newline='') as f:
    rows=list(csv.reader(f))
assert len(rows)==8 and all(len(r)==14 for r in rows[1:])
assert [r[3] for r in rows[2:]]==[
    f'2026-11-08 12:00:{s:02d}' for s in (3,3,13,13,23,23)]
assert [r[-1] for r in rows[2:]]==['WIFI','BLE']*3
assert all(abs(float(r[7])-48.1173)<1e-7 for r in rows[2:])
report=json.loads((p/'export.json').read_text())
assert report['exported']==6 and report['unlocated_or_untimed']==2
assert not report['tail_damaged']
recovery=json.loads((p/'recovery.json').read_text())
assert recovery['valid_observations']==8 and not recovery['tail_damaged']
assert json.loads((p/'session.json').read_text())['display_timezone']=='Australia/Sydney'
print('Independent GPS/session CSV timestamps and recovery validation passed')
PY
CJSON_DIR=${CJSON_DIR:-managed_components/espressif__cjson/cJSON}
if [ -f "$CJSON_DIR/cJSON.c" ]; then
    $CC $COMMON $INC -Itools/wardrive_hostcheck/stubs -Imodules/poom_secrets_store/include -I"$CJSON_DIR" -DWD_ROOT=\"$TASK_TMP/data\" modules/poom_wardrive/test/test_credentials.c modules/poom_wardrive/wardrive_credentials.c "$CJSON_DIR/cJSON.c" -o "$TASK_TMP/credentials"
    "$TASK_TMP/credentials"
else
    echo 'Credential tests require CJSON_DIR pointing to cJSON source (IDF component manager downloads it).'
    exit 1
fi
$CC $COMMON $INC -I"$CJSON_DIR" modules/poom_wardrive/test/test_sessions.c modules/poom_wardrive/wardrive_timezone.c modules/poom_wardrive/wardrive_sessions.c modules/poom_wardrive/wardrive_format.c "$CJSON_DIR/cJSON.c" -Wl,--wrap=realloc -lm -o "$TASK_TMP/sessions"
"$TASK_TMP/sessions" "$TASK_TMP/data"
$CC $COMMON $INC -Itools/wardrive_hostcheck/stubs tools/wardrive_hostcheck/test_clock.c modules/poom_wardrive/wardrive_clock.c modules/poom_wardrive/wardrive_clock_scope.c modules/poom_wardrive/wardrive_format.c -lm -o "$TASK_TMP/clock"
"$TASK_TMP/clock"
python3 tools/wardrive_hostcheck/test_clock_jobs.py
python3 tools/wardrive_hostcheck/test_radio_lifecycle.py
python3 tools/wardrive_hostcheck/test_transfer_connect.py
python3 tools/wardrive_hostcheck/test_upload_jobs.py
$CC $COMMON $INC -Itools/wardrive_hostcheck/stubs -I"$CJSON_DIR" -DWD_ROOT=\"$TASK_TMP/data\" modules/poom_wardrive/test/test_transfer.c modules/poom_wardrive/wardrive_timezone.c modules/poom_wardrive/wardrive_format.c modules/poom_wardrive/wardrive_sessions.c modules/poom_wardrive/wardrive_export.c modules/poom_wardrive/wardrive_files.c "$CJSON_DIR/cJSON.c" -lm -o "$TASK_TMP/transfer"
"$TASK_TMP/transfer"
$CC $COMMON $INC -DCONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1 -Itools/wardrive_hostcheck/stubs -I"$CJSON_DIR" modules/poom_wardrive/test/test_upload.c modules/poom_wardrive/wardrive_upload.c modules/poom_wardrive/wardrive_files.c tools/wardrive_hostcheck/fat_rename.c -Wl,--wrap=rename -Wl,--wrap=fopen -Wl,--wrap=fclose "$CJSON_DIR/cJSON.c" -lm -o "$TASK_TMP/upload"
"$TASK_TMP/upload" "$TASK_TMP/data/sessions/synthetic"
$CC $COMMON $INC -DCONFIG_IDF_TARGET_ESP32C5=1 -Itools/wardrive_hostcheck/radio_stubs -Itools/wardrive_hostcheck/stubs modules/poom_wardrive/test/test_radio.c modules/poom_wardrive/wardrive_format.c modules/poom_gps/poom_nmea.c -lm -o "$TASK_TMP/radio"
"$TASK_TMP/radio"
python3 tools/wardrive_hostcheck/test_menu_handoff.py

$CC $COMMON $INC modules/poom_wardrive/test/test_files.c modules/poom_wardrive/wardrive_files.c tools/wardrive_hostcheck/fat_rename.c -Wl,--wrap=rename -o "$TASK_TMP/files"
"$TASK_TMP/files" "$TASK_TMP/data"
$CC $COMMON -Iapplications/poom_app_pack/src tools/wardrive_hostcheck/test_ui_nav.c -o "$TASK_TMP/ui_nav"
"$TASK_TMP/ui_nav"
$CC $COMMON $INC -Iapplications/poom_app_pack/src tools/wardrive_hostcheck/test_ui_logic.c applications/poom_app_pack/src/wardrive_ui_logic.c modules/poom_wardrive/wardrive_timezone.c modules/poom_wardrive/wardrive_format.c -lm -o "$TASK_TMP/ui_logic"
"$TASK_TMP/ui_logic"
# Whole production UI (menu_wardrive.c) with hardware stubs; optional
# WARDRIVE_PREVIEW_DIR=dir saves PBM screenshots of each reviewed screen.
mkdir "$TASK_TMP/ui"
$CC $COMMON $INC -DCONFIG_IDF_TARGET_ESP32C5=1 -DWD_ROOT=\"$TASK_TMP/ui\" -Itools/wardrive_hostcheck/ui_stubs -Itools/wardrive_hostcheck/radio_stubs -Itools/wardrive_hostcheck/stubs \
    -Imodules/poom_secrets_store/include -Imodules/poom_ui_keyboard/include -Iapplications/poom_app_pack/src -Iapplications/poom_app_pack/include -I"$CJSON_DIR" \
    tools/wardrive_hostcheck/test_ui_flow.c applications/poom_app_pack/src/wardrive_ui_logic.c modules/poom_wardrive/wardrive_sessions.c \
    modules/poom_wardrive/wardrive_timezone.c modules/poom_wardrive/wardrive_format.c modules/poom_wardrive/wardrive_files.c \
    modules/poom_wardrive/wardrive_export.c modules/poom_wardrive/wardrive_credentials.c modules/poom_wardrive/wardrive_distance.c \
    modules/poom_gps/poom_nmea.c modules/poom_gps/poom_gps_capture.c modules/poom_wardrive/wardrive_clock.c modules/poom_arduboy_display/poom_arduboy_font5x7.c "$CJSON_DIR/cJSON.c" -Wl,--wrap=fopen -Wl,--wrap=opendir -lm -o "$TASK_TMP/ui_flow"
"$TASK_TMP/ui_flow"
$CC $COMMON $INC modules/poom_gps/test/test_capture.c modules/poom_gps/poom_gps_capture.c modules/poom_gps/poom_nmea.c -lm -o "$TASK_TMP/capture"
"$TASK_TMP/capture" "$TASK_TMP/data"


python3 tools/wardrive_hostcheck/test_isolation_contract.py
