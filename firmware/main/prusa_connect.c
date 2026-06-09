/* Prusa-Touch — Prusa Connect (OAuth2 + API). */
#include "prusa_connect.h"
#include "printer_store.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "esp_random.h"
#include "esp_heap_caps.h"   /* buffer response bodies in PSRAM, not scarce internal RAM */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "connect";

/* Serializes all cloud HTTP so the net-task poll and web-handler calls never run
 * concurrent TLS sessions to Prusa (which intermittently failed with http=0). */
static SemaphoreHandle_t s_http_mtx;
/* Serializes token refresh so two tasks (net poll + web handler) can't both POST the same
 * single-use refresh token — the 2nd would get 400 and wipe the session the 1st just renewed. */
static SemaphoreHandle_t s_refresh_mtx;
static SemaphoreHandle_t s_login_mtx;   /* serializes the multi-step OAuth login flow (net-task auto-reauth vs web login) */

#define PRUSA_AUTH_URL     "https://account.prusa3d.com/o/authorize/"
#define PRUSA_TOKEN_URL    "https://account.prusa3d.com/o/token/"
#define PRUSA_LOGIN_URL    "https://account.prusa3d.com/login/"
#define PRUSA_ACCOUNT_URL  "https://account.prusa3d.com"
#define PRUSA_CLIENT_ID    "MRHTlZhZqkNrrQ6FUPtjyusAz8nc59ErHXP8XkS4"
#define PRUSA_REDIRECT_URI "https://connect.prusa3d.com/login/auth-callback"
#define PRUSA_MOBILE_API   "https://connect-mobile-api.prusa3d.com"
#define PRUSA_USER_AGENT   "PrusaTouch/0.3.3"

/* NVS keys */
#define NS                 "pp"
#define KEY_AT             "conn_at"   /* access token  */
#define KEY_RT             "conn_rt"   /* refresh token */
#define KEY_TEAM           "conn_tid"  /* default team id */
#define KEY_ORG            "conn_org"  /* farm organization UUID */
#define KEY_EMAIL          "conn_em"   /* saved account email (auto re-auth) */
#define KEY_PASS           "conn_pw"   /* saved account password (auto re-auth; opt-in) */

/* Internal state */
static char s_at[2048];
static char s_rt[256];
static char s_team[64];
static char s_org[48];
static char s_pkce_verifier[128];
static char s_pkce_challenge[128];
static char s_cookies[4096];
static char s_csrf[128];
static char s_next[256];
static char s_totp_url[128];
/* Saved account credentials for automatic re-authentication (opt-in; stored in NVS).
 * When the OAuth refresh chain dies, the net task replays the login flow with these so the
 * cloud session restores with no user action. NB: NVS is not encrypted — a flash dump exposes
 * these (as it already does the refresh token). 2FA accounts can't auto-complete (need a TOTP). */
static char s_saved_email[128];
static char s_saved_pass[128];
static bool s_remember = true;   /* persist creds on successful login (device-owner opt-in) */

/* ---- response accumulator (heap) ---- */
typedef struct {
    char  *buf;
    int    len;
    int    cap;
    char   location[512];   /* captured Location response header (redirect target) */
} resp_t;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    resp_t *r = (resp_t *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        if (!r) return ESP_OK;
        int need = r->len + e->data_len + 1;
        if (need > 512 * 1024) return ESP_ERR_NO_MEM;
        if (need > r->cap) {
            int nc = r->cap ? r->cap : 2048;
            while (nc < need) nc *= 2;
            /* PSRAM: a fleet response is ~22KB and would otherwise spike the scarce internal
             * heap (mbedTLS needs it) during a poll. free() handles PSRAM allocs fine. */
            char *nb = heap_caps_realloc(r->buf, nc, MALLOC_CAP_SPIRAM);
            if (!nb) return ESP_ERR_NO_MEM;
            r->buf = nb; r->cap = nc;
        }
        memcpy(r->buf + r->len, e->data, e->data_len);
        r->len += e->data_len;
        r->buf[r->len] = '\0';
    } else if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(e->header_key, "Set-Cookie") == 0) {
            char *p = strchr(e->header_value, ';');
            int len = p ? (p - e->header_value) : strlen(e->header_value);
            if (s_cookies[0]) strlcat(s_cookies, "; ", sizeof(s_cookies));
            strncat(s_cookies, e->header_value, len);
            ESP_LOGI(TAG, "Scraped cookie: %.*s", len, e->header_value);
        } else if (strcasecmp(e->header_key, "Location") == 0) {
            /* Response headers arrive ONLY via this event; esp_http_client_get_header()
             * reads REQUEST headers, so the 302 Location must be captured here or the
             * whole redirect chain stalls with an empty location. */
            if (r) strlcpy(r->location, e->header_value, sizeof(r->location));
        }
    }
    return ESP_OK;
}

/* ---- helpers ---- */

static void base64url_encode(const uint8_t *src, size_t slen, char *dst, size_t dlen)
{
    size_t out_len = 0;
    mbedtls_base64_encode((unsigned char *)dst, dlen, &out_len, src, slen);
    /* URL-safe mapping: + -> -, / -> _ */
    for (char *p = dst; *p; p++) {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
        else if (*p == '=') { *p = '\0'; break; }   /* no padding */
    }
}

static void generate_pkce(void)
{
    uint8_t rnd[32];
    esp_fill_random(rnd, 32);
    base64url_encode(rnd, 32, s_pkce_verifier, sizeof(s_pkce_verifier));

    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)s_pkce_verifier, strlen(s_pkce_verifier), hash, 0);
    base64url_encode(hash, 32, s_pkce_challenge, sizeof(s_pkce_challenge));
}

static char *url_encode(const char *in)
{
    if (!in) return NULL;
    char *out = malloc(strlen(in) * 3 + 1);
    if (!out) return NULL;
    char *p = out;
    while (*in) {
        if (isalnum((int)*in) || *in == '-' || *in == '_' || *in == '.' || *in == '~') {
            *p++ = *in;
        } else {
            p += sprintf(p, "%%%02X", (unsigned char)*in);
        }
        in++;
    }
    *p = '\0';
    return out;
}

/* Pull a hidden form input's value by field name. Anchors on name="<field>" rather
 * than a bare substring of the name, so e.g. "next" doesn't match the first stray
 * "next" elsewhere on the page (there's one ~5 KB before the real field) and grab the
 * wrong value. Django renders name before value, so scanning forward for value=" works. */
static char *extract_field(const char *html, const char *field)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "name=\"%s\"", field);
    const char *p = strstr(html, needle);
    if (!p) return NULL;
    p = strstr(p, "value=\"");
    if (!p) return NULL;
    p += 7;
    const char *end = strchr(p, '"');
    if (!end) return NULL;
    return strndup(p, end - p);
}

/* -------------------------------------------------------------------------- */

typedef struct {
    int code;
    char *body;
    int  len;             /* body byte count — needed for binary bodies (JPEG snapshots) */
    char location[512];   /* OAuth redirect URLs (/login/?next=<authorize>) run ~270 chars */
} http_resp_t;

/* Control commands (home/jog/preheat/pause/...) use a shorter timeout than bulk polls so a
 * stalled cloud request can't hold the TLS mutex — and therefore the next command — for the
 * full 15 s. Polls keep the longer timeout (the cloud is occasionally slow under load). */
#define CONNECT_CMD_TIMEOUT_MS 8000

static http_resp_t do_http_to(const char *method, const char *url, const char *ct, const char *body, bool with_auth, int timeout_ms)
{
    ESP_LOGI(TAG, "HTTP %s %s", method, url);
    http_resp_t r = {0};
    if (s_http_mtx) xSemaphoreTake(s_http_mtx, portMAX_DELAY);   /* serialize cloud TLS */
    resp_t acc = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = (!strcmp(method, "POST")) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = PRUSA_USER_AGENT,
        .timeout_ms = timeout_ms,
        .event_handler = http_event,
        .user_data = &acc,
        .buffer_size_tx = 4096,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { if (s_http_mtx) xSemaphoreGive(s_http_mtx); return r; }

    if (s_cookies[0]) esp_http_client_set_header(client, "Cookie", s_cookies);
    if (with_auth && s_at[0]) {
        char hdr[2100]; snprintf(hdr, sizeof(hdr), "Bearer %s", s_at);
        esp_http_client_set_header(client, "Authorization", hdr);
    }
    if (ct) esp_http_client_set_header(client, "Content-Type", ct);
    esp_http_client_set_header(client, "Referer", PRUSA_ACCOUNT_URL "/login/");

    if (body) esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        r.code = esp_http_client_get_status_code(client);
        r.body = acc.buf;
        r.len  = acc.len;
        strlcpy(r.location, acc.location, sizeof(r.location));   /* captured in http_event */
        ESP_LOGI(TAG, "  -> %d (body %d, loc %s)", r.code, acc.len, r.location);
    } else {
        ESP_LOGE(TAG, "  -> err %s", esp_err_to_name(err));
        free(acc.buf);
    }
    esp_http_client_cleanup(client);
    if (s_http_mtx) xSemaphoreGive(s_http_mtx);
    return r;
}

static http_resp_t do_http(const char *method, const char *url, const char *ct, const char *body, bool with_auth)
{
    return do_http_to(method, url, ct, body, with_auth, 15000);
}

/* Ported followRedirects */
static http_resp_t follow_redirects(http_resp_t r, int max)
{
    while (max-- > 0 && (r.code >= 301 && r.code <= 307)) {
        if (!r.location[0]) break;
        char next_url[640];   /* account host prefix + a full (long) location */
        if (strncmp(r.location, "http", 4) != 0) {
            snprintf(next_url, sizeof(next_url), "%s%s", PRUSA_ACCOUNT_URL, r.location);
        } else {
            strlcpy(next_url, r.location, sizeof(next_url));
        }
        if (strstr(next_url, "auth-callback") && strstr(next_url, "code=")) return r;
        
        free(r.body);
        r = do_http("GET", next_url, NULL, NULL, false);
    }
    return r;
}

/* -------------------------------------------------------------------------- */

static pp_connect_status_t try_exchange_code(void)
{
    /* The login POST redirects to the account dashboard ("/"), NOT to the OAuth
     * callback. Now that we hold an authenticated session cookie, re-request
     * /o/authorize/ with the SAME PKCE challenge — this redirect carries the code. */
    char *enc = url_encode(PRUSA_REDIRECT_URI);
    char aurl[512];
    snprintf(aurl, sizeof(aurl),
             "%s?response_type=code&client_id=%s&redirect_uri=%s&code_challenge_method=S256&code_challenge=%s",
             PRUSA_AUTH_URL, PRUSA_CLIENT_ID, enc, s_pkce_challenge);
    free(enc);

    http_resp_t r = follow_redirects(do_http("GET", aurl, NULL, NULL, false), 10);
    const char *code_ptr = strstr(r.location, "code=");
    if (!code_ptr && r.body) code_ptr = strstr(r.body, "code=");
    if (!code_ptr) {
        ESP_LOGW(TAG, "no auth code after authorize (http=%d loc=%s)", r.code, r.location);
        free(r.body);
        return PP_CONNECT_ERROR;
    }

    char code[256];
    strlcpy(code, code_ptr + 5, sizeof(code));
    char *amp = strchr(code, '&'); if (amp) *amp = '\0';
    free(r.body);

    char *enc_uri = url_encode(PRUSA_REDIRECT_URI);
    char body[1024];
    snprintf(body, sizeof(body), 
             "grant_type=authorization_code&client_id=%s&code=%s&code_verifier=%s&redirect_uri=%s",
             PRUSA_CLIENT_ID, code, s_pkce_verifier, enc_uri);
    free(enc_uri);

    http_resp_t tr = do_http("POST", PRUSA_TOKEN_URL, "application/x-www-form-urlencoded", body, false);
    if (tr.code == 200 && tr.body) {
        cJSON *j = cJSON_Parse(tr.body);
        if (j) {
            cJSON *at = cJSON_GetObjectItem(j, "access_token");
            cJSON *rt = cJSON_GetObjectItem(j, "refresh_token");
            if (cJSON_IsString(at)) strlcpy(s_at, at->valuestring, sizeof(s_at));
            if (cJSON_IsString(rt)) strlcpy(s_rt, rt->valuestring, sizeof(s_rt));
            
            nvs_handle_t h;
            if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_str(h, KEY_AT, s_at);
                nvs_set_str(h, KEY_RT, s_rt);
                nvs_commit(h); nvs_close(h);
            }
            cJSON_Delete(j);
            free(tr.body);
            ESP_LOGI(TAG, "login OK — access/refresh tokens stored");
            return PP_CONNECT_AUTH_OK;
        }
    }
    ESP_LOGW(TAG, "token exchange failed (http=%d)", tr.code);
    free(tr.body);
    return PP_CONNECT_AUTH_FAILED;
}

static pp_connect_status_t login_locked(const char *email, const char *password)
{
    /* Stash for credential persistence on success (auto re-auth). */
    strlcpy(s_saved_email, email ? email : "", sizeof(s_saved_email));
    strlcpy(s_saved_pass, password ? password : "", sizeof(s_saved_pass));
    s_cookies[0] = s_at[0] = s_rt[0] = '\0';
    generate_pkce();
    
    char *enc_uri = url_encode(PRUSA_REDIRECT_URI);
    char url[512];
    snprintf(url, sizeof(url), "%s?response_type=code&client_id=%s&redirect_uri=%s&code_challenge_method=S256&code_challenge=%s",
             PRUSA_AUTH_URL, PRUSA_CLIENT_ID, enc_uri, s_pkce_challenge);
    free(enc_uri);

    http_resp_t r = follow_redirects(do_http("GET", url, NULL, NULL, false), 10);
    if (!r.body) return PP_CONNECT_ERROR;

    char *csrf = extract_field(r.body, "csrfmiddlewaretoken");
    char *next = extract_field(r.body, "next");
    if (csrf) strlcpy(s_csrf, csrf, sizeof(s_csrf));
    if (next) strlcpy(s_next, next, sizeof(s_next));
    free(csrf); free(next);

    ESP_LOGI(TAG, "login form: csrf=%s next=%.60s", s_csrf[0] ? "ok" : "MISSING", s_next);
    if (!s_csrf[0]) { free(r.body); return PP_CONNECT_ERROR; }

    char *e_email = url_encode(email);
    char *e_pass = url_encode(password);
    char *e_next = url_encode(s_next);
    char lbody[1024];
    snprintf(lbody, sizeof(lbody), "csrfmiddlewaretoken=%s&next=%s&email=%s&password=%s",
             s_csrf, e_next, e_email, e_pass);
    free(e_email); free(e_pass); free(e_next);

    free(r.body);
    r = follow_redirects(do_http("POST", PRUSA_ACCOUNT_URL "/login/", "application/x-www-form-urlencoded", lbody, false), 10);

    if (r.body && (strstr(r.body, "/login/totp/") || strstr(r.location, "/login/totp/"))) {
        char *c2 = extract_field(r.body, "csrfmiddlewaretoken");
        if (c2) strlcpy(s_csrf, c2, sizeof(s_csrf));
        free(c2);
        strlcpy(s_totp_url, PRUSA_ACCOUNT_URL "/login/totp/", sizeof(s_totp_url));
        free(r.body);
        return PP_CONNECT_NEED_TOTP;
    }

    if (r.body && strstr(r.body, "invalid-feedback")) { free(r.body); return PP_CONNECT_AUTH_FAILED; }

    free(r.body);   /* login established the session cookie; the code comes from a fresh authorize */
    pp_connect_status_t st = try_exchange_code();
    if (st == PP_CONNECT_AUTH_OK) prusa_connect_save_creds();
    return st;
}

/* The login flow mutates shared OAuth state across several blocking calls; serialize it so the
 * net-task auto-reauth and a concurrent web login can't interleave and corrupt each other. */
pp_connect_status_t prusa_connect_login(const char *email, const char *password)
{
    if (s_login_mtx) xSemaphoreTake(s_login_mtx, portMAX_DELAY);
    pp_connect_status_t st = login_locked(email, password);
    if (s_login_mtx) xSemaphoreGive(s_login_mtx);
    return st;
}

/* Persist (or erase) the saved account credentials for auto re-auth. */
void prusa_connect_save_creds(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_remember && s_saved_email[0] && s_saved_pass[0]) {
        nvs_set_str(h, KEY_EMAIL, s_saved_email);
        nvs_set_str(h, KEY_PASS, s_saved_pass);
    } else {
        nvs_erase_key(h, KEY_EMAIL);
        nvs_erase_key(h, KEY_PASS);
    }
    nvs_commit(h); nvs_close(h);
}

void prusa_connect_set_remember(bool on)
{
    s_remember = on;
    if (!on) { s_saved_email[0] = s_saved_pass[0] = '\0'; prusa_connect_save_creds(); }
}

bool prusa_connect_remember(void) { return s_remember; }
bool prusa_connect_have_saved_creds(void) { return s_saved_email[0] && s_saved_pass[0]; }

/* Auto re-auth: replay the login flow with the saved credentials. Returns AUTH_OK on success;
 * NEED_TOTP if the account has 2FA (can't auto-complete); FAILED/ERROR otherwise. */
pp_connect_status_t prusa_connect_try_saved_login(void)
{
    if (!prusa_connect_have_saved_creds()) return PP_CONNECT_AUTH_FAILED;
    char em[128], pw[128];
    strlcpy(em, s_saved_email, sizeof(em));   /* login() overwrites the s_saved_* via its args */
    strlcpy(pw, s_saved_pass, sizeof(pw));
    ESP_LOGI(TAG, "auto re-auth: replaying saved login for %.40s", em);
    return prusa_connect_login(em, pw);
}

pp_connect_status_t prusa_connect_submit_totp(const char *code)
{
    if (s_login_mtx) xSemaphoreTake(s_login_mtx, portMAX_DELAY);
    char *e_next = url_encode(s_next);
    char body[512];
    snprintf(body, sizeof(body), "csrfmiddlewaretoken=%s&next=%s&otp_token=%s",
             s_csrf, e_next, code);
    free(e_next);

    http_resp_t r = follow_redirects(do_http("POST", s_totp_url, "application/x-www-form-urlencoded", body, false), 10);
    free(r.body);
    pp_connect_status_t st = try_exchange_code();
    if (s_login_mtx) xSemaphoreGive(s_login_mtx);
    return st;
}

bool prusa_connect_is_authenticated(void) { return s_at[0] != '\0' || s_rt[0] != '\0'; }

/* Forget the Prusa account: wipe tokens + team from RAM and NVS. Call off the LVGL
 * task (NVS write) — the web handler runs on the httpd task, which is fine. */
void prusa_connect_logout(void)
{
    s_at[0] = s_rt[0] = s_team[0] = s_cookies[0] = '\0';
    s_saved_email[0] = s_saved_pass[0] = '\0';   /* an explicit logout also forgets saved creds */
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY_AT);
        nvs_erase_key(h, KEY_RT);
        nvs_erase_key(h, KEY_TEAM);
        nvs_erase_key(h, KEY_EMAIL);
        nvs_erase_key(h, KEY_PASS);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "logged out — tokens + saved creds cleared");
}

esp_err_t prusa_connect_refresh_token(void)
{
    if (!s_rt[0]) return ESP_FAIL;
    if (s_refresh_mtx) xSemaphoreTake(s_refresh_mtx, portMAX_DELAY);

    /* Capture the refresh token AFTER acquiring the lock: a concurrent caller may have just
     * rotated it. We POST this exact value and compare later so a 400 from a stale token
     * (already rotated away by another refresh) doesn't wipe a freshly-renewed session. */
    if (!s_rt[0]) { if (s_refresh_mtx) xSemaphoreGive(s_refresh_mtx); return ESP_FAIL; }
    char rt_used[256]; strlcpy(rt_used, s_rt, sizeof(rt_used));

    char body[512];
    snprintf(body, sizeof(body), "grant_type=refresh_token&client_id=%s&refresh_token=%s",
             PRUSA_CLIENT_ID, rt_used);

    http_resp_t r = do_http("POST", PRUSA_TOKEN_URL, "application/x-www-form-urlencoded", body, false);
    esp_err_t ret = ESP_FAIL;
    if (r.code == 200 && r.body) {
        cJSON *j = cJSON_Parse(r.body);
        if (j) {
            cJSON *at = cJSON_GetObjectItem(j, "access_token");
            cJSON *rt = cJSON_GetObjectItem(j, "refresh_token");
            if (cJSON_IsString(at)) strlcpy(s_at, at->valuestring, sizeof(s_at));
            if (cJSON_IsString(rt)) strlcpy(s_rt, rt->valuestring, sizeof(s_rt));   /* rotated token saved */
            nvs_handle_t h;
            if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_str(h, KEY_AT, s_at);
                nvs_set_str(h, KEY_RT, s_rt);
                nvs_commit(h); nvs_close(h);
            }
            cJSON_Delete(j);
            ret = ESP_OK;
        }
    } else if (r.code == 400) {
        /* invalid_grant. Only treat as a real expiry if the token we used is STILL current —
         * if it changed, a concurrent refresh already rotated it (our token was just stale),
         * so the session is fine and we report OK so the caller retries with the new access
         * token. This prevents the multi-task refresh race from spuriously logging out. */
        if (strcmp(s_rt, rt_used) == 0) {
            s_at[0] = s_rt[0] = '\0';
            nvs_handle_t h;
            if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_key(h, KEY_AT); nvs_erase_key(h, KEY_RT);
                nvs_commit(h); nvs_close(h);
            }
            ESP_LOGW(TAG, "refresh rejected (400) — session expired; cleared, re-login needed");
        } else {
            ESP_LOGI(TAG, "refresh 400 on stale token; concurrent refresh already renewed — keeping session");
            ret = ESP_OK;
        }
    }
    free(r.body);
    if (s_refresh_mtx) xSemaphoreGive(s_refresh_mtx);
    return ret;
}

/* -------------------------------------------------------------------------- */

static float jnum(const cJSON *o, const char *k, float def)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(n) ? (float)n->valuedouble : def;
}

esp_err_t prusa_connect_get_fleet(pp_status_t *arr, int max, int *count)
{
    *count = 0;
    http_resp_t r = do_http("GET", "https://connect.prusa3d.com/app/printers?limit=64&offset=0", NULL, NULL, true);
    if (r.code == 401 && prusa_connect_refresh_token() == ESP_OK) {
        free(r.body);
        r = do_http("GET", "https://connect.prusa3d.com/app/printers?limit=64&offset=0", NULL, NULL, true);
    }
    if (r.code != 200 || !r.body) { free(r.body); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(r.body);
    free(r.body);
    if (!root) return ESP_FAIL;

    cJSON *printers = cJSON_GetObjectItemCaseSensitive(root, "printers");
    cJSON *p = NULL;
    cJSON_ArrayForEach(p, printers) {
        if (*count >= max) break;
        pp_status_t *s = &arr[*count];
        memset(s, 0, sizeof(*s));
        
        cJSON *uuid = cJSON_GetObjectItemCaseSensitive(p, "uuid");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(p, "name");
        /* Model: prefer the human name ("Prusa CORE One"), else the enum ("COREONE").
         * NB: "printer_type" is a VERSION string (e.g. "7.1.0") — NOT the model. */
        cJSON *model = cJSON_GetObjectItemCaseSensitive(p, "printer_type_name");
        if (!cJSON_IsString(model)) model = cJSON_GetObjectItemCaseSensitive(p, "printer_model");
        cJSON *fw = cJSON_GetObjectItemCaseSensitive(p, "firmware");
        cJSON *st = cJSON_GetObjectItemCaseSensitive(p, "printer_state");
        cJSON *conn = cJSON_GetObjectItemCaseSensitive(p, "connect_state");  /* not connection_state */
        cJSON *team = cJSON_GetObjectItemCaseSensitive(p, "team_name");
        cJSON *teamid = cJSON_GetObjectItemCaseSensitive(p, "team_id");

        if (cJSON_IsString(uuid)) strlcpy(s->uuid, uuid->valuestring, sizeof(s->uuid));
        if (cJSON_IsString(name)) strlcpy(s->printer_name, name->valuestring, sizeof(s->printer_name));
        if (cJSON_IsString(model)) strlcpy(s->model, model->valuestring, sizeof(s->model));
        if (cJSON_IsString(fw)) strlcpy(s->firmware, fw->valuestring, sizeof(s->firmware));
        if (cJSON_IsString(st)) strlcpy(s->state, st->valuestring, sizeof(s->state));
        s->online = (cJSON_IsString(conn) && strcmp(conn->valuestring, "OFFLINE") != 0);
        if (cJSON_IsString(team)) strlcpy(s->team, team->valuestring, sizeof(s->team));
        s->team_id = cJSON_IsNumber(teamid) ? teamid->valueint : 0;
        s->is_cloud = true;
        /* Connect relays pause/stop/GCODE (preheat/jog/home) to any connected printer,
         * so an online cloud printer always has control — no per-printer probe needed
         * (and unlike local PrusaLink, this works for Buddy-embedded printers). This is
         * what surfaces the CONTROL button (ui.c gates it on has_control). */
        s->has_control = s->online;

        /* Local PrusaLink fallback creds: Connect exposes the printer's LAN IP +
         * PrusaLink API key. Capturing them lets the device keep polling/controlling the
         * printer directly when Connect auth expires. Prefer the wired (lan) IP. */
        cJSON *ni = cJSON_GetObjectItemCaseSensitive(p, "network_info");
        if (ni) {
            cJSON *lan = cJSON_GetObjectItemCaseSensitive(ni, "lan_ipv4");
            cJSON *wifi = cJSON_GetObjectItemCaseSensitive(ni, "wifi_ipv4");
            if (cJSON_IsString(lan) && lan->valuestring[0]) strlcpy(s->local_ip, lan->valuestring, sizeof(s->local_ip));
            else if (cJSON_IsString(wifi) && wifi->valuestring[0]) strlcpy(s->local_ip, wifi->valuestring, sizeof(s->local_ip));
        }
        cJSON *lk = cJSON_GetObjectItemCaseSensitive(p, "prusalink_api_key");
        if (cJSON_IsString(lk)) strlcpy(s->link_key, lk->valuestring, sizeof(s->link_key));

        /* Telemetry: temps are nested under "temp"; speed/axis_z are top-level. */
        cJSON *temp = cJSON_GetObjectItemCaseSensitive(p, "temp");
        if (temp) {
            s->temp_nozzle   = jnum(temp, "temp_nozzle", 0);
            s->target_nozzle = jnum(temp, "target_nozzle", 0);
            s->temp_bed      = jnum(temp, "temp_bed", 0);
            s->target_bed    = jnum(temp, "target_bed", 0);
        }
        s->speed  = (int)jnum(p, "speed", 0);
        s->axis_z = jnum(p, "axis_z", 0);
        /* Active job lives in "job_info" on the list response (NOT a separate fetch):
         * {state, progress (%), display_name, time_remaining, preview_url, ...}. Populate
         * so PRINTING/PAUSED printers show progress + name on the card and detail. */
        cJSON *ji = cJSON_GetObjectItemCaseSensitive(p, "job_info");
        cJSON *jst = ji ? cJSON_GetObjectItemCaseSensitive(ji, "state") : NULL;
        s->has_job = (cJSON_IsString(jst) &&
                      (strcmp(jst->valuestring, "PRINTING") == 0 ||
                       strcmp(jst->valuestring, "PAUSED")   == 0));
        if (s->has_job) {
            s->progress       = (float)jnum(ji, "progress", 0);
            s->time_remaining = (int)jnum(ji, "time_remaining", -1);
            cJSON *dn = cJSON_GetObjectItemCaseSensitive(ji, "display_name");
            if (cJSON_IsString(dn)) strlcpy(s->job_name, dn->valuestring, sizeof(s->job_name));
        } else {
            s->time_remaining = -1;
        }
        (*count)++;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/* Fetch a single printer's LAN IP + PrusaLink API key (the bulk /app/printers list omits
 * network_info/prusalink_api_key — they're only on the per-printer endpoint). Used once per
 * cloud printer to seed the local PrusaLink fallback. ESP_OK if an IP was found. */
esp_err_t prusa_connect_get_printer_net(const char *uuid, char *ip, int iplen, char *key, int keylen)
{
    if (ip && iplen) ip[0] = '\0';
    if (key && keylen) key[0] = '\0';
    char url[160]; snprintf(url, sizeof(url), "https://connect.prusa3d.com/app/printers/%s", uuid);
    http_resp_t r = do_http("GET", url, NULL, NULL, true);
    if (r.code == 401 && prusa_connect_refresh_token() == ESP_OK) { free(r.body); r = do_http("GET", url, NULL, NULL, true); }
    if (r.code != 200 || !r.body) { free(r.body); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(r.body);
    free(r.body);
    if (!j) return ESP_FAIL;
    cJSON *ni = cJSON_GetObjectItemCaseSensitive(j, "network_info");
    if (ni && ip) {
        cJSON *lan  = cJSON_GetObjectItemCaseSensitive(ni, "lan_ipv4");
        cJSON *wifi = cJSON_GetObjectItemCaseSensitive(ni, "wifi_ipv4");
        if (cJSON_IsString(lan) && lan->valuestring[0])  strlcpy(ip, lan->valuestring, iplen);
        else if (cJSON_IsString(wifi) && wifi->valuestring[0]) strlcpy(ip, wifi->valuestring, iplen);
    }
    cJSON *lk = cJSON_GetObjectItemCaseSensitive(j, "prusalink_api_key");
    if (cJSON_IsString(lk) && key) strlcpy(key, lk->valuestring, keylen);
    cJSON_Delete(j);
    return (ip && ip[0]) ? ESP_OK : ESP_FAIL;
}

/* DEBUG: return the raw per-printer JSON (caller frees) — used to inspect dialog_info etc. */
char *prusa_connect_get_printer_raw(const char *uuid)
{
    char url[160]; snprintf(url, sizeof(url), "https://connect.prusa3d.com/app/printers/%s", uuid);
    http_resp_t r = do_http("GET", url, NULL, NULL, true);
    if (r.code == 401 && prusa_connect_refresh_token() == ESP_OK) { free(r.body); r = do_http("GET", url, NULL, NULL, true); }
    if (r.code != 200) { free(r.body); return NULL; }
    return r.body;
}

static esp_err_t connect_sync_cmd(const char *uuid, const char *cmd, const char *args_json)
{
    char url[256]; snprintf(url, sizeof(url), "https://connect.prusa3d.com/app/printers/%s/commands/sync", uuid);
    char body[512];
    snprintf(body, sizeof(body), "{\"command\":\"%s\",\"args\":%s}", cmd, args_json ? args_json : "[]");
    http_resp_t r = do_http_to("POST", url, "application/json", body, true, CONNECT_CMD_TIMEOUT_MS);
    free(r.body);
    return (r.code >= 200 && r.code < 300) ? ESP_OK : ESP_FAIL;
}

/* Control-path probe: send M115 (firmware-info query — NO motion, harmless) to a
 * printer via commands/sync, to verify the control endpoint + command name work
 * with our token before wiring move/home/preheat. Fills out with the HTTP result. */
void prusa_connect_ctrl_probe(const char *uuid, const char *cmd, char *out, int outlen)
{
    char url[256]; snprintf(url, sizeof(url), "https://connect.prusa3d.com/app/printers/%s/commands/sync", uuid);
    char body[160]; snprintf(body, sizeof(body), "{\"command\":\"%s\",\"args\":[]}", (cmd && cmd[0]) ? cmd : "GET_INFO");
    http_resp_t r = do_http("POST", url, "application/json", body, true);
    if (r.code == 401 && prusa_connect_refresh_token() == ESP_OK) {
        free(r.body);
        r = do_http("POST", url, "application/json", body, true);
    }
    snprintf(out, outlen, "http=%d body=%.200s", r.code, r.body ? r.body : "(none)");
    free(r.body);
}

esp_err_t prusa_connect_pause(const char *uuid)  { return connect_sync_cmd(uuid, "PAUSE_PRINT", "[]"); }
esp_err_t prusa_connect_resume(const char *uuid) { return connect_sync_cmd(uuid, "RESUME_PRINT", "[]"); }
esp_err_t prusa_connect_stop(const char *uuid)   { return connect_sync_cmd(uuid, "STOP_PRINT", "[]"); }

/* Connect's command for raw G-code is "GCODE" (NOT "SEND_GCODE" — that 404s as
 * NOT_FOUND_COMMAND). Confirmed against Prusa-Connect-SDK-Printer const.py: the
 * Command enum has GCODE="GCODE"; there are no dedicated temp/motion/home commands,
 * so preheat/cooldown/home/jog are all expressed as G/M-code through this path. The
 * gcode string is the single positional arg, matching the SDK's args=[gcode]. */
esp_err_t prusa_connect_gcode(const char *uuid, const char *gcode)
{
    char args[256]; snprintf(args, sizeof(args), "[\"%s\"]", gcode);
    return connect_sync_cmd(uuid, "GCODE", args);
}

/* Modern Prusa cloud printers (CORE One, recent MK4/XL firmware) reject raw GCODE on
 * commands/sync (404 NOT_FOUND_COMMAND) and instead take DEDICATED commands with NAMED
 * args carried in "kwargs" (verified live: returns 201 CREATED). The exact command set
 * per printer comes from GET /app/printers/{uuid}/supported-commands. This choke-point
 * is the single place that emits the kwargs envelope. (Klipper/Moonraker still use raw
 * gcode — that path stays in be_gcode/moonraker_gcode.) */
static esp_err_t connect_send_kwargs(const char *uuid, const char *cmd, const char *kwargs_json)
{
    char url[256]; snprintf(url, sizeof(url), "https://connect.prusa3d.com/app/printers/%s/commands/sync", uuid);
    char body[512];
    snprintf(body, sizeof(body), "{\"command\":\"%s\",\"kwargs\":%s}", cmd, (kwargs_json && kwargs_json[0]) ? kwargs_json : "{}");
    http_resp_t r = do_http_to("POST", url, "application/json", body, true, CONNECT_CMD_TIMEOUT_MS);
    if (r.code == 401 && prusa_connect_refresh_token() == ESP_OK) {   /* token expired mid-session */
        free(r.body);
        r = do_http_to("POST", url, "application/json", body, true, CONNECT_CMD_TIMEOUT_MS);
    }
    esp_err_t rc = (r.code >= 200 && r.code < 300) ? ESP_OK : ESP_FAIL;
    ESP_LOGI(TAG, "cmd %s -> %d", cmd, r.code);
    free(r.body);
    return rc;
}

esp_err_t prusa_connect_set_nozzle_temp(const char *uuid, int temp_c)
{
    char k[64]; snprintf(k, sizeof(k), "{\"nozzle_temperature\":%d}", temp_c);
    return connect_send_kwargs(uuid, "SET_NOZZLE_TEMPERATURE", k);
}
esp_err_t prusa_connect_set_bed_temp(const char *uuid, int temp_c)
{
    char k[64]; snprintf(k, sizeof(k), "{\"bed_temperature\":%d}", temp_c);
    return connect_send_kwargs(uuid, "SET_HEATBED_TEMPERATURE", k);
}
esp_err_t prusa_connect_home(const char *uuid, const char *axes)
{
    /* Axis is an UPPERCASE concatenated string e.g. "XYZ" / "X" (verified live; lowercase
     * or arrays return COMMAND_VALUE_TYPE). HOME also requires the printer be READY/IDLE. */
    char k[48]; snprintf(k, sizeof(k), "{\"axis\":\"%s\"}", (axes && axes[0]) ? axes : "XYZ");
    return connect_send_kwargs(uuid, "HOME", k);
}
esp_err_t prusa_connect_move(const char *uuid, int feedrate, float x, float y)
{
    char k[96]; snprintf(k, sizeof(k), "{\"feedrate\":%d,\"x\":%.2f,\"y\":%.2f}", feedrate, x, y);
    return connect_send_kwargs(uuid, "MOVE", k);
}
esp_err_t prusa_connect_move_z(const char *uuid, int feedrate, float distance)
{
    char k[80]; snprintf(k, sizeof(k), "{\"feedrate\":%d,\"distance\":%.2f}", feedrate, distance);
    return connect_send_kwargs(uuid, "MOVE_Z", k);
}

/* Fetch a printer's active attention dialog (dialog_info) from its per-printer endpoint into
 * the dialog_* fields of *s. ESP_OK if a dialog is present (s->dialog_id != 0), else ESP_FAIL.
 * dialog_info shape: {id, code, title, text, buttons:[labels...], key}. */
esp_err_t prusa_connect_get_dialog(const char *uuid, pp_status_t *s)
{
    s->dialog_id = 0; s->dialog_btn_count = 0;
    s->dialog_title[0] = s->dialog_text[0] = '\0';
    char url[160]; snprintf(url, sizeof(url), "https://connect.prusa3d.com/app/printers/%s", uuid);
    http_resp_t r = do_http("GET", url, NULL, NULL, true);
    if (r.code == 401 && prusa_connect_refresh_token() == ESP_OK) { free(r.body); r = do_http("GET", url, NULL, NULL, true); }
    if (r.code != 200 || !r.body) { free(r.body); return ESP_FAIL; }
    cJSON *root = cJSON_Parse(r.body);
    free(r.body);
    if (!root) return ESP_FAIL;
    cJSON *di = cJSON_GetObjectItemCaseSensitive(root, "dialog_info");
    if (cJSON_IsObject(di)) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(di, "id");
        cJSON *ti = cJSON_GetObjectItemCaseSensitive(di, "title");
        cJSON *tx = cJSON_GetObjectItemCaseSensitive(di, "text");
        cJSON *bt = cJSON_GetObjectItemCaseSensitive(di, "buttons");
        if (cJSON_IsNumber(id)) s->dialog_id = id->valueint;
        if (cJSON_IsString(ti)) strlcpy(s->dialog_title, ti->valuestring, sizeof(s->dialog_title));
        if (cJSON_IsString(tx)) strlcpy(s->dialog_text, tx->valuestring, sizeof(s->dialog_text));
        if (cJSON_IsArray(bt)) {
            cJSON *b = NULL;
            cJSON_ArrayForEach(b, bt) {
                if (s->dialog_btn_count >= 3) break;
                if (cJSON_IsString(b)) strlcpy(s->dialog_btns[s->dialog_btn_count++], b->valuestring, sizeof(s->dialog_btns[0]));
            }
        }
    }
    cJSON_Delete(root);
    return (s->dialog_id != 0) ? ESP_OK : ESP_FAIL;
}

/* Respond to an attention dialog: DIALOG_ACTION {dialog_id:int, button:string(label)}. */
esp_err_t prusa_connect_dialog_action(const char *uuid, int dialog_id, const char *button)
{
    char k[96]; snprintf(k, sizeof(k), "{\"dialog_id\":%d,\"button\":\"%s\"}", dialog_id, button ? button : "");
    return connect_send_kwargs(uuid, "DIALOG_ACTION", k);
}

/* Webcam snapshot. Discovered (read-only) from the Connect web app:
 *   GET /app/cameras?limit=100                              -> camera list
 *   GET /thumbnail/camera/{id}?printer_uuid={uuid}          -> snapshot JPEG (404 if none)
 * Step 1 maps the printer UUID to its camera id; step 2 pulls the JPEG bytes. On success
 * *out is a malloc'd buffer (caller frees) of *out_len bytes. ESP_FAIL if the printer has
 * no camera or no fresh frame. The thumbnail is small (downscaled), so internal-RAM
 * buffering via do_http is fine. */
esp_err_t prusa_connect_fetch_snapshot(const char *uuid, uint8_t **out, int *out_len)
{
    *out = NULL; *out_len = 0;
    if (!uuid || !uuid[0]) return ESP_FAIL;

    http_resp_t r = do_http("GET", "https://connect.prusa3d.com/app/cameras?limit=100", NULL, NULL, true);
    if (r.code == 401 && prusa_connect_refresh_token() == ESP_OK) {
        free(r.body);
        r = do_http("GET", "https://connect.prusa3d.com/app/cameras?limit=100", NULL, NULL, true);
    }
    if (r.code != 200 || !r.body) { free(r.body); return ESP_FAIL; }

    cJSON *j = cJSON_Parse(r.body);
    free(r.body);
    if (!j) return ESP_FAIL;
    cJSON *arr = cJSON_IsArray(j) ? j : cJSON_GetObjectItem(j, "cameras");
    long cam_id = -1;
    cJSON *c;
    cJSON_ArrayForEach(c, arr) {
        cJSON *pu = cJSON_GetObjectItemCaseSensitive(c, "printer_uuid");
        cJSON *id = cJSON_GetObjectItemCaseSensitive(c, "id");
        if (cJSON_IsString(pu) && strcmp(pu->valuestring, uuid) == 0 && cJSON_IsNumber(id)) {
            cam_id = (long)id->valuedouble; break;
        }
    }
    cJSON_Delete(j);
    if (cam_id < 0) return ESP_FAIL;   /* printer has no registered camera */

    char url[160];
    snprintf(url, sizeof(url), "https://connect.prusa3d.com/thumbnail/camera/%ld?printer_uuid=%s", cam_id, uuid);
    r = do_http("GET", url, NULL, NULL, true);
    if (r.code != 200 || !r.body || r.len <= 0) { free(r.body); return ESP_FAIL; }
    *out = (uint8_t *)r.body;   /* transfer ownership to caller */
    *out_len = r.len;
    return ESP_OK;
}

/* Feasibility probe: does OUR device token authenticate against the farm GraphQL
 * API (connect-api.prusa3d.com)? A minimal query returns 200 if the token is
 * accepted (data) — or an auth error if not. Fills out with "http=<code> <snippet>". */
void prusa_connect_farm_probe(char *out, int outlen)
{
    http_resp_t r = do_http("POST", "https://connect-api.prusa3d.com/graphql",
                            "application/json", "{\"query\":\"{ __typename }\"}", true);
    if (r.code == 401 && prusa_connect_refresh_token() == ESP_OK) {
        free(r.body);
        r = do_http("POST", "https://connect-api.prusa3d.com/graphql",
                    "application/json", "{\"query\":\"{ __typename }\"}", true);
    }
    snprintf(out, outlen, "http=%d body=%.220s", r.code, r.body ? r.body : "(none)");
    free(r.body);
}

/* Farm organization UUID (persisted) — the org the touch/web Farm view scopes to. */
void prusa_connect_set_org(const char *org)
{
    strlcpy(s_org, org ? org : "", sizeof(s_org));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        if (s_org[0]) nvs_set_str(h, KEY_ORG, s_org); else nvs_erase_key(h, KEY_ORG);
        nvs_commit(h); nvs_close(h);
    }
}
const char *prusa_connect_get_org(void) { return s_org; }

/* POST a GraphQL query to connect-api with refresh-on-401 + retry-on-transient
 * (cloud TLS occasionally fails with http=0). Returns malloc'd body or NULL. */
static char *connect_graphql(const char *body)
{
    for (int a = 0; a < 3; a++) {
        http_resp_t r = do_http("POST", "https://connect-api.prusa3d.com/graphql", "application/json", body, true);
        if (r.code == 200 && r.body) return r.body;   /* transfer ownership */
        bool refreshed = (r.code == 401 && prusa_connect_refresh_token() == ESP_OK);
        free(r.body);
        if (!refreshed) vTaskDelay(pdMS_TO_TICKS(400));
    }
    return NULL;
}

/* Run an arbitrary GraphQL body (the device has no CORS, unlike the browser) — used by the
 * /api/test/gql probe to introspect the schema. Caller frees the malloc'd response. */
char *prusa_connect_graphql_raw(const char *body) { return connect_graphql(body); }

/* Farm: org-scoped Stats (printer + order state counts). malloc'd body or NULL. */
char *prusa_connect_get_farm_stats(const char *org)
{
    char body[600];
    snprintf(body, sizeof(body),
        "{\"query\":\"query S($o:UUID!){stats(organizationId:$o){printers{active online error total}orders(organizationId:$o){states{state count}}}}\",\"variables\":{\"o\":\"%s\"}}",
        (org && org[0]) ? org : s_org);
    return connect_graphql(body);
}

/* Farm: org-scoped order list (with per-order jobCounts for completion %) via GraphQL.
 * Returns the malloc'd GraphQL response body (caller frees), or NULL on failure. */
char *prusa_connect_get_orders(const char *org)
{
    char body[640];
    snprintf(body, sizeof(body),
        "{\"query\":\"query O($o:UUID!,$f:Int!){order{orders(organizationId:$o,first:$f){edges{node{id name number completionDate state jobsCountCompleted jobCounts{created printing done cancelled needAttention}}}}}}\",\"variables\":{\"o\":\"%s\",\"f\":20}}",
        (org && org[0]) ? org : s_org);
    return connect_graphql(body);
}

esp_err_t prusa_connect_get_teams(cJSON **out_json)
{
    http_resp_t r = do_http("GET", PRUSA_MOBILE_API "/api/teams", NULL, NULL, true);
    if (r.code == 401 && prusa_connect_refresh_token() == ESP_OK) {
        free(r.body);
        r = do_http("GET", PRUSA_MOBILE_API "/api/teams", NULL, NULL, true);
    }
    if (r.code != 200 || !r.body) { free(r.body); return ESP_FAIL; }
    *out_json = cJSON_Parse(r.body);
    free(r.body);
    return *out_json ? ESP_OK : ESP_FAIL;
}

esp_err_t prusa_connect_get_team_printers(const char *team_id, pp_status_t *arr, int max, int *count)
{
    *count = 0;
    char url[256]; snprintf(url, sizeof(url), PRUSA_MOBILE_API "/api/teams/%s/printers", team_id);
    http_resp_t r = do_http("GET", url, NULL, NULL, true);
    if (r.code != 200 || !r.body) { free(r.body); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(r.body);
    free(r.body);
    if (!root) return ESP_FAIL;

    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "hydra:member");
    if (!items) items = root;   /* fallback if not Hydra */

    cJSON *p = NULL;
    cJSON_ArrayForEach(p, items) {
        if (*count >= max) break;
        pp_status_t *s = &arr[*count];
        memset(s, 0, sizeof(*s));
        
        cJSON *uuid = cJSON_GetObjectItemCaseSensitive(p, "uuid");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(p, "name");
        cJSON *type = cJSON_GetObjectItemCaseSensitive(p, "printerType");
        if (!type) type = cJSON_GetObjectItemCaseSensitive(p, "printer_type");

        if (cJSON_IsString(uuid)) strlcpy(s->uuid, uuid->valuestring, sizeof(s->uuid));
        if (cJSON_IsString(name)) strlcpy(s->printer_name, name->valuestring, sizeof(s->printer_name));
        if (cJSON_IsString(type)) strlcpy(s->model, type->valuestring, sizeof(s->model));
        s->online = true; /* assume online if in team list for now, or fetch telemetry later */
        (*count)++;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

void prusa_connect_set_default_team(const char *team_id)
{
    strlcpy(s_team, team_id ? team_id : "", sizeof(s_team));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        if (s_team[0]) nvs_set_str(h, KEY_TEAM, s_team);
        else nvs_erase_key(h, KEY_TEAM);
        nvs_commit(h); nvs_close(h);
    }
}

const char* prusa_connect_get_default_team(void) { return s_team; }

void prusa_connect_init(void)
{
    if (!s_http_mtx) s_http_mtx = xSemaphoreCreateMutex();
    if (!s_refresh_mtx) s_refresh_mtx = xSemaphoreCreateMutex();
    if (!s_login_mtx) s_login_mtx = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_at); nvs_get_str(h, KEY_AT, s_at, &sz);
        sz = sizeof(s_rt); nvs_get_str(h, KEY_RT, s_rt, &sz);
        sz = sizeof(s_team); nvs_get_str(h, KEY_TEAM, s_team, &sz);
        sz = sizeof(s_org); nvs_get_str(h, KEY_ORG, s_org, &sz);
        sz = sizeof(s_saved_email); nvs_get_str(h, KEY_EMAIL, s_saved_email, &sz);
        sz = sizeof(s_saved_pass); nvs_get_str(h, KEY_PASS, s_saved_pass, &sz);
        nvs_close(h);
    }
}
