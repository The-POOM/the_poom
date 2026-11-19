This module ports five Wi-Fi frame families from ESP32 Marauder develop
`ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da` into POOM's native ESP-IDF runtime.
Credit belongs to Just Call Me Koko for the original implementations; the full
MIT notice is in `MARAUDER_LICENSE.txt`.

The caller first stops other radio services, discovers a selected AP and any
required stations, then supplies that snapshot to `poom_wifi_marauder_start`.
Bad Msg and Assoc Sleep cycle the supplied client list. Their "all" and
"targeted" UI actions differ only in which discovered clients the UI supplies;
neither mode discovers clients inside the transmit worker. Quiet Time and
Channel Switch use the selected AP's identity/channel. SAE Commit accepts
WPA3-Personal or WPA2/WPA3-Personal transition APs.

Runs default to 60 seconds and cannot exceed 60 seconds. One frame is attempted
per interval (minimum/default 100 ms), round-robin for a client list. Stop waits
for worker cleanup before returning. The worker stops at the first transmit,
frame-build, or crypto failure. `sent` counts successful Wi-Fi driver returns;
it does not prove over-air reception, association disruption, or client behavior.

The native port uses the existing POOM raw-transmit driver environment. Driver
acceptance of arbitrary data/authentication frames is SDK- and device-dependent;
unsupported calls are surfaced in `last_error`, not counted as sent. Radio
initialization, tuning, power-save configuration and promiscuous-mode setup are
checked. The SDK continues to enforce country/channel restrictions. Channel
Switch requires a different supported destination channel in the same band.

Compatibility limits:

- Bad Msg preserves Marauder's deliberately unusual EAPOL key flags. Each
  frame is built independently. The shorter WPA3 body has a matching zero
  key-data length; WPA2 fields cannot inherit mutations from a previous target.
  Descriptor choice follows the AP's advertised authentication mode. A transition
  AP can have clients using different negotiated security modes; no per-client
  negotiated cipher/AKM is inferred by this module.
- Assoc Sleep preserves the association request's power-management bit and
  WPA2-PSK RSN capabilities. It is not a complete association negotiation, and
  modern clients/APs may ignore it. Supported rates follow the selected band;
  channel and operating-class elements describe the selected channel only.
- Quiet Time deliberately uses POOM parameters: count 1, period 0, duration
  100 TU (102.4 ms), offset 0. The requested quiet interval starts at the next
  target beacon transmission time (TBTT). Period zero advertises no recurring
  quiet schedule; the worker refreshes one-shot announcements only during its
  bounded run. These values replace upstream's period 5, duration 65535 TU,
  and offset 25600 TU. The advertised beacon interval is also 100 TU. A receiver
  can still act on an already received announcement after transmission stops;
  stopping this transmitter is not proof of immediate receiver recovery.
- Channel Switch uses mode 1 and count 1, announcing a switch at the next TBTT,
  rather than repeating upstream's count 255. Beacon construction uses a current
  timestamp and sequence; the hardcoded US country element and unrelated
  vendor/TIM elements from upstream Quiet are omitted. Clients may ignore
  unprotected or inconsistent announcements. These adaptations are not proof
  that a particular device performs either advertised action.
- SAE generates fresh valid P-256 scalar/point material with PSA Crypto and
  sends initial group-19 commits. It does not implement anti-clogging token
  retries, H2E/password-element negotiation, or complete SAE authentication.
  A target requiring any of those can reject the initial commits.

No automatic scan, packet capture, anti-clogging receiver, or background network
selection is hidden in this backend. Those are separate UI/scanner services.
Portable frame and mocked-lifecycle tests do not exercise physical RF. Firmware
build success also does not establish live attack effectiveness.

Field semantics were checked against IEEE 802.11's Quiet-element definition
(count/period in beacon intervals, duration/offset in TU; count zero reserved)
and hostap's CSA TBTT-count definition:
https://www.cs.mun.ca/~yzchen/bib/802.11-2007.pdf (section 7.3.2.23)
https://www.w1.fi/wpa_supplicant/devel/structcsa__settings.html
The IDF 6.1 bundled ieee802_11_defs.h identifies the CSA/Quiet element IDs as
37/40; Linux's ieee80211_quiet_ie also confirms the two little-endian 16-bit
duration/offset fields.
