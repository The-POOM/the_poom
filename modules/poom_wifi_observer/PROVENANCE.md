# Passive Wi-Fi observer provenance

Native POOM implementation of bounded 802.11 observation, relationships, reporting,
and radio lifetime management. Pine-like OUI/minimal-beacon signatures, Pwnagotchi
JSON field names, the three-SSID threshold, and Flock candidate signatures are
adapted from JustCallMeKoko's ESP32 Marauder, MIT licensed:

- Repository: https://github.com/justcallmekoko/ESP32Marauder
- Source revision: `ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da` (`develop`).
- Source: `esp32_marauder/WiFiScan.cpp` (`pwnSnifferCallback`, PineScan, Flock
  branches in `beaconSnifferCallback`); `esp32_marauder/WiFiScan.h` signatures.
- Full upstream copyright and permission notice: [LICENSE.Marauder](LICENSE.Marauder).

POOM retains its own controller, display, buttons and SD APIs. Parsing happens
outside the Wi-Fi callback, validates complete IE boundaries, and uses bounded
cJSON parsing rather than searching arbitrary frame memory for JSON. Pine-like
confirmation requires three matching observations within ten seconds. Both Pine
and Flock results are heuristics; shared OUIs or names do not prove device identity.

There is no GPS dependency or automatic upload. The observer never transmits.
FOX HUNT is a separate existing POOM application entered only by an explicit
button action after the observer has released its receive callback and radio.
