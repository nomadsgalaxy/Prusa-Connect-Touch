/* Prusa-Touch — runtime printer store (NVS-backed, mutex-protected). */
#include "printer_store.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "esp_attr.h"

static const char *TAG = "printers";
#define NS         "pp"
#define KEY_LIST   "printers"
#define KEY_COUNT  "count"
#define KEY_ACTIVE "active"
#define KEY_VER    "store_ver"
#define STORE_VER  1            /* bump if pp_printer_t layout changes */

static EXT_RAM_ATTR pp_printer_t      s_list[PP_MAX_PRINTERS];
static int               s_count;
static int               s_active = -1;
static SemaphoreHandle_t s_mtx;

#define LOCK()   do { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mtx) xSemaphoreGive(s_mtx); } while (0)

/* Persist current state. Caller must hold s_mtx. */
static void save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, KEY_VER, STORE_VER);

    /* Split the printer list into chunks of 16 to avoid the ~4KB NVS blob limit. */
    for (int i = 0; i < (PP_MAX_PRINTERS + 15) / 16; i++) {
        char key[16];
        snprintf(key, sizeof(key), "plist_%d", i);
        int start = i * 16;
        if (start < s_count) {
            int count = s_count - start;
            if (count > 16) count = 16;
            nvs_set_blob(h, key, &s_list[start], count * sizeof(pp_printer_t));
        } else {
            nvs_erase_key(h, key);
        }
    }

    nvs_set_i32(h, KEY_COUNT, s_count);
    nvs_set_i32(h, KEY_ACTIVE, s_active);
    nvs_commit(h);
    nvs_close(h);
}

static void load_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t ver = 0, cnt = 0, act = -1;
    nvs_get_i32(h, KEY_VER, &ver);
    if (ver != STORE_VER) {           /* schema mismatch -> ignore old blob */
        ESP_LOGW(TAG, "store schema %ld != %d; starting empty", (long)ver, STORE_VER);
        nvs_close(h);
        return;
    }
    nvs_get_i32(h, KEY_COUNT, &cnt);
    nvs_get_i32(h, KEY_ACTIVE, &act);
    if (cnt > 0 && cnt <= PP_MAX_PRINTERS) {
        s_count = 0;
        for (int i = 0; i < (PP_MAX_PRINTERS + 15) / 16; i++) {
            char key[16];
            snprintf(key, sizeof(key), "plist_%d", i);
            size_t sz = 16 * sizeof(pp_printer_t);
            if (nvs_get_blob(h, key, &s_list[s_count], &sz) == ESP_OK) {
                s_count += (sz / sizeof(pp_printer_t));
            }
        }
        /* Fallback for legacy single-blob storage if no chunks found. */
        if (s_count == 0) {
            size_t sz = cnt * sizeof(pp_printer_t);
            if (nvs_get_blob(h, KEY_LIST, s_list, &sz) == ESP_OK) {
                s_count = (int)(sz / sizeof(pp_printer_t));
            }
        }
        s_active = (act >= 0 && act < s_count) ? act : 0;
    }
    nvs_close(h);
}

void printer_store_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    LOCK();
    load_locked();
    if (s_count == 0 && CONFIG_PP_PRINTER_HOST[0] &&
        strcmp(CONFIG_PP_PRINTER_HOST, "192.168.1.50") != 0) {
        pp_printer_t p = {0};
        strlcpy(p.name, "Printer", sizeof(p.name));
        strlcpy(p.host, CONFIG_PP_PRINTER_HOST, sizeof(p.host));
        p.port = CONFIG_PP_PRINTER_PORT;
        strlcpy(p.api_key, CONFIG_PP_PRINTER_APIKEY, sizeof(p.api_key));
        s_list[0] = p;
        s_count = 1;
        s_active = 0;
        save_locked();
        ESP_LOGI(TAG, "seeded printer from Kconfig: %s", p.host);
    }
    ESP_LOGI(TAG, "loaded %d printer(s), active=%d", s_count, s_active);
    UNLOCK();
}

int printer_store_count(void)
{
    LOCK(); int n = s_count; UNLOCK(); return n;
}

bool printer_store_get(int idx, pp_printer_t *out)
{
    bool ok = false;
    LOCK();
    if (out && idx >= 0 && idx < s_count) { *out = s_list[idx]; ok = true; }
    UNLOCK();
    return ok;
}

bool printer_store_active_get(pp_printer_t *out)
{
    bool ok = false;
    LOCK();
    if (out && s_active >= 0 && s_active < s_count) { *out = s_list[s_active]; ok = true; }
    UNLOCK();
    return ok;
}

int printer_store_active(void)
{
    LOCK(); int a = s_active; UNLOCK(); return a;
}

void printer_store_set_active(int idx)
{
    LOCK();
    if (idx >= 0 && idx < s_count) { s_active = idx; save_locked(); }
    UNLOCK();
}

int printer_store_add(const pp_printer_t *p)
{
    int idx = -1;
    LOCK();
    if (p && s_count < PP_MAX_PRINTERS) {
        s_list[s_count] = *p;
        if (s_list[s_count].port == 0) s_list[s_count].port = 80;
        idx = s_count++;
        if (s_active < 0) s_active = idx;
        save_locked();
    }
    UNLOCK();
    return idx;
}

bool printer_store_update(int idx, const pp_printer_t *p)
{
    bool ok = false;
    LOCK();
    if (p && idx >= 0 && idx < s_count) {
        pp_printer_t e = *p;
        if (e.port == 0) e.port = 80;
        s_list[idx] = e;
        save_locked();
        ok = true;
    }
    UNLOCK();
    return ok;
}

void printer_store_remove(int idx)
{
    LOCK();
    if (idx >= 0 && idx < s_count) {
        for (int i = idx; i < s_count - 1; i++) s_list[i] = s_list[i + 1];
        s_count--;
        if (s_active >= s_count) s_active = s_count - 1;
        save_locked();
    }
    UNLOCK();
}

void printer_store_clear(void)
{
    LOCK();
    s_count = 0;
    s_active = -1;
    save_locked();
    UNLOCK();
}
