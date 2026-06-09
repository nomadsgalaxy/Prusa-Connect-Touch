/* Prusa-Touch — state, polling, command worker. */
#include "app_state.h"
#include "prusalink.h"
#include "moonraker.h"
#include "bambu.h"
#include "bambu_cloud.h"
#include "printer_store.h"
#include "prefs.h"
#include "wifi.h"
#include "ui.h"
#include "ota_update.h"
#include "pandatouch_msc.h"
#include "prusa_connect.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"   /* esp_restart (orientation-class relayout) */
#include "esp_timer.h"    /* esp_timer_get_time — dashboard publish coalescing */
#include "sdkconfig.h"

#include "pandatouch_display.h"   /* pt_display_schedule_ui */

#include "esp_attr.h"

static const char *TAG = "app_state";

/* wifi_scan() writes up to WIFI_MAX_SCAN entries into pp_wifi_list_t.ssids
 * (sized PP_WIFI_MAX_SCAN); enforce the invariant at compile time. */
_Static_assert(WIFI_MAX_SCAN <= PP_WIFI_MAX_SCAN, "wifi scan cap exceeds ssids[] size");

static pp_status_t      s_status;                  /* active printer (detail screen) */
static EXT_RAM_BSS_ATTR pp_status_t      s_cache[PP_MAX_PRINTERS];   /* fleet cache (dashboard)        */
static EXT_RAM_BSS_ATTR char             s_info_model[PP_MAX_PRINTERS][28];  /* lazy /api/version cache */
static EXT_RAM_BSS_ATTR char             s_info_fw[PP_MAX_PRINTERS][24];
static EXT_RAM_BSS_ATTR bool             s_info_control[PP_MAX_PRINTERS];
static EXT_RAM_BSS_ATTR uint8_t          s_backend[PP_MAX_PRINTERS];          /* pp_backend_t, auto-detected */
static int              s_cache_count;

/* Detect (and cache) whether a printer speaks PrusaLink or Moonraker. Probe runs
 * once per printer on the net task. NOTE: if a Moonraker printer is unreachable at
 * first contact it defaults to PrusaLink until the cache resets (printer edit / reboot). */
static pp_backend_t detect_backend(int i, const pp_printer_t *pr)
{
    if (i < 0 || i >= PP_MAX_PRINTERS) return PP_BK_PRUSALINK;
    if (strncmp(pr->host, "cloud:", 6) == 0) return PP_BK_PRUSA_CONNECT;
    if (strncmp(pr->host, "bambu:", 6) == 0 ||
        strncmp(pr->host, "bambucloud:", 11) == 0) return PP_BK_BAMBU;   /* LAN or cloud; bambu.c routes */
    if (s_backend[i] == PP_BK_UNKNOWN) {
        s_backend[i] = moonraker_probe(pr) ? PP_BK_MOONRAKER : PP_BK_PRUSALINK;
    }
    return (pp_backend_t)s_backend[i];
}

/* Send a gcode line to whichever backend the active printer speaks. Cloud printers
 * go through Connect's GCODE command (uuid is stored as "cloud:<uuid>" in host); this
 * is the only control path that works for Buddy-embedded printers, whose local
 * PrusaLink 404s on gcode endpoints. */
static esp_err_t be_gcode(pp_backend_t bk, const pp_printer_t *pr, const char *g)
{
    if (bk == PP_BK_PRUSA_CONNECT) return prusa_connect_gcode(pr->host + 6, g);
    if (bk == PP_BK_BAMBU)         return bambu_gcode(pr, g);
    return (bk == PP_BK_MOONRAKER) ? moonraker_gcode(pr, g) : prusalink_gcode(g);
}
static int              s_poll_idx;                 /* round-robin cursor             */
static SemaphoreHandle_t s_lock;
static QueueHandle_t    s_cmds;

void app_state_get(pp_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

void app_state_printers_changed(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_cache, 0, sizeof(s_cache));
    memset(s_info_model, 0, sizeof(s_info_model));   /* re-fetch identity after edits */
    memset(s_info_fw, 0, sizeof(s_info_fw));
    memset(s_info_control, 0, sizeof(s_info_control));
    memset(s_backend, 0, sizeof(s_backend));   /* re-detect backend after edits */
    s_cache_count = printer_store_count();
    s_poll_idx = 0;
    xSemaphoreGive(s_lock);
}

void app_state_get_fleet(pp_status_t *arr, int max, int *count)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_cache_count;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) arr[i] = s_cache[i];
    *count = n;
    xSemaphoreGive(s_lock);
}

void app_state_post_cmd(pp_cmd_kind_t kind, const char *path)
{
    pp_cmd_t cmd = { .kind = kind };
    if (path) {
        strlcpy(cmd.path, path, sizeof(cmd.path));
    }
    if (s_cmds) {
        xQueueSend(s_cmds, &cmd, 0);
    }
}

void app_state_select_printer(int index)
{
    pp_cmd_t cmd = { .kind = PP_CMD_SET_PRINTER, .index = index };
    if (s_cmds) {
        xQueueSend(s_cmds, &cmd, 0);
    }
}

void app_state_refresh_dashboard(void)
{
    pp_cmd_t cmd = { .kind = PP_CMD_DASH_REFRESH };
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

void app_state_farm_refresh(void)
{
    pp_cmd_t cmd = { .kind = PP_CMD_FARM_REFRESH };
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

void app_state_fetch_snapshot(void)
{
    pp_cmd_t cmd = { .kind = PP_CMD_SNAPSHOT };
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

void app_state_post_cmd_n(pp_cmd_kind_t kind, int index, int i32a, int i32b)
{
    pp_cmd_t cmd = { .kind = kind, .index = index, .i32a = i32a, .i32b = i32b };
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

void app_state_dialog_action(int dialog_id, const char *button)
{
    pp_cmd_t cmd = { .kind = PP_CMD_DIALOG_ACTION, .index = dialog_id };
    strlcpy(cmd.path, button ? button : "", sizeof(cmd.path));
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

void app_state_store_add(const pp_printer_t *p)
{
    pp_cmd_t cmd = { .kind = PP_CMD_STORE_ADD };
    if (p) cmd.printer = *p;
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}
void app_state_store_update(int idx, const pp_printer_t *p)
{
    pp_cmd_t cmd = { .kind = PP_CMD_STORE_UPDATE, .index = idx };
    if (p) cmd.printer = *p;
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}
void app_state_store_remove(int idx)
{
    pp_cmd_t cmd = { .kind = PP_CMD_STORE_REMOVE, .index = idx };
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

void app_state_set_pref(pp_pref_kind_t pref, int value)
{
    /* Packed so the NVS write happens on this task, not the PSRAM-stack LVGL task. */
    pp_cmd_t cmd = { .kind = PP_CMD_SET_PREF, .index = ((int)pref << 8) | (value & 0xFF) };
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

void app_state_wifi_scan(void)
{
    pp_cmd_t cmd = { .kind = PP_CMD_WIFI_SCAN };
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

void app_state_wifi_connect(const char *ssid, const char *pass)
{
    pp_cmd_t cmd = { .kind = PP_CMD_WIFI_CONNECT };
    strlcpy(cmd.path, ssid ? ssid : "", sizeof(cmd.path));
    strlcpy(cmd.arg2, pass ? pass : "", sizeof(cmd.arg2));
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

void app_state_fetch_thumb(const char *ref)
{
    pp_cmd_t cmd = { .kind = PP_CMD_THUMB };
    strlcpy(cmd.path, ref ? ref : "", sizeof(cmd.path));
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

void app_state_fetch_thumb_dash(const char *ref, int idx)
{
    pp_cmd_t cmd = { .kind = PP_CMD_THUMB_DASH, .index = idx };
    strlcpy(cmd.path, ref ? ref : "", sizeof(cmd.path));
    if (s_cmds) xQueueSend(s_cmds, &cmd, 0);
}

/* Push a heap copy of the current status to the LVGL thread. */
static void publish_status(void)
{
    pp_status_t *copy = malloc(sizeof(*copy));
    if (!copy) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *copy = s_status;
    xSemaphoreGive(s_lock);
    if (pt_display_schedule_ui(ui_apply_status, copy) != LV_RESULT_OK) {
        free(copy);   /* not enqueued -> applier won't run -> free here */
    }
}

/* Push a heap snapshot of the whole fleet cache to the LVGL thread. */
static void publish_dashboard(void)
{
    pp_dash_t *d = malloc(sizeof(*d));
    if (!d) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    d->count = s_cache_count;
    for (int i = 0; i < s_cache_count && i < PP_MAX_PRINTERS; i++) d->items[i] = s_cache[i];
    xSemaphoreGive(s_lock);
    /* Auth-expiry signal: at least one cloud printer is configured but the Connect session
     * has lapsed (refresh token expired → tokens wiped). Drives the dashboard re-connect
     * banner. Local PrusaLink fallback keeps such printers reachable meanwhile. */
    d->conn_expired = false;
    for (int i = 0; i < printer_store_count(); i++) {
        pp_printer_t pr;
        if (printer_store_get(i, &pr) && strncmp(pr.host, "cloud:", 6) == 0) {
            if (!prusa_connect_is_authenticated()) d->conn_expired = true;
            break;
        }
    }
    if (pt_display_schedule_ui(ui_apply_dashboard, d) != LV_RESULT_OK) {
        free(d);
    }
}

/* Fetch Prusa Farm stats + orders (org from NVS), parse, and push to the LVGL thread.
 * Runs on the net task (blocking cloud HTTP + cJSON). */
static int jget(const cJSON *o, const char *k) { return (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, k)); }

static void do_farm_refresh(void)
{
    pp_farm_t *f = malloc(sizeof(*f));
    if (!f) return;
    memset(f, 0, sizeof(*f));

    char *stats = prusa_connect_get_farm_stats(NULL);
    if (stats) {
        cJSON *root = cJSON_Parse(stats);
        cJSON *st = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "data"), "stats");
        cJSON *pr = cJSON_GetObjectItemCaseSensitive(st, "printers");
        if (pr) {
            f->p_active = jget(pr, "active"); f->p_online = jget(pr, "online");
            f->p_error  = jget(pr, "error");  f->p_total  = jget(pr, "total");
            f->valid = true;
        }
        cJSON_Delete(root);
        free(stats);
    }

    char *orders = prusa_connect_get_orders(NULL);
    if (orders) {
        cJSON *root = cJSON_Parse(orders);
        cJSON *oo = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "data"), "order"), "orders");
        cJSON *edges = cJSON_GetObjectItemCaseSensitive(oo, "edges");
        cJSON *e = NULL;
        cJSON_ArrayForEach(e, edges) {
            if (f->order_count >= 8) break;
            cJSON *n = cJSON_GetObjectItemCaseSensitive(e, "node");
            if (!n) continue;
            cJSON *jc = cJSON_GetObjectItemCaseSensitive(n, "jobCounts");
            /* Only PROCESSING orders — the Connect Farm UI's order status (Draft/Processing/
             * Finished/Cancelled). User wants only what's actively being processed; finished
             * and cancelled aren't shown. */
            cJSON *stt = cJSON_GetObjectItemCaseSensitive(n, "state");
            if (!cJSON_IsString(stt) || strcmp(stt->valuestring, "PROCESSING") != 0) continue;
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(n, "name");
            int idx = f->order_count++;
            if (cJSON_IsString(nm)) strlcpy(f->orders[idx].name, nm->valuestring, sizeof(f->orders[idx].name));
            f->orders[idx].done  = jget(n, "jobsCountCompleted");   /* real completed count (not jobCounts.done=0) */
            f->orders[idx].attn  = jget(jc, "needAttention");
            f->orders[idx].total = jget(jc, "created") + jget(jc, "printing") + jget(jc, "done") + jget(jc, "cancelled") + jget(jc, "needAttention");
        }
        cJSON_Delete(root);
        free(orders);
    }

    if (pt_display_schedule_ui(ui_apply_farm, f) != LV_RESULT_OK) free(f);
}

/* Poll one printer (by store index) into the cache; also update s_status if it is
 * the active printer. Returns true if that printer's cached data changed (so the
 * dashboard only republishes on real change, not every cycle). Blocking HTTP —
 * runs on the net task only. */
static bool poll_printer(int i)
{
    pp_printer_t pr;
    if (!printer_store_get(i, &pr)) return false;
    int active = printer_store_active();
    pp_backend_t bk = detect_backend(i, &pr);
    pp_status_t fresh;

    if (bk == PP_BK_PRUSA_CONNECT) {
        /* Local PrusaLink is a FALLBACK, used ONLY when the Connect session has expired.
         * While Connect is authenticated, cloud printers are updated by the fleet poll in
         * net_task — hitting each printer's local PrusaLink every cycle (now that the IP/key
         * are auto-learned for the whole fleet) would waste resources and bypass Connect. */
        if (pr.local_host[0] && !prusa_connect_is_authenticated()) {
            pp_printer_t lpr = pr;
            strlcpy(lpr.host, pr.local_host, sizeof(lpr.host));
            if (prusalink_get_status_of(&lpr, &fresh) == ESP_OK) {
                strlcpy(fresh.printer_name, pr.name, sizeof(fresh.printer_name));
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_cache[i] = fresh;
                s_cache[i].is_cloud = false;
                xSemaphoreGive(s_lock);
                if (i == active) { xSemaphoreTake(s_lock, portMAX_DELAY); s_status = fresh; xSemaphoreGive(s_lock); }
                return true;
            }
        }
        /* Authenticated (or no fallback): cloud printers are updated via the fleet poll. */
        return false;
    }

    if (bk == PP_BK_BAMBU) {
        if (bambu_get_status_of(&pr, &fresh) != ESP_OK) {
            /* Unreachable (off, wrong access code, or Developer Mode disabled): show offline. */
            bool was_online = false;
            if (i >= 0 && i < PP_MAX_PRINTERS) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                was_online = s_cache[i].online;
                memset(&s_cache[i], 0, sizeof(s_cache[i]));
                strlcpy(s_cache[i].printer_name, pr.name, sizeof(s_cache[i].printer_name));
                if (i == active) s_status.online = false;
                xSemaphoreGive(s_lock);
            }
            return was_online;
        }
        /* The report carries identity inline; cache the model once for the detail screen. */
        if (i >= 0 && i < PP_MAX_PRINTERS && s_info_model[i][0] == '\0') {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strlcpy(s_info_model[i], fresh.model, sizeof(s_info_model[i]));
            s_info_control[i] = true;
            xSemaphoreGive(s_lock);
        }
    } else if (bk == PP_BK_MOONRAKER) {
        if (moonraker_get_status_of(&pr, &fresh) != ESP_OK) return false;
    } else {
        if (prusalink_get_status_of(&pr, &fresh) != ESP_OK) return false;
    }
    strlcpy(fresh.printer_name, pr.name, sizeof(fresh.printer_name));

    /* Identity (model/firmware/control): one blocking fetch per HTTP printer, then reuse cache. */
    if (bk != PP_BK_BAMBU && fresh.online && i >= 0 && i < PP_MAX_PRINTERS && s_info_model[i][0] == '\0') {
        char m[28] = {0}, fw[24] = {0}, u[40] = {0};
        bool ctl = false;
        bool got;
        if (bk == PP_BK_MOONRAKER) {
            got = (moonraker_get_info(&pr, m, sizeof(m), fw, sizeof(fw)) == ESP_OK);
            ctl = true;
        } else {
            got = (prusalink_get_info(&pr, m, sizeof(m), fw, sizeof(fw), u, sizeof(u), &ctl) == ESP_OK);
            if (got && u[0] && strcmp(pr.uuid, u) != 0) {
                strlcpy(pr.uuid, u, sizeof(pr.uuid));
                printer_store_update(i, &pr);
                /* If we just learned a UUID for a local printer, check if any Cloud printer matches it. */
                int n = printer_store_count();
                for (int j = 0; j < n; j++) {
                    pp_printer_t cp;
                    if (j != i && printer_store_get(j, &cp) && 
                        strncmp(cp.host, "cloud:", 6) == 0 && strcmp(cp.host + 6, u) == 0) {
                        strlcpy(cp.local_host, pr.host, sizeof(cp.local_host));
                        printer_store_update(j, &cp);
                    }
                }
            }
        }
        if (got && m[0]) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strlcpy(s_info_model[i], m, sizeof(s_info_model[i]));
            strlcpy(s_info_fw[i], fw, sizeof(s_info_fw[i]));
            s_info_control[i] = ctl;
            xSemaphoreGive(s_lock);
        }
    }

    /* Hydrate the polled status from our cached identity row. */
    if (i >= 0 && i < PP_MAX_PRINTERS) {
        strlcpy(fresh.model, s_info_model[i], sizeof(fresh.model));
        strlcpy(fresh.firmware, s_info_fw[i], sizeof(fresh.firmware));
        fresh.has_control = s_info_control[i];
    }

    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (i >= 0 && i < PP_MAX_PRINTERS) {
        changed = (memcmp(&s_cache[i], &fresh, sizeof(fresh)) != 0);
        s_cache[i] = fresh;
        s_cache[i].is_cloud = false;
    }
    s_cache_count = printer_store_count();
    if (i == active) s_status = fresh;
    xSemaphoreGive(s_lock);
    return changed;
}

/* Poll the active printer and publish both views (after a command, or on switch). */
static void refresh_files_usb(const char *path)
{
    pp_file_list_t *fl = malloc(sizeof(pp_file_list_t));
    if (!fl) return;
    fl->count = 0;
    
    int err = 0;
    pt_usb_dir_list_t *list = pt_usb_list_dir(path ? path : "/usb", &err);
    if (list) {
        for (int i = 0; i < list->count && fl->count < PP_MAX_FILES; i++) {
            pt_usb_dir_entry_t *e = &list->entries[i];
            if (e->is_hidden) continue;
            
            bool is_gcode = false;
            const char *ext = strrchr(e->name, '.');
            if (ext) {
                if (!strcasecmp(ext, ".gcode") || !strcasecmp(ext, ".g") || 
                    !strcasecmp(ext, ".bgcode") || !strcasecmp(ext, ".gco")) is_gcode = true;
            }
            if (!e->is_dir && !is_gcode) continue;

            pp_file_t *f = &fl->items[fl->count];
            memset(f, 0, sizeof(*f));
            strlcpy(f->path, e->path, sizeof(f->path));
            strlcpy(f->display, e->name, sizeof(f->display));
            f->is_folder = e->is_dir;
            f->is_print = !e->is_dir;
            if (!e->is_dir) {
                if (e->size < 1024) snprintf(f->meta, sizeof(f->meta), "%u B", (unsigned)e->size);
                else if (e->size < 1024*1024) snprintf(f->meta, sizeof(f->meta), "%.1f KB", (float)e->size/1024.0f);
                else snprintf(f->meta, sizeof(f->meta), "%.1f MB", (float)e->size/(1024.0f*1024.0f));
            }
            fl->count++;
        }
        pt_usb_dir_list_free(list);
    }
    if (pt_display_schedule_ui(ui_apply_files, fl) != LV_RESULT_OK) {
        free(fl);
    }
}

static void poll_active_and_publish(void)
{
    int a = printer_store_active();
    if (a >= 0) {
        poll_printer(a);
        publish_status();
    }
    publish_dashboard();
}

static void run_command(const pp_cmd_t *cmd)
{
    int job_id;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    job_id = s_status.job_id;
    xSemaphoreGive(s_lock);

    /* Resolve the active printer's backend so control/file commands hit the right API. */
    pp_printer_t apr;
    bool have_apr = printer_store_active_get(&apr);
    int aidx = printer_store_active();
    pp_backend_t abk = (have_apr && aidx >= 0) ? detect_backend(aidx, &apr) : PP_BK_PRUSALINK;
    bool moon = (abk == PP_BK_MOONRAKER);
    bool cloud = (abk == PP_BK_PRUSA_CONNECT);
    bool bambu = (abk == PP_BK_BAMBU);
    const char *uuid = cloud ? apr.host + 6 : NULL;

    switch (cmd->kind) {
    case PP_CMD_PAUSE:
        if (cloud) prusa_connect_pause(uuid);
        else if (bambu) bambu_pause(&apr);
        else moon ? moonraker_pause(&apr) : prusalink_pause(job_id);
        break;
    case PP_CMD_RESUME:
        if (cloud) prusa_connect_resume(uuid);
        else if (bambu) bambu_resume(&apr);
        else moon ? moonraker_resume(&apr) : prusalink_resume(job_id);
        break;
    case PP_CMD_STOP:
        if (cloud) prusa_connect_stop(uuid);
        else if (bambu) bambu_stop(&apr);
        else moon ? moonraker_stop(&apr) : prusalink_stop(job_id);
        break;
    case PP_CMD_PRINT:
        if (cloud || bambu) ESP_LOGW(TAG, "print-from-file not implemented for this backend");
        else moon ? moonraker_print(&apr, cmd->path) : prusalink_print(cmd->path);
        break;
    case PP_CMD_SET_PRINTER:
        printer_store_set_active(cmd->index);
        break;
    case PP_CMD_WIFI_SCAN: {
        pp_wifi_list_t *wl = malloc(sizeof(*wl));
        if (wl) {
            wl->count = wifi_scan(wl->ssids, WIFI_MAX_SCAN);
            if (pt_display_schedule_ui(ui_apply_wifi_list, wl) != LV_RESULT_OK) {
                free(wl);
            }
        }
        return;   /* no status poll needed after a scan */
    }
    case PP_CMD_WIFI_CONNECT:
        wifi_save_and_connect(cmd->path, cmd->arg2);
        return;
    case PP_CMD_THUMB: {
        if (!moon && cmd->path[0]) {   /* Moonraker thumbnails not wired yet */
            uint8_t *buf = NULL; int len = 0;
            if (prusalink_get_blob(cmd->path, &buf, &len) == ESP_OK) {
                pp_image_t *im = malloc(sizeof(*im));
                if (im) {
                    im->data = buf; im->len = len;
                    if (pt_display_schedule_ui(ui_apply_thumb, im) != LV_RESULT_OK) { free(buf); free(im); }
                } else {
                    free(buf);
                }
            }
        }
        return;
    }
    case PP_CMD_THUMB_DASH: {
        if (cmd->path[0]) {
            uint8_t *buf = NULL; int len = 0;
            /* Fetching a dashboard thumbnail for printer cmd->index (PrusaLink only). */
            pp_printer_t pr;
            if (printer_store_get(cmd->index, &pr) &&
                detect_backend(cmd->index, &pr) != PP_BK_MOONRAKER) {
                if (prusalink_get_blob_of(&pr, cmd->path, &buf, &len) == ESP_OK) {
                    pp_image_t *im = malloc(sizeof(*im));
                    pp_thumb_dash_t *td = malloc(sizeof(*td));
                    if (im && td) {
                        im->data = buf; im->len = len;
                        td->image = im; td->index = cmd->index;
                        /* Use the wrapper to pass data to the LVGL thread. */
                        if (pt_display_schedule_ui(ui_apply_thumb_dash, td) != LV_RESULT_OK) {
                            free(buf); free(im); free(td);
                        }
                    } else {
                        free(buf); free(im); free(td);
                    }
                }
            }
        }
        return;
    }
    case PP_CMD_LIST: {
        if (cloud && !apr.local_host[0]) break;   /* cloud: list via local PrusaLink once its IP/key are known */
        pp_file_list_t *list = malloc(sizeof(*list));
        if (list) {
            list->count = 0;
            esp_err_t lr = moon ? moonraker_list(&apr, list->items, PP_MAX_FILES, &list->count)
                                : prusalink_list("/", list->items, PP_MAX_FILES, &list->count);
            if (lr == ESP_OK) {
                if (pt_display_schedule_ui(ui_apply_files, list) != LV_RESULT_OK) {
                    free(list);
                }
            } else {
                free(list);
            }
        }
        break;
    }
    case PP_CMD_LIST_USB:
        refresh_files_usb(cmd->path);
        break;
    case PP_CMD_UPLOAD: {
        const char *name = strrchr(cmd->path, '/');
        name = name ? name + 1 : cmd->path;
        esp_err_t err;
        if (moon) {
            err = moonraker_upload(&apr, cmd->path, name);
        } else {
            err = prusalink_upload(cmd->path, name);
        }
        if (err == ESP_OK) {
            /* If upload succeeded, start printing it. */
            moon ? moonraker_print(&apr, name) : prusalink_print(name);
        }
        break;
    }
    case PP_CMD_GCODE:
        if (cloud) prusa_connect_gcode(uuid, cmd->path);
        else be_gcode(abk, &apr, cmd->path);
        break;
    case PP_CMD_PREHEAT: {
        /* Material presets: PLA / PETG / ASA / Cooldown. Backend-aware: modern Connect
         * printers take dedicated SET_NOZZLE/HEATBED_TEMPERATURE; Klipper/PrusaLink use gcode. */
        static const struct { int noz, bed; } P[] = { {215,60}, {230,85}, {260,100}, {0,0} };
        int m = cmd->index; if (m < 0 || m > 3) m = 0;
        if (cloud) {
            prusa_connect_set_nozzle_temp(uuid, P[m].noz);
            prusa_connect_set_bed_temp(uuid, P[m].bed);
        } else {
            char g[16];
            snprintf(g, sizeof(g), "M104 S%d", P[m].noz); be_gcode(abk, &apr, g);
            snprintf(g, sizeof(g), "M140 S%d", P[m].bed); be_gcode(abk, &apr, g);
        }
        break;
    }
    case PP_CMD_HOME:
        if (cloud) prusa_connect_home(uuid, "XYZ");
        else       be_gcode(abk, &apr, "G28");
        break;
    case PP_CMD_MOVE: {
        /* Relative jog. index=axis(0=X,1=Y,2=Z), i32a=distance*100 mm (signed), i32b=feedrate. */
        float dist = cmd->i32a / 100.0f;
        int   feed = cmd->i32b;
        if (cloud) {
            if (cmd->index == 0)      prusa_connect_move(uuid, feed, dist, 0);
            else if (cmd->index == 1) prusa_connect_move(uuid, feed, 0, dist);
            else                      prusa_connect_move_z(uuid, feed, dist);
        } else {
            char g[40]; const char ax = "XYZ"[cmd->index < 0 || cmd->index > 2 ? 0 : cmd->index];
            be_gcode(abk, &apr, "G91");                                  /* relative */
            snprintf(g, sizeof(g), "G1 %c%.2f F%d", ax, dist, feed); be_gcode(abk, &apr, g);
            be_gcode(abk, &apr, "G90");                                  /* back to absolute */
        }
        break;
    }
    case PP_CMD_DIALOG_ACTION:
        /* Answer the active printer's attention dialog (cloud only). index=dialog_id, path=button. */
        if (cloud && cmd->index) prusa_connect_dialog_action(uuid, cmd->index, cmd->path);
        break;
    case PP_CMD_STORE_ADD: {
        /* Printer-store NVS writes run here (net task), never on the LVGL/PSRAM-stack task. */
        int idx = printer_store_add(&cmd->printer);
        app_state_printers_changed();
        if (idx >= 0) printer_store_set_active(idx);
        pt_display_schedule_ui(ui_apply_printers, NULL);
        publish_dashboard();
        publish_status();
        return;
    }
    case PP_CMD_STORE_UPDATE:
        printer_store_update(cmd->index, &cmd->printer);
        app_state_printers_changed();
        pt_display_schedule_ui(ui_apply_printers, NULL);
        publish_dashboard();
        publish_status();
        return;
    case PP_CMD_STORE_REMOVE:
        printer_store_remove(cmd->index);
        app_state_printers_changed();
        pt_display_schedule_ui(ui_apply_printers, NULL);
        publish_dashboard();
        publish_status();
        return;
    case PP_CMD_DASH_REFRESH:
        publish_dashboard();
        return;
    case PP_CMD_FARM_REFRESH:
        do_farm_refresh();
        return;
    case PP_CMD_SNAPSHOT: {
        /* Active printer's webcam snapshot — cloud only (Connect relays the camera). */
        pp_image_t *im = malloc(sizeof(*im));
        if (im) {
            im->data = NULL; im->len = 0;
            if (cloud) prusa_connect_fetch_snapshot(uuid, &im->data, &im->len);
            if (pt_display_schedule_ui(ui_apply_snapshot, im) != LV_RESULT_OK) {
                free(im->data); free(im);
            }
        }
        return;
    }
    case PP_CMD_SET_PREF: {
        int pref = cmd->index >> 8, val = cmd->index & 0xFF;   /* NVS write on net task */
        if (pref == PP_PREF_SORT) { prefs_set_sort((pp_sort_t)val); publish_dashboard(); }
        else if (pref == PP_PREF_HIDE_OFFLINE) { prefs_set_hide_offline(val != 0); publish_dashboard(); }
        else if (pref == PP_PREF_LOGO) {
            prefs_set_logo((pp_logo_t)val);
            pt_display_schedule_ui(ui_apply_logo, NULL);   /* relayout on the LVGL task */
        }
        else if (pref == PP_PREF_AUTOUPDATE) { prefs_set_auto_update(val != 0); }
        else if (pref == PP_PREF_ORIENT) {
            /* Switching between a landscape class (0,1) and a portrait class (2,3) changes the
             * logical resolution (800x480 <-> 480x800). Screens are laid out once at boot for the
             * active resolution, so a class change needs a reboot to relayout; a same-class flip
             * (0<->1 or 2<->3) is just a live rotation. */
            bool was_portrait = (prefs_orient() == PP_ORIENT_PORTRAIT || prefs_orient() == PP_ORIENT_PORTRAIT_FLIPPED);
            bool now_portrait = (val == PP_ORIENT_PORTRAIT || val == PP_ORIENT_PORTRAIT_FLIPPED);
            prefs_set_orient((pp_orient_t)val);
            if (was_portrait != now_portrait) {
                ESP_LOGI(TAG, "orientation class changed -> reboot to relayout");
                vTaskDelay(pdMS_TO_TICKS(400));   /* let the NVS commit + HTTP response flush */
                esp_restart();
            } else {
                pt_display_schedule_ui(ui_apply_orient, NULL);   /* same class: rotate live */
            }
        }
        return;
    }
    }
    /* Reflect the effect of the command quickly on the active printer. */
    poll_active_and_publish();
}

/* Run every queued UI command now, without blocking. Called at several points across the poll
 * cycle so a button press (home/jog/preheat/pause) is serviced promptly instead of waiting for
 * the whole cloud poll sequence to finish. Runs on the net task, same as the polling, so no new
 * locking is required. */
static void drain_commands(void)
{
    pp_cmd_t cmd;
    while (s_cmds && xQueueReceive(s_cmds, &cmd, 0) == pdTRUE) {
        ESP_LOGI(TAG, "command %d", cmd.kind);
        run_command(&cmd);
    }
}

/* Coalesce the fleet-overview redraw: on a local-poll change, republish at most this often.
 * Fleet temps don't need 1 s granularity, and rebuilding/refreshing every card competes with
 * touch/scroll on the LVGL thread. Cloud refreshes (did_cloud) always publish. */
#define DASH_PUBLISH_MIN_MS 2500
static int64_t s_last_dash_pub_us;

static void net_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(CONFIG_PP_POLL_INTERVAL_MS);
    /* Render the configured fleet (seeded as offline) immediately, before any cloud poll —
     * otherwise a logged-out / token-expired boot leaves the dashboard blank until the first
     * successful poll (which never comes while logged out). */
    publish_dashboard();
    publish_status();
    for (;;) {
        drain_commands();   /* service pending button presses before the (slow) poll cycle */
        int n = printer_store_count();
        if (n > 0) {
            /* If we have cloud printers, poll Connect once per cycle. */
            bool has_cloud = false;
            for (int i = 0; i < n; i++) {
                pp_printer_t pr;
                if (printer_store_get(i, &pr) && strncmp(pr.host, "cloud:", 6) == 0) {
                    has_cloud = true; break;
                }
            }
            /* Auto re-authentication: if the Connect session has lapsed but the user opted to
             * save credentials, replay the login flow to restore it with no manual step. Throttled
             * (~every 60 s) to avoid hammering the auth server; 2FA accounts can't auto-complete. */
            static int s_reauth_tick = 0;
            if (has_cloud && !prusa_connect_is_authenticated() &&
                prusa_connect_have_saved_creds() && (s_reauth_tick++ % 30) == 0) {
                pp_connect_status_t st = prusa_connect_try_saved_login();
                if (st == PP_CONNECT_AUTH_OK) {
                    ESP_LOGI(TAG, "auto re-auth succeeded");
                    publish_dashboard();   /* clears the expiry banner */
                } else if (st == PP_CONNECT_NEED_TOTP) {
                    ESP_LOGW(TAG, "auto re-auth blocked: account requires 2FA");
                }
            }

            /* Throttle cloud polling: hitting /app/printers (Cloudflare-fronted) every
             * ~2 s cycle gets the device rate-limited/blocked. Poll Connect ~every 12 s
             * instead (the one call returns the whole fleet). */
            static int s_cloud_tick = 0;
            bool did_cloud = false;
            if (has_cloud && prusa_connect_is_authenticated() && (s_cloud_tick++ % 6) == 0) {
                did_cloud = true;
                pp_status_t *fleet = heap_caps_malloc(PP_MAX_PRINTERS * sizeof(pp_status_t), MALLOC_CAP_SPIRAM);
                int count = 0;
                if (fleet && prusa_connect_get_fleet(fleet, PP_MAX_PRINTERS, &count) == ESP_OK) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    int act = printer_store_active();
                    for (int i = 0; i < n; i++) {
                        pp_printer_t pr;
                        if (printer_store_get(i, &pr) && strncmp(pr.host, "cloud:", 6) == 0) {
                            const char *uuid = pr.host + 6;
                            for (int j = 0; j < count; j++) {
                                if (strcmp(fleet[j].uuid, uuid) == 0) {
                                    s_cache[i] = fleet[j];
                                    s_cache[i].is_cloud = true;
                                    /* poll_printer() skips cloud printers (no local
                                     * fallback), so the active printer's detail/control
                                     * status would otherwise go stale — and the CONTROL
                                     * button (gated on has_control) would never appear.
                                     * Keep s_status in sync from the fleet snapshot. */
                                    if (i == act) s_status = s_cache[i];
                                    /* Learn the printer's LAN IP + PrusaLink key from Connect
                                     * and persist them as the local fallback, so status/control
                                     * keep working if Connect auth expires. Only write when they
                                     * changed (avoids needless NVS wear every poll). */
                                    if (fleet[j].local_ip[0] &&
                                        (strcmp(pr.local_host, fleet[j].local_ip) != 0 ||
                                         strcmp(pr.api_key, fleet[j].link_key) != 0)) {
                                        strlcpy(pr.local_host, fleet[j].local_ip, sizeof(pr.local_host));
                                        if (fleet[j].link_key[0]) strlcpy(pr.api_key, fleet[j].link_key, sizeof(pr.api_key));
                                        printer_store_update(i, &pr);
                                    }
                                    break;
                                }

                            }
                        }
                    }
                    xSemaphoreGive(s_lock);
                }
                if (fleet) heap_caps_free(fleet);
            }
            drain_commands();   /* the fleet GET is the longest blocker — service commands now */

            /* The bulk fleet list omits network_info/prusalink_api_key, so learn each cloud
             * printer's LAN IP + PrusaLink key from the per-printer endpoint (one printer per
             * cloud cycle to stay under Connect's rate limits). This seeds the local PrusaLink
             * fallback so status/files/control keep working when Connect auth expires. */
            if (did_cloud && prusa_connect_is_authenticated()) {
                for (int i = 0; i < n; i++) {
                    pp_printer_t pr;
                    if (printer_store_get(i, &pr) && strncmp(pr.host, "cloud:", 6) == 0 && !pr.local_host[0]) {
                        char ip[20] = {0}, key[40] = {0};
                        if (prusa_connect_get_printer_net(pr.host + 6, ip, sizeof(ip), key, sizeof(key)) == ESP_OK) {
                            strlcpy(pr.local_host, ip, sizeof(pr.local_host));
                            if (key[0]) strlcpy(pr.api_key, key, sizeof(pr.api_key));
                            printer_store_update(i, &pr);
                            ESP_LOGI(TAG, "learned %s local fallback -> %s", pr.name, ip);
                        }
                        break;  /* one per cycle */
                    }
                }
            }

            /* If the active cloud printer is in ATTENTION, pull its dialog_info (omitted from the
             * bulk list) so the detail screen can surface the attention banner + action buttons.
             * The fleet merge memset-clears dialog_* each poll, so a resolved dialog auto-clears. */
            if (did_cloud && prusa_connect_is_authenticated()) {
                int a = printer_store_active();
                pp_printer_t apr;
                if (a >= 0 && printer_store_get(a, &apr) && strncmp(apr.host, "cloud:", 6) == 0) {
                    bool attn;
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    attn = (strcmp(s_cache[a].state, "ATTENTION") == 0);
                    xSemaphoreGive(s_lock);
                    if (attn) {
                        pp_status_t tmp;
                        if (prusa_connect_get_dialog(apr.host + 6, &tmp) == ESP_OK) {
                            xSemaphoreTake(s_lock, portMAX_DELAY);
                            s_cache[a].dialog_id = tmp.dialog_id;
                            strlcpy(s_cache[a].dialog_title, tmp.dialog_title, sizeof(s_cache[a].dialog_title));
                            strlcpy(s_cache[a].dialog_text,  tmp.dialog_text,  sizeof(s_cache[a].dialog_text));
                            memcpy(s_cache[a].dialog_btns, tmp.dialog_btns, sizeof(s_cache[a].dialog_btns));
                            s_cache[a].dialog_btn_count = tmp.dialog_btn_count;
                            s_status = s_cache[a];
                            xSemaphoreGive(s_lock);
                        }
                    }
                }
            }

            drain_commands();   /* one more checkpoint before the per-printer poll */
            int i = s_poll_idx % n;
            s_poll_idx++;
            bool changed = poll_printer(i);
            if (i == printer_store_active() || did_cloud) publish_status();   /* did_cloud refreshes the active cloud printer's detail/control view */
            /* Always republish on a cloud refresh; coalesce local-poll changes so the fleet
             * overview redraws at most every DASH_PUBLISH_MIN_MS, keeping the LVGL thread free
             * for smooth scroll/tap (the detail screen keeps its full-rate updates). */
            if (did_cloud) {
                publish_dashboard();
                s_last_dash_pub_us = esp_timer_get_time();
            } else if (changed) {
                int64_t now = esp_timer_get_time();
                if (now - s_last_dash_pub_us >= (int64_t)DASH_PUBLISH_MIN_MS * 1000) {
                    publish_dashboard();
                    s_last_dash_pub_us = now;
                }
            }
        } else {
            /* No printers configured yet. */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memset(&s_status, 0, sizeof(s_status));
            s_status.time_remaining = -1;
            strlcpy(s_status.printer_name, "No printer", sizeof(s_status.printer_name));
            s_cache_count = 0;
            xSemaphoreGive(s_lock);
            publish_status();
            publish_dashboard();
        }
        pp_cmd_t cmd;
        if (xQueueReceive(s_cmds, &cmd, period) == pdTRUE) {
            ESP_LOGI(TAG, "command %d", cmd.kind);
            run_command(&cmd);
        }
    }
}

void app_state_start(void)
{
    prusa_connect_init();
    bambu_cloud_init();
    s_lock = xSemaphoreCreateMutex();
    s_cmds = xQueueCreate(8, sizeof(pp_cmd_t));
    memset(&s_status, 0, sizeof(s_status));
    memset(s_cache, 0, sizeof(s_cache));
    s_status.time_remaining = -1;
    /* Seed the dashboard cache from the persisted store at boot so printers show
     * immediately (named, offline) instead of "No printers yet" until the first
     * poll/edit. Without this, s_cache_count stayed 0 after every reboot. */
    s_cache_count = printer_store_count();
    for (int i = 0; i < s_cache_count && i < PP_MAX_PRINTERS; i++) {
        pp_printer_t pr;
        if (printer_store_get(i, &pr)) {
            strlcpy(s_cache[i].printer_name, pr.name, sizeof(s_cache[i].printer_name));
            s_cache[i].is_cloud = (strncmp(pr.host, "cloud:", 6) == 0);
        }
    }
    s_poll_idx = 0;
    /* 16 KB: cloud TLS (do_http frame ~3.3 KB) + token-refresh + cJSON parsing of
     * the fleet/stats/orders responses overflowed the old 8 KB stack (crash loop).
     * Pinned to core 0 (PRO_CPU, alongside WiFi) so the CPU-heavy cloud TLS/crypto in the poll
     * and command path never contends with the LVGL render task (pinned to core 1) — this keeps
     * dashboard scroll/tap smooth during the ~6 s cloud polls. */
    xTaskCreatePinnedToCore(net_task, "pp_net", 16384, NULL, 5, NULL, 0);
}
