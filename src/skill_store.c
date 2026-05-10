// ---------------------------------------------------------------------------
//  Skill store implementation. See skill_store.h.
// ---------------------------------------------------------------------------
#include "skill_store.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "cJSON.h"
#include "devcfg.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "skill_store";

#define MOUNT_BASE      "/storage"
#define SKILLS_DIR      "/storage/skills"
#define PARTITION_LABEL "storage"

static bool s_mounted = false;

// In-memory cache so build_system_prompt + ${VAR} substitution can serve
// reads without touching flash. Critical because both can run from speech_task
// (PSRAM-backed stack), and any LittleFS read from a PSRAM stack hits
// `esp_task_stack_is_sane_cache_disabled` and panics. The cache is the
// source of truth for reads; writes go through both the cache and LittleFS.
//
// Each entry holds the FRONTMATTER-STRIPPED body (what the prompt injects)
// and the raw creds JSON string. Both pointers are PSRAM mallocs and may
// be NULL if the corresponding file doesn't exist yet.
#define SKILL_CACHE_MAX 16
typedef struct {
    char    id[SKILL_ID_MAX];
    char   *body_stripped;
    size_t  body_stripped_len;
    char   *creds_json;            // raw JSON: "{\"K\":\"v\",...}" or NULL
} body_cache_entry_t;
static body_cache_entry_t s_cache[SKILL_CACHE_MAX];
static SemaphoreHandle_t  s_cache_mtx = NULL;

static int cache_find(const char *id) {
    for (int i = 0; i < SKILL_CACHE_MAX; ++i) {
        if (s_cache[i].id[0] && strcmp(s_cache[i].id, id) == 0) return i;
    }
    return -1;
}

static int cache_alloc_slot(const char *id) {
    int slot = cache_find(id);
    if (slot >= 0) return slot;
    for (int i = 0; i < SKILL_CACHE_MAX; ++i) {
        if (s_cache[i].id[0] == '\0') {
            strlcpy(s_cache[i].id, id, SKILL_ID_MAX);
            s_cache[i].body_stripped = NULL;
            s_cache[i].body_stripped_len = 0;
            s_cache[i].creds_json = NULL;
            return i;
        }
    }
    return -1;
}

static void cache_lock(void)   { if (s_cache_mtx) xSemaphoreTake(s_cache_mtx, portMAX_DELAY); }
static void cache_unlock(void) { if (s_cache_mtx) xSemaphoreGive(s_cache_mtx); }

static void cache_clear_entry_locked(int idx) {
    if (s_cache[idx].body_stripped) { free(s_cache[idx].body_stripped); s_cache[idx].body_stripped = NULL; }
    if (s_cache[idx].creds_json)    { free(s_cache[idx].creds_json);    s_cache[idx].creds_json = NULL; }
    s_cache[idx].body_stripped_len = 0;
    s_cache[idx].id[0] = '\0';
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Validate skill id: lowercase alnum, hyphen, underscore. 1..32 chars.
static bool valid_id(const char *id) {
    if (!id) return false;
    size_t n = strlen(id);
    if (n == 0 || n >= SKILL_ID_MAX) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = id[i];
        if (!(c == '-' || c == '_' ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

// Validate credential key: ^[A-Z][A-Z0-9_]*$, 1..64 chars.
static bool valid_cred_key(const char *k) {
    if (!k) return false;
    size_t n = strlen(k);
    if (n == 0 || n >= SKILL_CRED_NAME_MAX) return false;
    if (!(k[0] >= 'A' && k[0] <= 'Z')) return false;
    for (size_t i = 1; i < n; ++i) {
        char c = k[i];
        if (!(c == '_' ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

static void path_body(char *out, size_t cap, const char *id) {
    snprintf(out, cap, SKILLS_DIR "/%s.md", id);
}

static void path_creds(char *out, size_t cap, const char *id) {
    snprintf(out, cap, SKILLS_DIR "/%s.creds.json", id);
}

// ---------------------------------------------------------------------------
// Mount
// ---------------------------------------------------------------------------
// Forward decl: scan_into_cache uses skill_store_read_body_stripped after
// init flips s_mounted, but we cache while the function is also used by
// callers that bypass the cache. Internal helper just for boot scan.
static int read_body_stripped_uncached(const char *id, char *out, size_t out_cap);
static int read_creds_uncached(const char *id, char *out, size_t out_cap);

static void cache_set_body_locked(const char *id, const char *body_stripped, size_t len);
static void cache_set_creds_locked(const char *id, const char *json);

static void scan_into_cache(void) {
    DIR *d = opendir(SKILLS_DIR);
    if (!d) return;
    struct dirent *de;
    cache_lock();
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        const char *dot = strrchr(de->d_name, '.');
        if (!dot) continue;
        // Only seed body cache here; creds JSON is small and loaded on demand.
        if (strcmp(dot, ".md") != 0) continue;
        char id[SKILL_ID_MAX];
        size_t id_len = (size_t)(dot - de->d_name);
        if (id_len == 0 || id_len >= SKILL_ID_MAX) continue;
        memcpy(id, de->d_name, id_len);
        id[id_len] = '\0';

        // Read stripped body into a stretchy buffer. Bodies are <= 64 KB.
        char *buf = malloc(SKILL_BODY_MAX + 1);
        if (!buf) continue;
        int n = read_body_stripped_uncached(id, buf, SKILL_BODY_MAX + 1);
        if (n > 0) cache_set_body_locked(id, buf, (size_t)n);
        free(buf);

        // Pre-load creds for the same id if present.
        char *cbuf = malloc(SKILL_CREDS_FILE_MAX + 1);
        if (cbuf) {
            int cn = read_creds_uncached(id, cbuf, SKILL_CREDS_FILE_MAX + 1);
            if (cn >= 0) cache_set_creds_locked(id, cbuf);
            free(cbuf);
        }
    }
    cache_unlock();
    closedir(d);
}

esp_err_t skill_store_init(void) {
    if (s_mounted) return ESP_OK;

    esp_vfs_littlefs_conf_t conf = {
        .base_path        = MOUNT_BASE,
        .partition_label  = PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount       = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs register failed: %s", esp_err_to_name(err));
        return err;
    }

    // Make the skills/ directory if it doesn't exist (mkdir is idempotent
    // returning -1 + EEXIST otherwise). LittleFS supports mkdir via VFS.
    if (mkdir(SKILLS_DIR, 0775) < 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: %s", SKILLS_DIR, strerror(errno));
    }

    if (!s_cache_mtx) s_cache_mtx = xSemaphoreCreateMutex();
    s_mounted = true;

    // Pre-warm the in-memory cache from LittleFS. Done here on the main task
    // (internal RAM stack, safe for flash I/O) so that subsequent reads from
    // PSRAM-stack tasks (speech_task etc.) never have to touch flash.
    scan_into_cache();

    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "littlefs mounted at %s (used=%u/%u, cache populated)",
                 MOUNT_BASE, (unsigned)used, (unsigned)total);
    }
    return ESP_OK;
}

static void cache_set_body_locked(const char *id, const char *body_stripped, size_t len) {
    int slot = cache_alloc_slot(id);
    if (slot < 0) return;
    if (s_cache[slot].body_stripped) free(s_cache[slot].body_stripped);
    s_cache[slot].body_stripped = malloc(len + 1);
    if (s_cache[slot].body_stripped) {
        memcpy(s_cache[slot].body_stripped, body_stripped, len);
        s_cache[slot].body_stripped[len] = '\0';
        s_cache[slot].body_stripped_len = len;
    } else {
        s_cache[slot].body_stripped_len = 0;
    }
}

static void cache_set_creds_locked(const char *id, const char *json) {
    int slot = cache_alloc_slot(id);
    if (slot < 0) return;
    if (s_cache[slot].creds_json) { free(s_cache[slot].creds_json); s_cache[slot].creds_json = NULL; }
    if (json && json[0]) {
        size_t n = strlen(json);
        s_cache[slot].creds_json = malloc(n + 1);
        if (s_cache[slot].creds_json) memcpy(s_cache[slot].creds_json, json, n + 1);
    }
}

// Strip frontmatter from a NUL-terminated markdown buffer in place.
// Returns new length (excluding NUL).
static size_t strip_frontmatter_inplace(char *buf) {
    if (!buf) return 0;
    if (!(buf[0] == '-' && buf[1] == '-' && buf[2] == '-')) return strlen(buf);
    char *p = buf + 4;
    while (*p) {
        if (p[0] == '\n' && p[1] == '-' && p[2] == '-' && p[3] == '-') {
            char *q = p + 4;
            while (*q == ' ' || *q == '\t' || *q == '\r') ++q;
            if (*q == '\n' || *q == '\0') {
                if (*q == '\n') ++q;
                size_t kept = strlen(q);
                memmove(buf, q, kept + 1);
                return kept;
            }
        }
        ++p;
    }
    return strlen(buf);
}

// ---------------------------------------------------------------------------
// Frontmatter parser. Tiny YAML-lite: flat key:value pairs, plus a
// `credentials:` list with `- ITEM` entries.
// ---------------------------------------------------------------------------

// Strip leading and trailing ASCII whitespace in-place. Returns the length.
static size_t lstrip_rstrip(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n')) {
        s[--n] = '\0';
    }
    return n;
}

// Returns offset just past the next "\n", or md_len if not found.
static size_t next_line(const char *md, size_t md_len, size_t off) {
    while (off < md_len && md[off] != '\n') ++off;
    if (off < md_len) ++off;  // step over '\n'
    return off;
}

static bool line_is_three_dashes(const char *line) {
    // Accepts "---" optionally followed by whitespace.
    if (line[0] != '-' || line[1] != '-' || line[2] != '-') return false;
    for (size_t i = 3; line[i]; ++i) {
        if (line[i] != ' ' && line[i] != '\t' &&
            line[i] != '\r' && line[i] != '\n') return false;
    }
    return true;
}

int skill_store_parse_frontmatter(const char *md, size_t md_len,
                                  skill_meta_t *meta,
                                  size_t *body_offset) {
    if (!md || !meta || !body_offset) return -4;
    memset(meta, 0, sizeof(*meta));

    // Must start with "---\n" (or "---\r\n").
    if (md_len < 4) return -1;
    if (!(md[0] == '-' && md[1] == '-' && md[2] == '-' &&
          (md[3] == '\n' || md[3] == '\r'))) {
        return -1;
    }

    size_t off = next_line(md, md_len, 0);
    bool   in_credentials_list = false;

    while (off < md_len) {
        // Read one line into a stack buffer.
        size_t end = off;
        while (end < md_len && md[end] != '\n') ++end;
        size_t line_len = end - off;
        if (line_len > 511) line_len = 511;

        char line[512];
        memcpy(line, md + off, line_len);
        line[line_len] = '\0';
        // Strip a trailing CR for Windows-style line endings.
        if (line_len > 0 && line[line_len-1] == '\r') line[line_len-1] = '\0';

        size_t next_off = end < md_len ? end + 1 : md_len;

        // Closing delimiter?
        if (line_is_three_dashes(line)) {
            *body_offset = next_off;
            // Validate required fields after the closing delimiter.
            if (meta->id[0] == '\0' || meta->description[0] == '\0') return -2;
            return 0;
        }

        // List continuation under `credentials:`.
        if (in_credentials_list) {
            // Lines starting with "  - " or "- " are list items; trim leading
            // whitespace and look for a leading dash.
            char *p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '-') {
                ++p;  // skip dash
                while (*p == ' ' || *p == '\t') ++p;
                if (*p) {
                    if (meta->credentials_count < SKILL_CREDS_LIST_MAX) {
                        strncpy(meta->credentials[meta->credentials_count],
                                p, SKILL_CRED_NAME_MAX - 1);
                        meta->credentials[meta->credentials_count]
                            [SKILL_CRED_NAME_MAX - 1] = '\0';
                        // Strip any trailing whitespace.
                        size_t cn = strlen(meta->credentials[meta->credentials_count]);
                        while (cn > 0 &&
                               (meta->credentials[meta->credentials_count][cn-1] == ' ' ||
                                meta->credentials[meta->credentials_count][cn-1] == '\t')) {
                            meta->credentials[meta->credentials_count][--cn] = '\0';
                        }
                        meta->credentials_count++;
                    }
                }
                off = next_off;
                continue;
            }
            // Anything else ends the list.
            in_credentials_list = false;
        }

        // Trim and skip blank lines / comments.
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;
        if (*trimmed == '\0' || *trimmed == '#') {
            off = next_off;
            continue;
        }

        // key: value (or key:)
        char *colon = strchr(trimmed, ':');
        if (!colon) {
            off = next_off;
            continue;  // tolerate weird lines
        }
        *colon = '\0';
        char *key = trimmed;
        char *val = colon + 1;
        // Trim key trailing whitespace and val leading whitespace.
        size_t kl = strlen(key);
        while (kl > 0 && (key[kl-1] == ' ' || key[kl-1] == '\t')) key[--kl] = '\0';
        while (*val == ' ' || *val == '\t') ++val;
        size_t vl = lstrip_rstrip(val);

        if (strcmp(key, "name") == 0) {
            if (vl == 0 || vl >= SKILL_ID_MAX) return -3;
            // Validate id charset; if invalid we still accept here so the
            // server-layer validator can return a precise error message.
            strncpy(meta->id, val, SKILL_ID_MAX - 1);
            meta->id[SKILL_ID_MAX - 1] = '\0';
            // Default `name` to id; can be overridden by an explicit
            // human-name field below.
            if (meta->name[0] == '\0') {
                strncpy(meta->name, val, SKILL_NAME_MAX - 1);
                meta->name[SKILL_NAME_MAX - 1] = '\0';
            }
        } else if (strcmp(key, "description") == 0) {
            if (vl == 0 || vl >= SKILL_DESC_MAX) return -3;
            strncpy(meta->description, val, SKILL_DESC_MAX - 1);
            meta->description[SKILL_DESC_MAX - 1] = '\0';
        } else if (strcmp(key, "version") == 0) {
            if (vl >= SKILL_VERSION_MAX) return -3;
            strncpy(meta->version, val, SKILL_VERSION_MAX - 1);
            meta->version[SKILL_VERSION_MAX - 1] = '\0';
        } else if (strcmp(key, "credentials") == 0) {
            in_credentials_list = true;
        }
        // Other keys (metadata.*, tags, etc.) are ignored.

        off = next_off;
    }

    return -1;  // ran out of input without seeing closing "---"
}

// ---------------------------------------------------------------------------
// Body file I/O
// ---------------------------------------------------------------------------
esp_err_t skill_store_write_body(const char *id,
                                 const char *body, size_t body_len) {
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!valid_id(id))   return ESP_ERR_INVALID_ARG;
    if (!body)           return ESP_ERR_INVALID_ARG;
    if (body_len > SKILL_BODY_MAX) return ESP_ERR_INVALID_SIZE;

    // Enforce global storage cap. We compute usage assuming this write
    // replaces any existing body for the same id.
    char path[64];
    path_body(path, sizeof(path), id);
    struct stat st;
    size_t replacing = (stat(path, &st) == 0) ? (size_t)st.st_size : 0;
    size_t used = skill_store_used_bytes();
    if (used > replacing) used -= replacing;
    if (used + body_len > SKILL_TOTAL_BYTES_CAP) {
        ESP_LOGW(TAG, "write rejected: would push storage past cap (%u + %u > %u)",
                 (unsigned)used, (unsigned)body_len,
                 (unsigned)SKILL_TOTAL_BYTES_CAP);
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    size_t w = fwrite(body, 1, body_len, f);
    fclose(f);
    if (w != body_len) {
        ESP_LOGE(TAG, "short write on %s (%u/%u)",
                 path, (unsigned)w, (unsigned)body_len);
        return ESP_FAIL;
    }

    // Refresh cache: copy body into a scratch buffer, strip frontmatter,
    // store. Without this the prompt-injection path would still hit flash
    // for this skill on the next /say.
    char *scratch = malloc(body_len + 1);
    if (scratch) {
        memcpy(scratch, body, body_len);
        scratch[body_len] = '\0';
        size_t stripped = strip_frontmatter_inplace(scratch);
        cache_lock();
        cache_set_body_locked(id, scratch, stripped);
        cache_unlock();
        free(scratch);
    }

    ESP_LOGI(TAG, "wrote skill body %s (%u bytes)", id, (unsigned)body_len);
    return ESP_OK;
}

bool skill_store_body_exists(const char *id) {
    if (!s_mounted || !valid_id(id)) return false;
    char path[64];
    path_body(path, sizeof(path), id);
    struct stat st;
    return stat(path, &st) == 0;
}

// Internal: bypasses cache. Used only by the boot scan from
// skill_store_init (main task, internal RAM stack — safe for flash I/O).
static int read_body_uncached(const char *id, char *out, size_t out_cap) {
    if (!s_mounted || !valid_id(id) || !out || out_cap == 0) return -1;
    char path[64];
    path_body(path, sizeof(path), id);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(out, 1, out_cap - 1, f);
    fclose(f);
    out[r] = '\0';
    return (int)r;
}

static int read_body_stripped_uncached(const char *id, char *out, size_t out_cap) {
    int n = read_body_uncached(id, out, out_cap);
    if (n <= 0) return n;
    return (int)strip_frontmatter_inplace(out);
}

int skill_store_read_body(const char *id, char *out, size_t out_cap) {
    // Public read used by the web UI's "view raw" path. Safe from httpd task.
    return read_body_uncached(id, out, out_cap);
}

int skill_store_read_body_stripped(const char *id, char *out, size_t out_cap) {
    if (!valid_id(id) || !out || out_cap == 0) return -1;
    cache_lock();
    int slot = cache_find(id);
    if (slot >= 0 && s_cache[slot].body_stripped) {
        size_t n = s_cache[slot].body_stripped_len;
        if (n >= out_cap) n = out_cap - 1;
        memcpy(out, s_cache[slot].body_stripped, n);
        out[n] = '\0';
        cache_unlock();
        return (int)n;
    }
    cache_unlock();
    // Fallback: not in cache. Only safe to hit flash from internal-RAM tasks
    // (httpd, main). Speech_task path will get an empty body instead of
    // crashing — markdown will be missing for one prompt and recover on the
    // next request once the cache fills.
    return read_body_stripped_uncached(id, out, out_cap);
}

esp_err_t skill_store_delete(const char *id) {
    if (!s_mounted || !valid_id(id)) return ESP_ERR_INVALID_ARG;
    char path[64];
    path_body(path, sizeof(path), id);
    if (unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "unlink %s failed: %s", path, strerror(errno));
    }
    path_creds(path, sizeof(path), id);
    if (unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "unlink %s failed: %s", path, strerror(errno));
    }
    cache_lock();
    int slot = cache_find(id);
    if (slot >= 0) cache_clear_entry_locked(slot);
    cache_unlock();
    return ESP_OK;
}

size_t skill_store_used_bytes(void) {
    if (!s_mounted) return 0;
    DIR *d = opendir(SKILLS_DIR);
    if (!d) return 0;
    size_t total = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[320];
        snprintf(path, sizeof(path), SKILLS_DIR "/%s", de->d_name);
        struct stat st;
        if (stat(path, &st) == 0) total += (size_t)st.st_size;
    }
    closedir(d);
    return total;
}

// ---------------------------------------------------------------------------
// Credentials (per-skill JSON file)
// ---------------------------------------------------------------------------

// Read whole creds file into newly-malloced buffer (NUL-terminated). Returns
// NULL if missing. Caller frees. CACHE-AWARE: serves from in-memory cache
// when populated so PSRAM-stack tasks (speech_task) never touch flash.
static char *creds_read(const char *id) {
    cache_lock();
    int slot = cache_find(id);
    if (slot >= 0 && s_cache[slot].creds_json) {
        size_t n = strlen(s_cache[slot].creds_json);
        char *copy = malloc(n + 1);
        if (copy) memcpy(copy, s_cache[slot].creds_json, n + 1);
        cache_unlock();
        return copy;
    }
    cache_unlock();
    // Fallback to LittleFS — only safe from internal-RAM stacks (httpd/main).
    char path[64];
    path_creds(path, sizeof(path), id);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || sz > SKILL_CREDS_FILE_MAX) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[r] = '\0';
    return buf;
}

// Internal: bypasses the cache. Used by skill_store_init's boot scan only.
static int read_creds_uncached(const char *id, char *out, size_t out_cap) {
    if (!s_mounted || !valid_id(id) || !out || out_cap == 0) return -1;
    char path[64];
    path_creds(path, sizeof(path), id);
    FILE *f = fopen(path, "rb");
    if (!f) { out[0] = '\0'; return 0; }   // missing creds is fine
    size_t r = fread(out, 1, out_cap - 1, f);
    fclose(f);
    out[r] = '\0';
    return (int)r;
}

static esp_err_t creds_write_string(const char *id, const char *json_str) {
    char path[64];
    path_creds(path, sizeof(path), id);
    if (json_str == NULL || json_str[0] == '\0') {
        if (unlink(path) != 0 && errno != ENOENT) return ESP_FAIL;
        cache_lock();
        cache_set_creds_locked(id, NULL);
        cache_unlock();
        return ESP_OK;
    }
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t n = strlen(json_str);
    size_t w = fwrite(json_str, 1, n, f);
    fclose(f);
    if (w != n) return ESP_FAIL;
    cache_lock();
    cache_set_creds_locked(id, json_str);
    cache_unlock();
    return ESP_OK;
}

int skill_store_get_cred(const char *skill_id, const char *key,
                         char *out, size_t out_cap) {
    if (!valid_id(skill_id) || !valid_cred_key(key) || !out || out_cap == 0) {
        return -1;
    }
    out[0] = '\0';
    char *json = creds_read(skill_id);
    if (!json) return 0;
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    int rv = 0;
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        size_t n = strlen(v->valuestring);
        if (n >= out_cap) n = out_cap - 1;
        memcpy(out, v->valuestring, n);
        out[n] = '\0';
        rv = (int)n;
    }
    cJSON_Delete(root);
    return rv;
}

esp_err_t skill_store_set_cred(const char *skill_id,
                               const char *key, const char *value) {
    if (!valid_id(skill_id) || !valid_cred_key(key)) return ESP_ERR_INVALID_ARG;

    char *json = creds_read(skill_id);
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (json) free(json);
    if (!root) root = cJSON_CreateObject();

    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    if (value && value[0]) {
        if (strlen(value) >= SKILL_CRED_VALUE_MAX) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_SIZE;
        }
        cJSON_AddStringToObject(root, key, value);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return ESP_ERR_NO_MEM;
    esp_err_t rc = creds_write_string(skill_id, out);
    free(out);
    return rc;
}

esp_err_t skill_store_set_creds_json(const char *skill_id, const char *json) {
    if (!valid_id(skill_id) || !json) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Build a fresh object containing only valid string entries with non-
    // empty values. Empties / non-strings are dropped (deletes the key).
    cJSON *clean = cJSON_CreateObject();
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, root) {
        if (!it->string) continue;
        if (!valid_cred_key(it->string)) continue;
        if (!cJSON_IsString(it) || !it->valuestring) continue;
        if (it->valuestring[0] == '\0') continue;
        if (strlen(it->valuestring) >= SKILL_CRED_VALUE_MAX) continue;
        cJSON_AddStringToObject(clean, it->string, it->valuestring);
    }
    cJSON_Delete(root);

    char *out = cJSON_PrintUnformatted(clean);
    cJSON_Delete(clean);
    if (!out) return ESP_ERR_NO_MEM;

    // Empty object means delete file.
    bool empty = (strcmp(out, "{}") == 0);
    esp_err_t rc = empty ? creds_write_string(skill_id, NULL)
                         : creds_write_string(skill_id, out);
    free(out);
    return rc;
}

int skill_store_set_creds_keys(const char *skill_id,
                               char *out, size_t out_cap) {
    if (!valid_id(skill_id) || !out || out_cap < 3) return -1;
    char *json = creds_read(skill_id);
    if (!json) {
        strlcpy(out, "[]", out_cap);
        return (int)strlen(out);
    }
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        strlcpy(out, "[]", out_cap);
        return (int)strlen(out);
    }
    cJSON *arr = cJSON_CreateArray();
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, root) {
        if (it->string && cJSON_IsString(it) && it->valuestring &&
            it->valuestring[0]) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(it->string));
        }
    }
    cJSON_Delete(root);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s) {
        strlcpy(out, "[]", out_cap);
        return (int)strlen(out);
    }
    strlcpy(out, s, out_cap);
    free(s);
    return (int)strlen(out);
}

// ---------------------------------------------------------------------------
// Registry: integrate with devcfg's svc_custom JSON array
// ---------------------------------------------------------------------------

// Build a cJSON object representing the markdown-kind service entry.
static cJSON *meta_to_json(const skill_meta_t *m) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "id",          m->id);
    cJSON_AddStringToObject(o, "name",        m->name[0] ? m->name : m->id);
    cJSON_AddStringToObject(o, "description", m->description);
    cJSON_AddStringToObject(o, "kind",        "markdown");
    if (m->version[0]) {
        cJSON_AddStringToObject(o, "version", m->version);
    }
    cJSON *creds = cJSON_AddArrayToObject(o, "credentials");
    for (int i = 0; i < m->credentials_count; ++i) {
        cJSON_AddItemToArray(creds, cJSON_CreateString(m->credentials[i]));
    }
    return o;
}

// Locate the index of an entry with matching id in `services` (a JSON array).
// Returns -1 if not found.
static int find_service_index(const cJSON *services, const char *id) {
    if (!services || !id) return -1;
    int idx = 0;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, services) {
        const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(e, "id");
        if (cJSON_IsString(id_j) && id_j->valuestring &&
            strcmp(id_j->valuestring, id) == 0) {
            return idx;
        }
        ++idx;
    }
    return -1;
}

esp_err_t skill_store_register(const skill_meta_t *meta) {
    if (!meta || !valid_id(meta->id) || !meta->description[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *services = cJSON_Parse(devcfg_custom_services());
    if (!services || !cJSON_IsArray(services)) {
        if (services) cJSON_Delete(services);
        services = cJSON_CreateArray();
        if (!services) return ESP_ERR_NO_MEM;
    }

    int idx = find_service_index(services, meta->id);
    cJSON *entry = meta_to_json(meta);
    if (!entry) {
        cJSON_Delete(services);
        return ESP_ERR_NO_MEM;
    }
    if (idx >= 0) {
        cJSON_ReplaceItemInArray(services, idx, entry);
    } else {
        cJSON_AddItemToArray(services, entry);
    }

    char *out = cJSON_PrintUnformatted(services);
    cJSON_Delete(services);
    if (!out) return ESP_ERR_NO_MEM;
    devcfg_set_custom_services(out);
    cJSON_free(out);
    return ESP_OK;
}

esp_err_t skill_store_unregister(const char *id) {
    if (!valid_id(id)) return ESP_ERR_INVALID_ARG;

    // Drop from svc_custom.
    cJSON *services = cJSON_Parse(devcfg_custom_services());
    if (services && cJSON_IsArray(services)) {
        int idx = find_service_index(services, id);
        if (idx >= 0) {
            cJSON_DeleteItemFromArray(services, idx);
            char *out = cJSON_PrintUnformatted(services);
            if (out) {
                devcfg_set_custom_services(out);
                cJSON_free(out);
            }
        }
    }
    if (services) cJSON_Delete(services);

    // Drop from svc_enabled.
    cJSON *enabled = cJSON_Parse(devcfg_services_enabled());
    if (enabled && cJSON_IsArray(enabled)) {
        cJSON *fresh = cJSON_CreateArray();
        const cJSON *e = NULL;
        bool changed = false;
        cJSON_ArrayForEach(e, enabled) {
            if (cJSON_IsString(e) && e->valuestring &&
                strcmp(e->valuestring, id) == 0) {
                changed = true;
                continue;
            }
            if (cJSON_IsString(e) && e->valuestring) {
                cJSON_AddItemToArray(fresh, cJSON_CreateString(e->valuestring));
            }
        }
        if (changed) {
            char *out = cJSON_PrintUnformatted(fresh);
            if (out) {
                devcfg_set_services_enabled(out);
                cJSON_free(out);
            }
        }
        cJSON_Delete(fresh);
    }
    if (enabled) cJSON_Delete(enabled);

    return ESP_OK;
}

// Helper: build a set-like lookup of enabled-id strings.
typedef struct {
    char ids[16][SKILL_ID_MAX];
    int  count;
} id_set_t;

static void load_enabled_set(id_set_t *s) {
    s->count = 0;
    cJSON *enabled = cJSON_Parse(devcfg_services_enabled());
    if (!enabled || !cJSON_IsArray(enabled)) {
        if (enabled) cJSON_Delete(enabled);
        return;
    }
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, enabled) {
        if (s->count >= 16) break;
        if (!cJSON_IsString(e) || !e->valuestring) continue;
        size_t n = strlen(e->valuestring);
        if (n >= SKILL_ID_MAX) continue;
        memcpy(s->ids[s->count], e->valuestring, n + 1);
        s->count++;
    }
    cJSON_Delete(enabled);
}

static bool id_set_contains(const id_set_t *s, const char *id) {
    for (int i = 0; i < s->count; ++i) {
        if (strcmp(s->ids[i], id) == 0) return true;
    }
    return false;
}

static int list_markdown(char out_ids[][SKILL_ID_MAX], int max,
                         bool only_enabled) {
    if (!out_ids || max <= 0) return 0;
    cJSON *services = cJSON_Parse(devcfg_custom_services());
    if (!services || !cJSON_IsArray(services)) {
        if (services) cJSON_Delete(services);
        return 0;
    }
    id_set_t enabled = {0};
    if (only_enabled) load_enabled_set(&enabled);

    int count = 0;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, services) {
        if (count >= max) break;
        const cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(e, "kind");
        if (!cJSON_IsString(kind_j) || !kind_j->valuestring ||
            strcmp(kind_j->valuestring, "markdown") != 0) continue;

        const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(e, "id");
        if (!cJSON_IsString(id_j) || !id_j->valuestring) continue;
        size_t n = strlen(id_j->valuestring);
        if (n >= SKILL_ID_MAX) continue;

        if (only_enabled && !id_set_contains(&enabled, id_j->valuestring)) continue;

        memcpy(out_ids[count], id_j->valuestring, n + 1);
        ++count;
    }
    cJSON_Delete(services);
    return count;
}

int skill_store_list_enabled(char out_ids[][SKILL_ID_MAX], int max) {
    return list_markdown(out_ids, max, true);
}

int skill_store_list_all(char out_ids[][SKILL_ID_MAX], int max) {
    return list_markdown(out_ids, max, false);
}

bool skill_store_any_enabled_markdown(void) {
    char ids[1][SKILL_ID_MAX];
    return skill_store_list_enabled(ids, 1) > 0;
}

bool skill_store_lookup(const char *id, skill_meta_t *out_meta) {
    if (!valid_id(id) || !out_meta) return false;
    memset(out_meta, 0, sizeof(*out_meta));

    cJSON *services = cJSON_Parse(devcfg_custom_services());
    if (!services || !cJSON_IsArray(services)) {
        if (services) cJSON_Delete(services);
        return false;
    }
    bool found = false;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, services) {
        const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(e, "id");
        if (!cJSON_IsString(id_j) || !id_j->valuestring) continue;
        if (strcmp(id_j->valuestring, id) != 0) continue;
        const cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(e, "kind");
        if (!cJSON_IsString(kind_j) || !kind_j->valuestring ||
            strcmp(kind_j->valuestring, "markdown") != 0) continue;

        strlcpy(out_meta->id, id_j->valuestring, SKILL_ID_MAX);

        const cJSON *name_j = cJSON_GetObjectItemCaseSensitive(e, "name");
        if (cJSON_IsString(name_j) && name_j->valuestring) {
            strlcpy(out_meta->name, name_j->valuestring, SKILL_NAME_MAX);
        }
        const cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(e, "description");
        if (cJSON_IsString(desc_j) && desc_j->valuestring) {
            strlcpy(out_meta->description, desc_j->valuestring, SKILL_DESC_MAX);
        }
        const cJSON *ver_j = cJSON_GetObjectItemCaseSensitive(e, "version");
        if (cJSON_IsString(ver_j) && ver_j->valuestring) {
            strlcpy(out_meta->version, ver_j->valuestring, SKILL_VERSION_MAX);
        }
        const cJSON *creds_j = cJSON_GetObjectItemCaseSensitive(e, "credentials");
        if (cJSON_IsArray(creds_j)) {
            const cJSON *c = NULL;
            cJSON_ArrayForEach(c, creds_j) {
                if (out_meta->credentials_count >= SKILL_CREDS_LIST_MAX) break;
                if (!cJSON_IsString(c) || !c->valuestring) continue;
                strlcpy(out_meta->credentials[out_meta->credentials_count],
                        c->valuestring, SKILL_CRED_NAME_MAX);
                out_meta->credentials_count++;
            }
        }
        found = true;
        break;
    }
    cJSON_Delete(services);
    return found;
}
