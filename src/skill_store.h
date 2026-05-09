// ---------------------------------------------------------------------------
//  Skill store — LittleFS-backed storage for user-uploaded skill markdown
//  bodies and per-skill credentials.
//
//  A skill is a markdown document with YAML-lite frontmatter:
//      ---
//      name: sp3nd
//      description: Buy from Amazon & eBay with USDC
//      version: 1.7.0
//      credentials:
//        - SP3ND_API_KEY
//        - SP3ND_API_SECRET
//      ---
//      <body...>
//
//  Lightweight metadata (id, description, declared credential names) is
//  duplicated into the existing svc_custom NVS blob as a markdown-kind
//  service entry; the actual markdown body lives at
//      /storage/skills/<id>.md
//  and per-skill credentials at
//      /storage/skills/<id>.creds.json   ({"KEY":"value", ...})
//  on the LittleFS-mounted `storage` partition.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SKILL_ID_MAX           33    // 32 chars + NUL
#define SKILL_NAME_MAX         64
#define SKILL_DESC_MAX         256
#define SKILL_VERSION_MAX      32
#define SKILL_CRED_NAME_MAX    65    // 64 chars + NUL
#define SKILL_CRED_VALUE_MAX   257   // 256 chars + NUL
#define SKILL_CREDS_LIST_MAX   8     // max declared credentials per skill
#define SKILL_BODY_MAX         (64 * 1024)
#define SKILL_CREDS_FILE_MAX   (4 * 1024)
#define SKILL_TOTAL_BYTES_CAP  (1 * 1024 * 1024)  // 1 MB across all skills

typedef struct {
    char id[SKILL_ID_MAX];                                       // matches `name:` frontmatter field
    char name[SKILL_NAME_MAX];                                   // human-readable, defaults to id
    char description[SKILL_DESC_MAX];
    char version[SKILL_VERSION_MAX];
    char credentials[SKILL_CREDS_LIST_MAX][SKILL_CRED_NAME_MAX]; // declared credential names
    int  credentials_count;
} skill_meta_t;

// Mount LittleFS at /storage. Idempotent.
esp_err_t skill_store_init(void);

// Parse frontmatter from raw markdown. On success populates *meta and writes
// the body start byte offset to *body_offset (offset of first byte after the
// closing `---` line, with leading newlines preserved).
//
// Returns 0 on success. Negative errno-ish codes on failure:
//   -1  no frontmatter delimiters found
//   -2  required field missing (name, description)
//   -3  field too long
//   -4  malformed
int skill_store_parse_frontmatter(const char *md, size_t md_len,
                                  skill_meta_t *meta,
                                  size_t *body_offset);

// Persist a skill markdown body (entire markdown including frontmatter) to
// /storage/skills/<id>.md. Overwrites if the file exists. Returns ESP_OK or
// an error if the new size would exceed the global cap.
esp_err_t skill_store_write_body(const char *id,
                                 const char *body, size_t body_len);

// Read the full markdown body (with frontmatter) into `out`. NUL-terminated.
// Returns bytes written excluding NUL, or -1 if the skill body file is
// missing / unreadable.
int skill_store_read_body(const char *id, char *out, size_t out_cap);

// Read just the body (everything AFTER the closing `---` of the frontmatter)
// for AI prompt injection. NUL-terminated. Returns bytes written or -1.
int skill_store_read_body_stripped(const char *id, char *out, size_t out_cap);

// Returns true if the body file exists for this skill.
bool skill_store_body_exists(const char *id);

// Delete body + credentials for a skill.
esp_err_t skill_store_delete(const char *id);

// Sum of bytes across all files under /storage/skills.
size_t skill_store_used_bytes(void);

// --- Credentials ----------------------------------------------------------

// Get one credential value. Writes NUL-terminated string to `out`.
// Returns bytes written, 0 if unset, or -1 on read error.
int skill_store_get_cred(const char *skill_id, const char *key,
                         char *out, size_t out_cap);

// Set one credential. Pass value=NULL or "" to delete the key.
esp_err_t skill_store_set_cred(const char *skill_id,
                               const char *key, const char *value);

// Bulk set: replace all credentials with the contents of `json` (a JSON
// object of {"KEY":"value", ...} shape). Keys not in `json` are removed.
// Keys whose value is "" are also removed.
esp_err_t skill_store_set_creds_json(const char *skill_id, const char *json);

// Writes a JSON array of credential names that currently have values set.
// Writes "[]" if no credentials are set or the file doesn't exist. Returns
// bytes written excluding NUL, or -1 on error.
int skill_store_set_creds_keys(const char *skill_id,
                               char *out, size_t out_cap);

// --- Registry (devcfg svc_custom integration) ----------------------------

// Insert (or replace, by id) a markdown-kind entry into devcfg's svc_custom
// JSON array. The entry shape is:
//   {"id":"...","name":"...","description":"...","kind":"markdown",
//    "version":"...","credentials":["A","B",...]}
// Existing structured services (those without `kind` or with `kind` != "markdown")
// are preserved. Returns ESP_OK or an error if the resulting JSON would
// exceed the devcfg svc_custom cap.
esp_err_t skill_store_register(const skill_meta_t *meta);

// Remove a service entry by id from svc_custom AND svc_enabled. Safe to
// call on an id that isn't present.
esp_err_t skill_store_unregister(const char *id);

// True if any markdown-kind service entry exists in svc_custom that is
// also listed in svc_enabled. Used by ai.c to decide whether to register
// the generic skill tools.
bool skill_store_any_enabled_markdown(void);

// Iterator over enabled markdown-kind skills. The id pointer remains valid
// only until the next devcfg_set_custom_services() / svc_enabled() call —
// callers should copy if they need to keep it. Returns the number of ids
// written to `out_ids`.
int skill_store_list_enabled(char out_ids[][SKILL_ID_MAX], int max);

// Same, but lists ALL markdown-kind skills regardless of enabled state.
int skill_store_list_all(char out_ids[][SKILL_ID_MAX], int max);

// Look up a markdown-kind service entry's metadata from svc_custom by id.
// Returns true on hit. Does not read from LittleFS; use skill_store_read_*
// to fetch the body.
bool skill_store_lookup(const char *id, skill_meta_t *out_meta);

#ifdef __cplusplus
}
#endif
