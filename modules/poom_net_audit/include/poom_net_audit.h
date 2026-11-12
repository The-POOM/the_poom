// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_net_audit.h
 * @brief Offline SearchSploit (Exploit-DB) lookup for the Network Scanner.
 *
 * Reads the Exploit-DB metadata CSV from the microSD card
 * (`/sdcard/searchsploit/files_exploits.csv`) and finds entries whose title
 * matches a detected product/service. Phase C increment 1 is a direct,
 * streaming CSV scan (no index yet); the quote-aware CSV field extraction and
 * the title-match logic are pure and host-unit-tested.
 *
 * A match is only ever a POTENTIAL lead: Exploit-DB titles are free text and
 * versions are inconsistent, so the caller must never present a match as a
 * confirmed vulnerability.
 */

#ifndef POOM_NET_AUDIT_H
#define POOM_NET_AUDIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Firmware path of the Exploit-DB metadata CSV on the SD card. */
#define POOM_NET_AUDIT_CSV_PATH "/sdcard/searchsploit/files_exploits.csv"

/* files_exploits.csv column indices (0-based):
 * id,file,description,date_published,author,type,platform,port,date_added,
 * date_updated,verified,codes,aliases,screenshot_url,application_url,source_url */
#define POOM_NET_AUDIT_COL_ID        (0)
#define POOM_NET_AUDIT_COL_TITLE     (2)
#define POOM_NET_AUDIT_COL_TYPE      (5)
#define POOM_NET_AUDIT_COL_PLATFORM  (6)
#define POOM_NET_AUDIT_COL_CODES     (11)

#define POOM_NET_AUDIT_ID_CAP     (10U)
#define POOM_NET_AUDIT_TITLE_CAP  (64U)
#define POOM_NET_AUDIT_TYPE_CAP   (12U)
#define POOM_NET_AUDIT_PLAT_CAP   (12U)
#define POOM_NET_AUDIT_CVE_CAP    (20U)

/** @brief Confidence tier of a match (higher = better; used for sort/filter). */
typedef enum
{
    POOM_NET_AUDIT_TIER_WEAK   = 0, /**< product only, no version in title.   */
    POOM_NET_AUDIT_TIER_MEDIUM = 1, /**< product + some version token.        */
    POOM_NET_AUDIT_TIER_STRONG = 2, /**< product + the detected version.      */
} poom_net_audit_tier_t;

/** @brief One Exploit-DB match (a POTENTIAL lead, never a confirmation). */
typedef struct
{
    char    edb_id[POOM_NET_AUDIT_ID_CAP];    /**< Exploit-DB id, e.g. "52345". */
    char    title[POOM_NET_AUDIT_TITLE_CAP];  /**< Entry title (truncated).     */
    char    type[POOM_NET_AUDIT_TYPE_CAP];    /**< webapps/remote/local/dos/... */
    char    platform[POOM_NET_AUDIT_PLAT_CAP];/**< php/windows/linux/...         */
    char    cve[POOM_NET_AUDIT_CVE_CAP];      /**< First CVE in "codes", or "".  */
    uint8_t term_id;                          /**< Index of the term that matched. */
    uint8_t tier;                             /**< poom_net_audit_tier_t.        */
} poom_net_audit_match_t;

/** @brief A product term to search for, with an optional detected version. */
typedef struct
{
    const char *term;    /**< product/service, whole-word matched in titles. */
    const char *version; /**< detected version (e.g. "1.24.0"), or NULL/"".  */
} poom_net_audit_term_t;

/** @brief Max distinct terms a single multi-term pass supports. */
#define POOM_NET_AUDIT_MAX_TERMS (64U)

/**
 * @brief Bounded "keep the best" collector for a multi-term pass. Retains the
 *        highest-tier matches within a global cap and a per-term cap, so one
 *        noisy product can't crowd out the others. Caller owns @c matches.
 */
typedef struct
{
    poom_net_audit_match_t *matches;
    uint16_t cap;
    uint16_t count;
    uint8_t  per_term_cap;
    uint8_t  term_count[POOM_NET_AUDIT_MAX_TERMS];
} poom_net_audit_collector_t;

/** @brief Progress callback (bytes scanned / total). Keep it short. */
typedef void (*poom_net_audit_progress_cb_t)(uint32_t done, uint32_t total, void *user);

/* ---- pure helpers (host-testable) --------------------------------------- */

/**
 * @brief Extract CSV field @p index from @p line into @p out (quote-aware:
 *        handles double-quoted fields with embedded commas and "" escapes).
 * @return Number of chars written (excluding NUL).
 */
size_t poom_net_audit_csv_field(const char *line, int index, char *out, size_t cap);

/**
 * @brief Test one CSV line against a product/service term and, on a match,
 *        populate @p out (id/title/type/platform/first-CVE, tier=WEAK/MEDIUM).
 *        Whole-word, case-insensitive match of @p term in the title. Empty term
 *        never matches.
 * @return true on a match.
 */
bool poom_net_audit_match_line(const char *line, const char *term,
                               poom_net_audit_match_t *out);

/**
 * @brief Whole-word, case-insensitive test: does @p title contain @p term as a
 *        word (bounded by non-alphanumerics), not merely as a substring?
 */
bool poom_net_audit_title_has_word(const char *title, const char *term);

/**
 * @brief Classify a title's relevance to a product/version into a tier:
 *        STRONG (title mentions @p version), MEDIUM (title has any version-like
 *        token), WEAK (neither). @p version may be NULL/"".
 */
poom_net_audit_tier_t poom_net_audit_classify(const char *title, const char *version);

/** @brief Initialise a collector over caller-owned @p matches. */
void poom_net_audit_collector_init(poom_net_audit_collector_t *c,
                                   poom_net_audit_match_t *matches,
                                   uint16_t cap, uint8_t per_term_cap);

/**
 * @brief Consider one CSV line against all @p terms (whole-word), inserting any
 *        matches into the collector with tier-aware, per-term-balanced
 *        retention. Pure (no I/O); the streaming search calls it per line.
 */
void poom_net_audit_collect_line(poom_net_audit_collector_t *c, const char *line,
                                 const poom_net_audit_term_t *terms, uint8_t n_terms);

/* ---- network-wide Assess helpers (pure, host-testable) ------------------ */

#define POOM_NET_AUDIT_PROD_CAP  (28U)  /**< product string capacity (incl. NUL). */
#define POOM_NET_AUDIT_VER_CAP   (24U)  /**< version string capacity (incl. NUL). */

/** @brief Returned by poom_net_audit_intern() when the table is full. */
#define POOM_NET_AUDIT_INTERN_FULL (0xFFU)

/** @brief One unique (product, version) pair seen across the network. */
typedef struct
{
    char product[POOM_NET_AUDIT_PROD_CAP];
    char version[POOM_NET_AUDIT_VER_CAP];
} poom_net_audit_prodver_t;

/** @brief Network-level category of one host after Assess All. */
typedef enum
{
    POOM_NET_AUDIT_HOST_NONE = 0,   /**< no open ports recorded (not listed).  */
    POOM_NET_AUDIT_HOST_POTENTIAL,  /**< >=1 service with an Exploit-DB match.  */
    POOM_NET_AUDIT_HOST_NOMATCH,    /**< products identified, none matched.     */
    POOM_NET_AUDIT_HOST_UNASSESSED, /**< open ports but no product to search.   */
} poom_net_audit_host_class_t;

/**
 * @brief Intern a (product, version) pair into a de-duplicated table so one
 *        CSV pass can search each unique product once for the whole network.
 *        Product compare is case-insensitive; version is exact.
 * @return 1-based id (table index + 1); 0 when @p product is NULL/empty;
 *         POOM_NET_AUDIT_INTERN_FULL when a new pair doesn't fit.
 */
uint8_t poom_net_audit_intern(poom_net_audit_prodver_t *tab, uint8_t *n, uint8_t cap,
                              const char *product, const char *version);

/**
 * @brief Count matches per term (saturating at 255) into @p counts[n_terms]
 *        (zeroed first). Matches with term_id >= n_terms are ignored.
 */
void poom_net_audit_count_terms(const poom_net_audit_match_t *m, size_t n,
                                uint8_t *counts, uint8_t n_terms);

/**
 * @brief Classify one host from the interned product id of each of its open
 *        ports (0 / INTERN_FULL = no searchable product) and per-term hit
 *        counts. Any matched product -> POTENTIAL; else any product ->
 *        NOMATCH; else (open ports, nothing searchable) -> UNASSESSED.
 */
poom_net_audit_host_class_t poom_net_audit_host_class(const uint8_t *prod_ids, uint8_t n_ports,
                                                      const uint8_t *term_counts, uint8_t n_terms);

/** @brief Host OS class for exploit platform fit (from the scanner's identity). */
typedef enum
{
    POOM_NET_AUDIT_OS_UNKNOWN = 0, /**< no filtering                         */
    POOM_NET_AUDIT_OS_POSIX,       /**< Unix-like but which one unknown       */
    POOM_NET_AUDIT_OS_WINDOWS,
    POOM_NET_AUDIT_OS_LINUX,
    POOM_NET_AUDIT_OS_ANDROID,
    POOM_NET_AUDIT_OS_BSD,
    POOM_NET_AUDIT_OS_MACOS,
    POOM_NET_AUDIT_OS_IOS,
    POOM_NET_AUDIT_OS_HARDWARE,    /**< router / printer / camera / IoT ...   */
} poom_net_audit_os_t;

typedef enum
{
    POOM_NET_AUDIT_FIT_NEUTRAL = 0, /**< generic platform or OS unknown       */
    POOM_NET_AUDIT_FIT_YES,         /**< platform matches the host's OS        */
    POOM_NET_AUDIT_FIT_NO,          /**< platform is a different specific OS   */
} poom_net_audit_fit_t;

/**
 * @brief Does an Exploit-DB "platform" value (windows, linux_x86-64, php,
 *        hardware, ...) fit a host of class @p os? Language/web platforms
 *        (php, asp, java, multiple, ...) are always NEUTRAL; only a specific
 *        OS platform that cannot run on the host is NO.
 */
poom_net_audit_fit_t poom_net_audit_platform_fit(const char *platform, uint8_t os);

/**
 * @brief Project network-wide matches onto one host's local terms, so the
 *        per-device Assess screens can be reused unchanged. Local term t maps
 *        to network term @p local_to_net[t]; each copied match gets
 *        term_id = t. Filled strongest tier first (all STRONG, then MEDIUM,
 *        then WEAK) so a small @p cap keeps the best leads. Matches whose
 *        platform cannot run on a host of class @p os are dropped
 *        (POOM_NET_AUDIT_OS_UNKNOWN = keep all).
 * @return Number of matches written to @p out (<= cap).
 */
size_t poom_net_audit_project(const poom_net_audit_match_t *net, size_t net_n,
                              const uint8_t *local_to_net, uint8_t n_local,
                              uint8_t os, poom_net_audit_match_t *out, size_t cap);

/* ---- exposure checks (Phase D; pure, observation-only) ------------------- */

/** @brief One observed service on a host (from the scan; nothing is probed). */
typedef struct
{
    uint16_t    port;
    const char *service;  /**< "ssh", "http", ... or NULL */
    const char *product;  /**< "OpenSSH", ... or NULL     */
    const char *version;  /**< "7.2p2", ... or NULL       */
} poom_net_audit_svc_t;

/** @brief Exposure finding ids (bit positions in the returned mask). */
typedef enum
{
    POOM_NET_AUDIT_EXP_TELNET = 0,
    POOM_NET_AUDIT_EXP_FTP,
    POOM_NET_AUDIT_EXP_DB,
    POOM_NET_AUDIT_EXP_OLD_SSH,
    POOM_NET_AUDIT_EXP_REMOTE_DESKTOP,
    POOM_NET_AUDIT_EXP_SMB,
    POOM_NET_AUDIT_EXP_MQTT,
    POOM_NET_AUDIT_EXP_HTTP_NO_TLS,
    POOM_NET_AUDIT_EXP_UPNP,
    POOM_NET_AUDIT_EXP_RAW_PRINT,
    POOM_NET_AUDIT_EXP_COUNT
} poom_net_audit_exp_t;

/** @brief Severity of an exposure finding. */
typedef enum
{
    POOM_NET_AUDIT_SEV_INFO = 0,  /**< worth knowing, not necessarily a problem */
    POOM_NET_AUDIT_SEV_DETECTED,  /**< a weakness directly observed in the scan */
} poom_net_audit_sev_t;

/** @brief Max rules - one bit each in a findings mask. */
#define POOM_NET_AUDIT_RULES_MAX   (32U)

/**
 * @brief One built-in exposure rule (see poom_net_audit_rules.c to add one).
 *        Condition grammar (observed data only):
 *          expr   := term ('|' term)*        term := factor ('&' factor)*
 *          factor := '!' factor | '(' expr ')' | atom
 *          atom   := port:N[,N...] | service:NAME[,NAME...]
 *                  | product:NAME[(<|<=|>|>=|=)VERSION] | upnp
 *        Each atom is true if ANY observed service on the host satisfies it.
 */
typedef struct
{
    char    id[12];
    uint8_t sev;          /**< poom_net_audit_sev_t */
    char    expr[96];
    char    title[24];
    char    detail[64];
} poom_net_audit_rule_t;

/**
 * @brief Evaluate a rule condition over one host's observed services.
 * @return 1 true, 0 false, -1 syntax error.
 */
int poom_net_audit_rule_eval(const char *expr, const poom_net_audit_svc_t *svcs, size_t n,
                             bool has_upnp);

/** @brief Number of built-in rules and access to them (const, in flash). */
size_t poom_net_audit_builtin_count(void);
const poom_net_audit_rule_t *poom_net_audit_builtin(size_t i);

/**
 * @brief Evaluate the BUILT-IN rules over one host's observed services.
 * @return bit mask (bit i = built-in rule i fired).
 */
uint32_t poom_net_audit_exposures(const poom_net_audit_svc_t *svcs, size_t n, bool has_upnp);

/** @brief Title / explanation / severity of built-in rule @p id. */
const char *poom_net_audit_exp_title(uint8_t id);
const char *poom_net_audit_exp_detail(uint8_t id);
poom_net_audit_sev_t poom_net_audit_exp_severity(uint8_t id);

/** @brief Compare dotted numeric versions ("7.2p2" vs "7.4"): <0, 0, >0. */
int poom_net_audit_version_cmp(const char *a, const char *b);

/* ---- active read-only web checks (pure parts host-tested) ----------------- */

/**
 * @brief One built-in read-only check: GET @p path on a web port and look for
 *        tell-tale text in the reply. GET only, fixed paths, no payloads, no
 *        logins - it can only notice that something is exposed, never change
 *        anything. A hit is reported as CONFIRMED (the device showed it).
 */
typedef struct
{
    char     id[12];
    char     path[48];   /**< request path (GET)                              */
    uint16_t status;     /**< required HTTP status, 0 = any 2xx               */
    char     match[64];  /**< alternatives separated by '|', case-insensitive */
    char     title[24];
    char     detail[64];
} poom_net_audit_check_t;

#define POOM_NET_AUDIT_CHECKS_MAX  (16U)  /**< one bit each in a uint16 mask */

/** @brief Built-in checks (const, in flash). */
size_t poom_net_audit_check_count(void);
const poom_net_audit_check_t *poom_net_audit_check(size_t i);

/** @brief Status code of an HTTP response ("HTTP/1.1 200 OK" -> 200), -1 if none. */
int poom_net_audit_http_status(const char *resp, size_t len);

/** @brief Does @p hay (len bytes, need not be NUL-terminated) contain any of the
 *         '|'-separated alternatives in @p alts (case-insensitive)? */
bool poom_net_audit_text_has_any(const char *hay, size_t len, const char *alts);

/**
 * @brief Did check @p c hit? @p resp is the reply to its GET; @p baseline is
 *        the reply to a GET of a random nonsense path on the same port (NULL
 *        if unavailable). Soft-404 guard: devices that answer 200 with the
 *        same page for any path never count as a hit for text that their
 *        nonsense-path reply also contains.
 */
bool poom_net_audit_check_match(const poom_net_audit_check_t *c, const char *resp, size_t len,
                                const char *baseline, size_t blen);

/* ---- automatic block index (plain stdio; host-tested) -------------------- */

#define POOM_NET_AUDIT_IDX_PATH  "/sdcard/searchsploit/files_exploits.idx"
#define POOM_NET_AUDIT_IDX_TMP   "/sdcard/searchsploit/files_exploits.idt"
#define POOM_NET_AUDIT_IDX_ROWS  (64U)    /**< CSV lines per index block          */
#define POOM_NET_AUDIT_IDX_BITS  (8192U)  /**< Bloom bits per block (1 KB)        */

/** @brief How a search used the index. */
typedef enum
{
    POOM_NET_AUDIT_IDX_NONE = 0, /**< full pass, index not written          */
    POOM_NET_AUDIT_IDX_USED,     /**< indexed pass (only candidate blocks)  */
    POOM_NET_AUDIT_IDX_BUILT,    /**< full pass that (re)built the index    */
} poom_net_audit_index_use_t;

/** @brief Case-insensitive FNV-1a of a word (index tokenisation). */
uint32_t poom_net_audit_word_hash(const char *w, size_t n);

/** @brief Add every alphanumeric word of @p text to a block bit-set. */
void poom_net_audit_bloom_add_text(uint8_t *bits, const char *text);

/** @brief May the block contain ALL words of @p term? (false = surely not) */
bool poom_net_audit_bloom_has_term(const uint8_t *bits, const char *term);

/** @brief Is the index at @p idx_path complete and stamped for this CSV? */
bool poom_net_audit_index_current(const char *csv_path, const char *idx_path);

/**
 * @brief Multi-term search of @p csv_path. Uses the index when current (only
 *        candidate blocks are read); otherwise does a full pass and, when
 *        @p idx_path / @p idx_tmp_path are given, writes a fresh index during
 *        that same pass (temp file, renamed when complete, deleted on cancel).
 *        Results are identical either way. Cancel keeps partial matches and
 *        returns ESP_OK.
 * @return ESP_OK, ESP_ERR_NOT_FOUND (no CSV), ESP_ERR_NO_MEM, else an error.
 */
esp_err_t poom_net_audit_search_file(const char *csv_path, const char *idx_path,
                                     const char *idx_tmp_path,
                                     const poom_net_audit_term_t *terms, uint8_t n_terms,
                                     poom_net_audit_match_t *out, size_t cap,
                                     uint8_t per_term_cap, size_t *out_count,
                                     poom_net_audit_index_use_t *how,
                                     const volatile bool *cancel,
                                     poom_net_audit_progress_cb_t cb, void *user);

/* ---- SD-backed search (ESP-only) ---------------------------------------- */

/**
 * @brief Release what the lookup acquired: unmounts the SD card ONLY if this
 *        module mounted it (a card another app mounted is left alone). Call on
 *        app exit. Safe to call repeatedly.
 */
void poom_net_audit_release(void);

/**
 * @brief Mount the SD if needed (one retry) and check the Exploit-DB CSV.
 * @return ESP_OK (ready), ESP_ERR_INVALID_STATE (SD could not be mounted),
 *         ESP_ERR_NOT_FOUND (mounted, but the CSV is missing).
 */
esp_err_t poom_net_audit_db_status(void);

/**
 * @brief Error from the last failed SD mount attempt (sd_card driver codes:
 *        ESP_ERR_NO_MEM = SPI bus / memory, ESP_ERR_NOT_FOUND = card init,
 *        ESP_ERR_NOT_SUPPORTED = no FAT filesystem), or ESP_OK.
 */
esp_err_t poom_net_audit_last_mount_err(void);

/**
 * @brief Whether the Exploit-DB CSV is present and readable on the SD card
 *        (mounts the card if needed).
 */
bool poom_net_audit_db_present(void);

/**
 * @brief Stream the CSV and collect up to @p cap matches for @p term (usually
 *        the detected product; fall back to the service name). @p version is
 *        not a hard filter (titles are inconsistent) but may be used later for
 *        ranking. Honors @p cancel; reports progress by bytes scanned.
 *
 * @return ESP_OK (even with 0 matches), ESP_ERR_NOT_FOUND (CSV missing),
 *         ESP_ERR_INVALID_STATE (SD not mountable), else an error.
 */
esp_err_t poom_net_audit_search(const char *term, const char *version,
                                poom_net_audit_match_t *out, size_t cap,
                                size_t *out_count, const volatile bool *cancel,
                                poom_net_audit_progress_cb_t cb, void *user);

/**
 * @brief One-pass search over the CSV for MANY terms at once (the efficient
 *        path for per-device and network-wide Assess). Each kept match carries
 *        its @c term_id and @c tier. Retention is bounded by @p cap and
 *        @p per_term_cap (keeps the strongest, balanced across terms).
 *
 * @return ESP_OK (even with 0 matches), ESP_ERR_NOT_FOUND (CSV missing),
 *         ESP_ERR_INVALID_STATE (SD not mountable), else an error.
 */
/**
 * @brief Mount the SD card (lean, one retry) for other files the audit writes
 *        (e.g. reports). Pair with poom_net_audit_release().
 * @return ESP_OK or ESP_ERR_INVALID_STATE (not mountable).
 */
esp_err_t poom_net_audit_sd_open(void);

/** @brief How the last search_multi() used the index (diagnostics). */
poom_net_audit_index_use_t poom_net_audit_last_index_use(void);

esp_err_t poom_net_audit_search_multi(const poom_net_audit_term_t *terms,
                                      uint8_t n_terms,
                                      poom_net_audit_match_t *out, size_t cap,
                                      uint8_t per_term_cap, size_t *out_count,
                                      const volatile bool *cancel,
                                      poom_net_audit_progress_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* POOM_NET_AUDIT_H */
