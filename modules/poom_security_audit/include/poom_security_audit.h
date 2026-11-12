// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_security_audit.h
 * @brief Nmap-style network scan engine for the POOM (ESP32-C5 / ESP-IDF).
 *
 * This is the headless engine behind the Security Audit app. It owns no UI
 * and no Wi-Fi connection logic: the caller joins a network first (via
 * poom_wifi_ctrl + the saved Wi-Fi Profiles) and then drives this engine, which
 * provides three independent capabilities:
 *
 *   1. Subnet math      - derive the local network/host-range from the STA
 *                          interface's IP + netmask, with a hard host cap so an
 *                          unexpectedly wide subnet can never trigger an
 *                          unbounded sweep.
 *   2. Host discovery    - ICMP echo (esp_ping) + ARP-table resolution to list
 *                          live hosts with IP and (where known) MAC. Confirmed
 *                          facts are flagged separately from inference.
 *   3. Port scanning     - TCP Connect (lwIP non-blocking connect + select) over
 *                          Quick / Extended / Full / Custom port sets, with a
 *                          conservative concurrency cap, per-socket timeout,
 *                          progress callback and cooperative cancellation. A
 *                          TCP SYN method is declared here but is feasibility-
 *                          gated (see poom_security_audit_syn_available()).
 *
 * Everything is caller-owned storage; the engine keeps no global scan state,
 * which keeps the pure logic (subnet math, port-set builder, service table)
 * host-compilable and unit-testable without an ESP32.
 */

#ifndef POOM_SECURITY_AUDIT_H
#define POOM_SECURITY_AUDIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* =========================================================================
 * Limits / tunables
 * ========================================================================= */

/** @brief Hard ceiling on hosts discovered/scanned in one pass (RAM + time). */
#define POOM_SECURITY_AUDIT_MAX_HOSTS        (256U)

/** @brief Largest subnet (host count) the engine will sweep. A /22 is 1022
 *  usable hosts; anything wider is clamped to the first this-many hosts so a
 *  misconfigured /16 never becomes a 65k-probe runaway. */
#define POOM_SECURITY_AUDIT_MAX_SWEEP_HOSTS  (1022U)

/** @brief Maximum ports recorded per host in one port scan. */
#define POOM_SECURITY_AUDIT_MAX_PORTS        (2048U)

/** @brief Default parallel TCP connects. Kept well under the lwIP socket cap
 *  (CONFIG_LWIP_MAX_SOCKETS, default 10) so the rest of the system keeps
 *  sockets. Raise CONFIG_LWIP_MAX_SOCKETS before raising this. */
#define POOM_SECURITY_AUDIT_DEFAULT_FANOUT   (8U)

/** @brief Absolute cap on parallel connects regardless of caller request. */
#define POOM_SECURITY_AUDIT_MAX_FANOUT       (12U)

/** @brief IPv4 dotted-quad string capacity ("255.255.255.255" + NUL). */
#define POOM_SECURITY_AUDIT_IP_STR_CAP       (16U)

/* =========================================================================
 * Subnet
 * ========================================================================= */

/**
 * @brief Derived view of the local IPv4 subnet, in host byte order fields plus
 *        ready-to-use first/last host integers.
 */
typedef struct
{
    uint32_t ip;          /**< This device's IP (host byte order).      */
    uint32_t netmask;     /**< Subnet mask (host byte order).           */
    uint32_t gateway;     /**< Default gateway (host byte order, 0 ok). */
    uint32_t network;     /**< Network address = ip & netmask.          */
    uint32_t broadcast;   /**< Broadcast = network | ~netmask.          */
    uint32_t first_host;  /**< First scannable host address.            */
    uint32_t last_host;   /**< Last scannable host address.             */
    uint32_t host_count;  /**< Scannable host count (after clamping).   */
    uint8_t  prefix_len;  /**< CIDR prefix length (0..32).              */
    bool     clamped;     /**< true when host_count was capped.         */
} poom_security_audit_subnet_t;

/**
 * @brief Compute subnet fields from an IP + netmask (pure, host-testable).
 *
 * Fills network/broadcast/first_host/last_host/host_count/prefix_len. For a
 * /31 or /32 the usable range degenerates sensibly (first==last==ip). The host
 * count is clamped to POOM_SECURITY_AUDIT_MAX_SWEEP_HOSTS and @c clamped is set.
 *
 * @param[in]  ip_host      IP in host byte order.
 * @param[in]  netmask_host Netmask in host byte order.
 * @param[in]  gw_host      Gateway in host byte order (0 if unknown).
 * @param[out] out          Result. Must not be NULL.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG on NULL / non-contiguous mask.
 */
esp_err_t poom_security_audit_subnet_compute(uint32_t ip_host, uint32_t netmask_host,
                                          uint32_t gw_host,
                                          poom_security_audit_subnet_t *out);

/**
 * @brief Read the live STA interface IP/mask/gw and compute the subnet.
 *
 * Requires an active STA link with a valid IP (poom_wifi_ctrl_sta_has_ip()).
 *
 * @param[out] out Result. Must not be NULL.
 * @return ESP_OK, ESP_ERR_INVALID_STATE (no IP), else an esp_netif error.
 */
esp_err_t poom_security_audit_subnet_current(poom_security_audit_subnet_t *out);

/** @brief Format a host-order IPv4 address as dotted quad into @p out. */
void poom_security_audit_ip_to_str(uint32_t ip_host, char *out, size_t out_cap);

/* =========================================================================
 * Discovery
 * ========================================================================= */

/** @brief One discovered host. */
typedef struct
{
    char     ip_str[POOM_SECURITY_AUDIT_IP_STR_CAP]; /**< Dotted-quad IP.         */
    uint32_t ip;                                  /**< IP (host byte order).   */
    uint8_t  mac[6];                              /**< MAC when @c mac_known.  */
    bool     mac_known;                           /**< MAC resolved via ARP.   */
    bool     icmp_responded;                      /**< Replied to ICMP echo.   */
    bool     arp_responded;                       /**< Appeared in ARP table.  */
    uint8_t  ttl;                                 /**< IP TTL of the ICMP reply.*/
    bool     ttl_known;                           /**< @c ttl is valid.        */
} poom_security_audit_host_t;

/** @brief Discovery tuning. Zero-initialise then override as needed. */
typedef struct
{
    uint32_t icmp_timeout_ms; /**< Per-host ICMP wait (0 => default 200).      */
    uint32_t arp_wait_ms;     /**< ARP settle time per batch (0 => default).   */
    bool     use_icmp;        /**< Send ICMP echo (default path).              */
    bool     use_arp;         /**< Resolve/confirm via ARP table.              */
} poom_security_audit_discovery_opts_t;

/**
 * @brief Progress callback. @p done/@p total let the UI render a bar; return
 *        value is ignored. Keep it short and non-blocking.
 */
typedef void (*poom_security_audit_progress_cb_t)(uint32_t done, uint32_t total,
                                                void *user);

/**
 * @brief Discover live hosts on @p subnet via ICMP and/or ARP.
 *
 * Hosts that block ICMP but answer ARP (or vice-versa) are still listed; the
 * per-host flags say which probe succeeded so the UI can mark confirmed vs
 * inferred. Networks with client isolation simply yield few/no hosts - not an
 * error. Honors @p cancel between hosts.
 *
 * @param[in]  subnet   Subnet to sweep.
 * @param[in]  opts     Options (NULL => ICMP+ARP, default timeouts).
 * @param[out] out      Caller array of @p cap entries.
 * @param[in]  cap      Capacity of @p out (<= POOM_SECURITY_AUDIT_MAX_HOSTS used).
 * @param[out] out_count Number of hosts found.
 * @param[in]  cancel   Optional cooperative-cancel flag (NULL => never).
 * @param[in]  cb       Optional progress callback (NULL => none).
 * @param[in]  user     Opaque pointer passed to @p cb.
 * @return ESP_OK on completion (including cancellation), else an error.
 */
esp_err_t poom_security_audit_discover(const poom_security_audit_subnet_t *subnet,
                                    const poom_security_audit_discovery_opts_t *opts,
                                    poom_security_audit_host_t *out, size_t cap,
                                    size_t *out_count,
                                    const volatile bool *cancel,
                                    poom_security_audit_progress_cb_t cb, void *user);

/**
 * @brief Resolve the MAC of a single already-known host via the ARP table.
 *        Useful for "Scan Specific IP" where no sweep ran.
 * @return ESP_OK and fills @p mac on success; ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t poom_security_audit_resolve_mac(const char *ip_str, uint8_t mac[6]);

/* =========================================================================
 * Port scanning
 * ========================================================================= */

/** @brief Observed port state. */
typedef enum
{
    POOM_SECURITY_AUDIT_PORT_UNKNOWN = 0, /**< Not yet probed.                    */
    POOM_SECURITY_AUDIT_PORT_OPEN,        /**< Connect succeeded / SYN-ACK.       */
    POOM_SECURITY_AUDIT_PORT_CLOSED,      /**< Connection refused (RST).          */
    POOM_SECURITY_AUDIT_PORT_FILTERED,    /**< No response within timeout.        */
} poom_security_audit_port_state_t;

/** @brief Scan method. */
typedef enum
{
    POOM_SECURITY_AUDIT_METHOD_TCP_CONNECT = 0, /**< Full 3-way handshake.        */
    POOM_SECURITY_AUDIT_METHOD_TCP_SYN,         /**< Half-open (feasibility-gated).*/
} poom_security_audit_method_t;

/** @brief Port-set profile. */
typedef enum
{
    POOM_SECURITY_AUDIT_PROFILE_QUICK = 0, /**< ~common TCP ports.                */
    POOM_SECURITY_AUDIT_PROFILE_EXTENDED,  /**< Top ~1000 TCP ports.              */
    POOM_SECURITY_AUDIT_PROFILE_FULL,      /**< 1..65535.                         */
    POOM_SECURITY_AUDIT_PROFILE_CUSTOM,    /**< Caller-supplied list/range.       */
} poom_security_audit_profile_t;

/** @brief One port result. */
typedef struct
{
    uint16_t                      port;      /**< TCP port number.             */
    poom_security_audit_port_state_t state;     /**< Observed state.              */
    poom_security_audit_method_t     method;    /**< How it was probed.           */
    const char                   *service;   /**< Likely service (never NULL). */
    bool                          service_is_guess; /**< true => port-table guess, not a verified banner. */
} poom_security_audit_port_result_t;

/** @brief Port-scan tuning. Zero-initialise then override. */
typedef struct
{
    poom_security_audit_method_t method;       /**< TCP_CONNECT or TCP_SYN.       */
    uint32_t                  connect_timeout_ms; /**< Per-port wait (0=>600). */
    uint8_t                   fanout;       /**< Parallel connects (0=>default).*/
    bool                      open_only;    /**< Record only OPEN ports.       */
    uint32_t                 *n_closed;     /**< Optional: += closed ports seen  (NULL ok). */
    uint32_t                 *n_filtered;   /**< Optional: += filtered ports seen (NULL ok). */
} poom_security_audit_scan_opts_t;

/**
 * @brief Build a port list for a profile into @p out (pure, host-testable).
 *
 * For CUSTOM the caller supplies ports directly and should not call this.
 *
 * @param[in]  profile Profile to expand.
 * @param[out] out     Destination array.
 * @param[in]  cap     Capacity of @p out.
 * @return Number of ports written (<= cap). FULL returns min(65535, cap).
 */
size_t poom_security_audit_build_ports(poom_security_audit_profile_t profile,
                                    uint16_t *out, size_t cap);

/**
 * @brief Windowed form of poom_security_audit_build_ports(): writes the profile's
 *        ports starting at position @p offset of its full (de-duplicated)
 *        list, up to @p cap. Lets callers scan Extended/Full through a small
 *        buffer. Concatenating windows equals the full list.
 * @return Ports written; 0 once @p offset is past the end (or for CUSTOM).
 */
size_t poom_security_audit_build_ports_window(poom_security_audit_profile_t profile, uint32_t offset,
                                           uint16_t *out, size_t cap);

/**
 * @brief Parse a user "ports" string such as "22,80,443,8000-8010" into @p out.
 *        Whitespace is ignored; ranges are inclusive; duplicates are kept in
 *        order of appearance (the scanner de-dupes at probe time if needed).
 * @return Number of ports written (<= cap), or 0 on a malformed string.
 */
size_t poom_security_audit_parse_ports(const char *spec, uint16_t *out, size_t cap);

/**
 * @brief Scan @p ports on a single host.
 *
 * TCP_CONNECT uses non-blocking connect()+select() with @p fanout parallel
 * sockets. Honors @p cancel between batches. With TCP_SYN, if SYN is not
 * available this returns ESP_ERR_NOT_SUPPORTED and scans nothing (the UI must
 * not silently fall back to Connect).
 *
 * @param[in]  ip_str   Target IPv4 dotted quad.
 * @param[in]  ports    Ports to probe.
 * @param[in]  n_ports  Number of ports.
 * @param[in]  opts     Options (NULL => TCP_CONNECT, defaults).
 * @param[out] out      Caller result array of @p cap entries.
 * @param[in]  cap      Capacity of @p out.
 * @param[out] out_count Results written.
 * @param[in]  cancel   Optional cooperative-cancel flag.
 * @param[in]  cb       Optional progress callback.
 * @param[in]  user     Opaque pointer for @p cb.
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED (SYN unavailable), else an error.
 */
esp_err_t poom_security_audit_port_scan(const char *ip_str,
                                     const uint16_t *ports, size_t n_ports,
                                     const poom_security_audit_scan_opts_t *opts,
                                     poom_security_audit_port_result_t *out,
                                     size_t cap, size_t *out_count,
                                     const volatile bool *cancel,
                                     poom_security_audit_progress_cb_t cb,
                                     void *user);

/* =========================================================================
 * Service table / helpers
 * ========================================================================= */

/**
 * @brief Best-guess service name for a TCP port (pure, host-testable).
 *
 * This is a static port->name table (a guess from the port number only, not a
 * verified banner). Always returns a non-NULL string; unknown ports map to
 * "unknown".
 */
const char *poom_security_audit_service_name(uint16_t port);

/** @brief Human label for a port state ("open"/"closed"/"filtered"/"-"). */
const char *poom_security_audit_state_label(poom_security_audit_port_state_t state);

/** @brief Human label for a method ("TCP Connect"/"TCP SYN"). */
const char *poom_security_audit_method_label(poom_security_audit_method_t method);

/**
 * @brief Whether a reliable TCP SYN scan is available on this build/target.
 *
 * Returns false unless a verified raw-socket SYN implementation is compiled in
 * and known to coexist with the lwIP TCP stack. The UI uses this to disable the
 * SYN option rather than silently substitute TCP Connect.
 */
bool poom_security_audit_syn_available(void);

/* =========================================================================
 * Service / version detection (nmap -sV-lite)
 * ========================================================================= */

/** @brief How strongly a service/version was established (never overstate). */
typedef enum
{
    POOM_SECURITY_AUDIT_EV_PORT_GUESS = 0,   /**< From the port number only.      */
    POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE,   /**< Confirmed by a protocol reply.  */
    POOM_SECURITY_AUDIT_EV_VERSION_REPORTED, /**< Service reported its version.   */
} poom_security_audit_evidence_t;

#define POOM_SECURITY_AUDIT_SVC_CAP     (16U)
#define POOM_SECURITY_AUDIT_PRODUCT_CAP (28U)
#define POOM_SECURITY_AUDIT_VERSION_CAP (24U)
#define POOM_SECURITY_AUDIT_BANNER_CAP  (80U)

/** @brief Result of service/version detection on one open port. */
typedef struct
{
    uint16_t                    port;
    char                        service[POOM_SECURITY_AUDIT_SVC_CAP];     /**< "http","ssh",... */
    char                        product[POOM_SECURITY_AUDIT_PRODUCT_CAP]; /**< "OpenSSH","nginx","" */
    char                        version[POOM_SECURITY_AUDIT_VERSION_CAP]; /**< "9.6p1","" if none */
    char                        banner[POOM_SECURITY_AUDIT_BANNER_CAP];   /**< trimmed raw evidence */
    poom_security_audit_evidence_t evidence;
} poom_security_audit_service_t;

/** @brief Human label for an evidence level ("guessed"/"confirmed"/"version"). */
const char *poom_security_audit_evidence_label(poom_security_audit_evidence_t ev);

/**
 * @brief Classify a raw TCP banner into service/product/version (pure,
 *        host-testable). Recognises SSH/FTP/SMTP/POP3/IMAP greetings; otherwise
 *        leaves a trimmed snippet with PROTO_RESPONSE if any text was seen, or
 *        PORT_GUESS (service from @p port) if empty.
 */
void poom_security_audit_parse_banner(const char *banner, size_t len, uint16_t port,
                                   poom_security_audit_service_t *out);

/**
 * @brief First "model-like" token in free text (letters AND digits, 3-20
 *        chars, not version-like): "NETGEAR R7000" -> "R7000",
 *        "ASUS RT-AC68U Login" -> "RT-AC68U". @return length (0 = none).
 */
size_t poom_security_audit_model_token(const char *text, char *out, size_t cap);

/**
 * @brief Parse an HTTP response head into service "http" + Server product/
 *        version (pure, host-testable). Returns false on a non-HTTP buffer
 *        (and leaves @p out unchanged).
 */
bool poom_security_audit_parse_http(const char *resp, size_t len,
                                 poom_security_audit_service_t *out);

/**
 * @brief Detect the service (and version where volunteered) on one open TCP
 *        port: passive banner grab, then an HTTP probe, else port-number guess.
 *        Networked; must run off the UI thread. No TLS handshake here (later).
 *        @p out is always populated (at worst PORT_GUESS).
 */
esp_err_t poom_security_audit_detect_service(const char *ip_str, uint16_t port,
                                          uint32_t timeout_ms,
                                          poom_security_audit_service_t *out,
                                          const volatile bool *cancel);

/* =========================================================================
 * OS / device hint + compact service ids (pure, host-testable)
 * ========================================================================= */

/**
 * @brief Best-effort OS / device family hint (NOT nmap -O fingerprinting).
 *
 * Priority: a service banner's OS token (strong) > the ICMP reply TTL family
 * (inferred: ~64 Linux/Unix, ~128 Windows, ~255 network device) > the MAC
 * vendor (inferred). Writes a short label into @p out ("" if nothing); sets
 * @p out_strong true only when derived from a banner.
 *
 * @param[in]  svc        Optional detected service (NULL if none yet).
 * @param[in]  ttl        ICMP reply TTL (ignored unless @p ttl_known).
 * @param[in]  ttl_known  Whether @p ttl is valid.
 * @param[in]  vendor     Optional OUI vendor string (NULL/"" if unknown).
 * @param[out] out        Destination label buffer.
 * @param[in]  cap        Capacity of @p out.
 * @param[out] out_strong Optional; set true when the hint came from a banner.
 */
void poom_security_audit_os_hint(const poom_security_audit_service_t *svc,
                              uint8_t ttl, bool ttl_known, const char *vendor,
                              char *out, size_t cap, bool *out_strong);

/** @brief Map a service name to a small stable id (0 = unknown), and back.
 *  Used to store a service per open port compactly (1 byte) in bulk scans. */
uint8_t     poom_security_audit_service_id(const char *name);
const char *poom_security_audit_service_by_id(uint8_t id);

/* =========================================================================
 * Duration estimate (pure, host-testable) - for the Full NetAudit confirm
 * ========================================================================= */

/**
 * @brief Worst-case TCP Connect scan time in seconds for @p hosts hosts of
 *        @p ports_per_host ports each: every batch of @p fanout ports waits the
 *        full @p timeout_ms (i.e. all filtered). Closed ports RST quickly, so
 *        real runs are usually faster. 0 fanout/timeout => engine defaults.
 *        Rounded up; saturates at UINT32_MAX.
 */
uint32_t poom_security_audit_estimate_scan_s(uint32_t hosts, uint32_t ports_per_host,
                                          uint32_t fanout, uint32_t timeout_ms);

/**
 * @brief Format seconds compactly for a 128x64 screen: "45s", "12m", "3h05m",
 *        ">99h". Always NUL-terminates @p out (cap >= 1).
 */
void poom_security_audit_fmt_duration(uint32_t seconds, char *out, size_t cap);

/* =========================================================================
 * Device / OS identification (pure parts host-tested in test/test_ident.c)
 * ========================================================================= */

/** @brief OS / device identity ids (compact: stored as one byte per host). */
typedef enum
{
    POOM_NS_OS_UNKNOWN = 0,
    POOM_NS_OS_POSIX,      /**< generic Unix-like (e.g. TTL 64)            */
    POOM_NS_OS_EMBEDDED,   /**< generic embedded stack (e.g. TTL 255)      */
    POOM_NS_OS_WINDOWS,
    POOM_NS_OS_LINUX,
    POOM_NS_OS_UBUNTU,
    POOM_NS_OS_DEBIAN,
    POOM_NS_OS_RHEL,       /**< RHEL / CentOS / Fedora                     */
    POOM_NS_OS_RPI,        /**< Raspberry Pi OS                            */
    POOM_NS_OS_OPENWRT,
    POOM_NS_OS_ROUTEROS,
    POOM_NS_OS_ANDROID,
    POOM_NS_OS_BSD,
    POOM_NS_OS_APPLE,      /**< Apple, macOS vs iOS not yet known          */
    POOM_NS_OS_MACOS,
    POOM_NS_OS_IOS,        /**< iOS / iPadOS                               */
    POOM_NS_OS_NETDEV,     /**< router / AP / switch                       */
    POOM_NS_OS_PRINTER,
    POOM_NS_OS_CAMERA,
    POOM_NS_OS_MEDIA,      /**< TV / streamer / speaker                    */
    POOM_NS_OS_IOT,
    POOM_NS_OS_NAS,
    POOM_NS_OS_CONSOLE,
    POOM_NS_OS_COUNT
} poom_ns_os_t;

/** @brief How an identity was established (never overstate). */
typedef enum
{
    POOM_NS_OSEV_GUESS = 0,  /**< open-port signature / MAC vendor          */
    POOM_NS_OSEV_INFERRED,   /**< ping reply TTL family                     */
    POOM_NS_OSEV_CONFIRMED,  /**< self-reported (banner, SSDP, mDNS model)   */
} poom_ns_osev_t;

#define POOM_NS_NAME_CAP (16U)

/** @brief Per-host identity (compact; one per discovered host). */
typedef struct
{
    char    name[POOM_NS_NAME_CAP]; /**< hostname (mDNS / NetBIOS), "" if none */
    uint8_t os;                     /**< poom_ns_os_t                          */
    uint8_t os_ev;                  /**< poom_ns_osev_t (valid if os != 0)     */
    uint8_t ttl;                    /**< ping reply TTL (valid if ttl_known)   */
    bool    ttl_known;
} poom_security_audit_ident_t;

/** @brief Short display label for an os id ("Windows", "Ubuntu", ...). */
const char *poom_ns_os_label(uint8_t os);

/** @brief Display label for an evidence level ("guess"/"inferred"/"confirmed"). */
const char *poom_ns_osev_label(uint8_t ev);

/**
 * @brief Fold one OS clue into @p id. Consistent clues refine the label to the
 *        more specific one (e.g. TTL "Unix-like" + banner "Ubuntu" -> Ubuntu)
 *        and keep the strongest evidence; a conflicting clue only replaces the
 *        current one when its evidence is strictly stronger.
 */
void poom_ns_os_add(poom_security_audit_ident_t *id, uint8_t os, uint8_t ev);

/** @brief Fold every OS/device token found in free text (banner, product,
 *         SSDP SERVER header, mDNS model) at evidence @p ev. */
void poom_ns_os_from_text(poom_security_audit_ident_t *id, const char *text, uint8_t ev);

/** @brief Fold the open-port signature (GUESS). */
void poom_ns_os_from_ports(poom_security_audit_ident_t *id, const uint16_t *ports, size_t n);

/** @brief Fold the ping reply TTL family (INFERRED). */
void poom_ns_os_from_ttl(poom_security_audit_ident_t *id, uint8_t ttl);

/** @brief Fold a MAC-vendor hint (GUESS). */
void poom_ns_os_from_vendor(poom_security_audit_ident_t *id, const char *vendor);

/* --- packet helpers (pure) ----------------------------------------------- */

/** @brief Build an ICMP echo request (8-byte header + @p payload_len bytes of
 *         pattern) with a valid checksum. @return bytes written, 0 if cap small. */
size_t poom_ns_icmp_echo_build(uint8_t *buf, size_t cap, uint16_t ident, uint16_t seq,
                               size_t payload_len);

/** @brief Parse a raw IPv4 packet carrying an ICMP echo REPLY with @p ident.
 *  @return true and the sender (host order) + TTL on a match. */
bool poom_ns_icmp_reply_parse(const uint8_t *pkt, size_t len, uint16_t ident,
                              uint32_t *src_host, uint8_t *ttl);

/** @brief Extract the value of the SERVER header from an SSDP response into
 *         @p out (case-insensitive header name). @return true if found. */
bool poom_ns_ssdp_server(const char *resp, size_t len, char *out, size_t cap);

/** @brief Build a NetBIOS node-status (NBSTAT "*") query. @return bytes. */
size_t poom_ns_nbns_query_build(uint8_t *buf, size_t cap, uint16_t txid);

/** @brief Parse a node-status reply: first unique (non-group) name with
 *         suffix 0x00, trailing spaces trimmed. @return true if found. */
bool poom_ns_nbns_reply_name(const uint8_t *pkt, size_t len, char *out, size_t cap);

/** @brief Build an mDNS legacy-unicast query asking (1) the reverse-PTR name of
 *         @p ip_host and (2) "_device-info._tcp.local" PTR. @return bytes. */
size_t poom_ns_mdns_query_build(uint8_t *buf, size_t cap, uint16_t txid, uint32_t ip_host);

/**
 * @brief Parse an mDNS/DNS response: @p name gets the hostname from a reverse
 *        PTR answer (".local" stripped) or, failing that, the _device-info
 *        instance name; @p model gets a TXT "model=" value. Either may be "".
 *        Bounds-checked; compression pointers followed with a hop limit.
 * @return true if anything was extracted.
 */
bool poom_ns_mdns_reply_parse(const uint8_t *pkt, size_t len,
                              char *name, size_t name_cap, char *model, size_t model_cap);

/** @brief A device product learned from a UPnP device description. */
typedef struct
{
    uint8_t  host;             /**< index into the hosts[] passed to identify  */
    uint16_t port;             /**< TCP port of the description URL            */
    char     manufacturer[16];
    char     model[24];        /**< search term (model token / product name)    */
    char     version[16];      /**< firmware / software version, or ""          */
} poom_security_audit_upnp_t;

/** @brief LOCATION of an SSDP reply as an IPv4 http URL -> ip/port/path. */
bool poom_ns_ssdp_location(const char *resp, size_t len, uint32_t *ip_host, uint16_t *port,
                           char *path, size_t path_cap);

/** @brief First <tag>value</tag> in @p xml (trimmed, &amp; etc. decoded). */
bool poom_ns_xml_tag(const char *xml, size_t len, const char *tag, char *out, size_t cap);

/**
 * @brief Read a UPnP device description: manufacturer, a searchable model term
 *        (model token from modelName/modelNumber, else a non-generic
 *        manufacturer name) and firmware/software version into @p u, and the
 *        friendlyName into @p friendly. @return true if a model term was found.
 */
bool poom_ns_upnp_from_xml(const char *xml, size_t len, poom_security_audit_upnp_t *u,
                           char *friendly, size_t friendly_cap);

/**
 * @brief Plain HTTP/1.0 GET of @p path on @p ip_host:@p port (1 s connect,
 *        1 s per read). Reads up to @p cap - 1 bytes of the reply (headers +
 *        start of the body) into @p dst, NUL-terminated.
 * @return bytes read (> 0), or -1 if the port did not answer.
 */
int poom_security_audit_http_get(uint32_t ip_host, uint16_t port, const char *path,
                              char *dst, size_t cap);

/**
 * @brief Networked identification pass over @p n discovered hosts: one ping
 *        each (TTL), then SSDP M-SEARCH, NetBIOS node-status and mDNS
 *        legacy-unicast queries on one UDP socket; replies are matched to hosts
 *        by source IP. Fills names, TTL and self-reported OS clues into
 *        @p out[n] (caller zeroes it first). Devices that advertise a UPnP
 *        description get it fetched (HTTP GET, <= 2 KB) and parsed into
 *        @p upnp (up to @p upnp_cap; may be NULL). Read-only discovery
 *        traffic only. Honors @p cancel. ~3 s plus ~0.5 s per UPnP device.
 */
esp_err_t poom_security_audit_identify(const poom_security_audit_host_t *hosts, size_t n,
                                    poom_security_audit_ident_t *out,
                                    poom_security_audit_upnp_t *upnp, size_t upnp_cap,
                                    size_t *upnp_count,
                                    const volatile bool *cancel);

/**
 * @brief Take the next display line (at most @p width chars) from @p text,
 *        word-wrapping at the last space that fits (hard break for a longer
 *        word). Leading spaces are skipped. Writes the line to @p out (always
 *        NUL-terminated, also bounded by @p cap).
 * @return Start of the remaining text, or NULL when @p text is exhausted.
 *         Pure; used by the scrolling detail pages.
 */
const char *poom_security_audit_wrap(const char *text, size_t width, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* POOM_SECURITY_AUDIT_H */
