/* Prusa-Touch — LVGL UI implementation (Prusa-themed). */
#include "ui.h"
#include "app_state.h"
#include "printer_store.h"
#include "pandaprusa_theme.h"
#include "wifi.h"
#include "prefs.h"
#include "pandatouch_display.h"   /* pt_display_schedule_ui — for the test nav API */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lvgl.h"
#include "misc/cache/lv_image_cache.h"   /* lv_image_cache_drop (not in lvgl.h) */
#include "draw/lv_image_decoder.h"       /* lv_image_decoder_get_info           */

#include "esp_attr.h"
#include "esp_heap_caps.h"   /* PSRAM allocation for the fleet snapshot */

static const char *TAG = "ui";

/* ---- display geometry (orientation-aware) ----
 * Screens are built once at boot AFTER ui_apply_orient() sets the rotation, so these report the
 * active logical resolution: 800x480 landscape, 480x800 portrait. Builders use these (and
 * ui_portrait()) to lay out single-column in portrait instead of hardcoding 800/480. */
static inline int32_t scr_w(void) { return lv_display_get_horizontal_resolution(lv_display_get_default()); }
static inline int32_t scr_h(void) { return lv_display_get_vertical_resolution(lv_display_get_default()); }
static inline bool    ui_portrait(void) { return scr_w() < 600; }   /* 480 portrait vs 800 landscape */

/* ---- screens ---- */
static lv_obj_t *s_scr_boot;       /* splash / loading        */
static lv_obj_t *s_boot_bar;
static lv_obj_t *s_boot_status;

static lv_obj_t *s_scr_dash;       /* fleet dashboard (home)  */
static lv_obj_t *s_dash_grid;      /* scrollable card grid    */
static int       s_dash_count;
static pp_status_t *s_dash_items;  /* persistent fleet snapshot (PSRAM) for instant card-open */
typedef struct {
    uint8_t       *buf;
    lv_image_dsc_t dsc;
    char           url[160];
} pp_card_thumb_t;
static EXT_RAM_BSS_ATTR pp_card_thumb_t s_card_thumbs[PP_MAX_PRINTERS];
static lv_obj_t      *s_scr_status;     /* per-printer detail      */
static lv_obj_t *s_scr_control;    /* preheat/jog/home        */
static lv_obj_t *s_scr_files;
static lv_obj_t *s_scr_printers;
static lv_obj_t *s_scr_addpick;    /* "Add a printer" type picker (Cloud accounts / Local printer) */
static lv_obj_t *s_scr_addform;
static lv_obj_t *s_scr_about;
static lv_obj_t *s_scr_prefs;      /* Preferences (sort/filter/logo) */
static lv_obj_t *s_scr_farm;       /* Prusa Farm (org stats + orders) */
static lv_obj_t *s_farm_stat;      /* farm printer-summary label */
static lv_obj_t *s_farm_list;      /* farm orders container */

/* header title (shows active printer name) */
static lv_obj_t *s_title_lbl;

/* Wordmark bylines across all headers — toggled by the logo preference. */
static lv_obj_t *s_bylines[12];
static int       s_byline_count;

/* Preferences widgets */
static lv_obj_t *s_pref_sort_dd;
static lv_obj_t *s_pref_logo_dd;
static lv_obj_t *s_pref_hideoff_sw;
static lv_obj_t *s_pref_autoupd_sw;
static lv_obj_t *s_pref_orient_dd;

/* printer picker + add form */
static lv_obj_t *s_pr_list;
static lv_obj_t *s_ta_name;
static lv_obj_t *s_ta_host;
static lv_obj_t *s_ta_key;
static lv_obj_t *s_ta_serial;         /* Bambu device serial (hidden for other types) */
static lv_obj_t *s_lbl_host;          /* relabeled per type (IP / Host:7125 / Printer IP) */
static lv_obj_t *s_lbl_key;           /* relabeled per type (API key / Access Code)       */
static lv_obj_t *s_lbl_serial;
static lv_obj_t *s_btn_save;
static lv_obj_t *s_btn_cancel;
static int       s_add_type;          /* 0 = Prusa/PrusaLink, 1 = Klipper, 2 = Bambu LAN */
static lv_obj_t *s_kb;
static int       s_edit_idx = -1;     /* -1 = add new; >=0 = editing that printer */
static lv_obj_t *s_btn_remove;
static lv_obj_t *s_btn_setactive;

/* wifi setup */
static lv_obj_t *s_scr_wifi;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_wifi_sel_lbl;
static lv_obj_t *s_wifi_ap_lbl;    /* hotspot-fallback hint */
static lv_obj_t *s_wifi_ta_pass;
static lv_obj_t *s_wifi_kb;
static char      s_wifi_ssids[PP_WIFI_MAX_SCAN][33];
static int       s_wifi_scan_count;
static char      s_wifi_selected[33];

/* ---- status / printer-detail widgets ---- */
static lv_obj_t *s_conn_dot;
static lv_obj_t *s_detail_img;     /* hero: model render on orange tile     */
static lv_obj_t *s_badge;          /* hero: state badge chip                */
static lv_obj_t *s_herotop;        /* hero: name+badge strip (state-tinted) */
static lv_obj_t *s_state_lbl;      /* hero: state text (badge label)        */
static lv_obj_t *s_model_lbl;      /* hero: model sub-line                  */
static lv_obj_t *s_nozzle_lbl;
static lv_obj_t *s_bed_lbl;
static lv_obj_t *s_speed_lbl;
static lv_obj_t *s_z_lbl;
static lv_obj_t *s_job_lbl;
static lv_obj_t *s_bar;
static lv_obj_t *s_pct_lbl;
static lv_obj_t *s_eta_lbl;
static lv_obj_t *s_btn_pause_lbl;
static lv_obj_t *s_btn_control;
/* ---- attention dialog banner (detail screen) ---- */
static lv_obj_t *s_jobcard;        /* hidden while an attention dialog is shown */
static lv_obj_t *s_attn_card;
static lv_obj_t *s_attn_title;
static lv_obj_t *s_attn_text;
static lv_obj_t *s_attn_btns[3];
static lv_obj_t *s_attn_btn_lbls[3];
static int       s_attn_dialog_id;            /* current dialog id (for the action) */
static char      s_attn_btn_text[3][24];      /* current button labels (for the action) */

/* ---- file screen ---- */
static lv_obj_t *s_file_list;
static lv_obj_t *s_files_banner;          /* "Files on <printer>" context banner */
static char      s_active_printer[24];    /* mirror of active printer name/model  */
static char      s_active_model[28];
static pp_file_t s_files[PP_MAX_FILES];
static int       s_file_count;
static bool      s_files_usb_mode = false;

/* ---- file-detail (gcode preview) screen ---- */
static lv_obj_t      *s_scr_filedetail;
static lv_obj_t      *s_fd_name;        /* header: file name        */
static lv_obj_t      *s_thumb_img;      /* lv_image (PNG preview)   */
static lv_obj_t      *s_thumb_ph;       /* placeholder label        */
static lv_image_dsc_t s_thumb_dsc;      /* descriptor over s_thumb_buf */
static uint8_t       *s_thumb_buf;      /* owned PNG bytes on display  */
static char           s_sel_path[160];  /* file selected for printing  */

static lv_obj_t      *s_snap_img;       /* lv_image (webcam JPEG, Control screen) */
static lv_obj_t      *s_snap_ph;        /* placeholder label                      */
static lv_image_dsc_t s_snap_dsc;       /* descriptor over s_snap_buf             */
static uint8_t       *s_snap_buf;       /* owned JPEG bytes on display            */

static void fmt_eta(int secs, char *out, size_t n)
{
    if (secs < 0) { snprintf(out, n, "--"); return; }
    int h = secs / 3600, m = (secs % 3600) / 60;
    if (h > 0) snprintf(out, n, "%dh %02dm", h, m);
    else       snprintf(out, n, "%dm", m);
}

/* forward declarations */
static void on_printers_clicked(lv_event_t *e);
static void refresh_printers_list(void);
/* Screen lock: returns true (and pops the PIN prompt) if the screen is locked, so an action
 * callback can bail. Browsing callbacks don't call it. */
static bool ui_locked_block(void);
static void configure_add_form(int type);   /* relabel fields + show/hide serial per add type */
static void on_wifi_open(lv_event_t *e);
static void on_about_open(lv_event_t *e);
static void on_farm_open(lv_event_t *e);
static void on_prefs_open(lv_event_t *e);
static void thumb_clear(void);
static void snap_clear(void);
static lv_obj_t *make_header(lv_obj_t *parent, const char *text);
static lv_obj_t *make_barbtn(lv_obj_t *bar, const char *text, lv_event_cb_t cb,
                             void *user_data, lv_coord_t w);
static void make_wordmark(lv_obj_t *parent);

/* Detach the preview image, drop its cached bitmap, free the PNG bytes, and show
 * the placeholder. Safe to call repeatedly (LVGL thread only). */
static void thumb_clear(void)
{
    lv_image_set_src(s_thumb_img, NULL);   /* stop referencing the buffer */
    lv_image_cache_drop(&s_thumb_dsc);     /* free any decoded bitmap      */
    if (s_thumb_buf) { free(s_thumb_buf); s_thumb_buf = NULL; }
    lv_memzero(&s_thumb_dsc, sizeof(s_thumb_dsc));
    lv_obj_add_flag(s_thumb_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_thumb_ph, LV_OBJ_FLAG_HIDDEN);
}

/* Clear the dashboard per-card thumbnail cache. */
static void card_thumbs_clear(void)
{
    for (int i = 0; i < PP_MAX_PRINTERS; i++) {
        if (s_card_thumbs[i].buf) {
            lv_image_cache_drop(&s_card_thumbs[i].dsc);
            free(s_card_thumbs[i].buf);
            s_card_thumbs[i].buf = NULL;
        }
        s_card_thumbs[i].url[0] = '\0';
        lv_memzero(&s_card_thumbs[i].dsc, sizeof(lv_image_dsc_t));
    }
}

/* ---------- event handlers (LVGL thread) ---------- */
static void on_pause_clicked(lv_event_t *e)
{
    if (ui_locked_block()) return;
    /* The label text tells us which action applies. */
    const char *txt = lv_label_get_text(s_btn_pause_lbl);
    if (txt && strcmp(txt, "RESUME") == 0) {
        app_state_post_cmd(PP_CMD_RESUME, NULL);
    } else {
        app_state_post_cmd(PP_CMD_PAUSE, NULL);
    }
}

static void on_stop_clicked(lv_event_t *e)
{
    if (ui_locked_block()) return;
    app_state_post_cmd(PP_CMD_STOP, NULL);
}

static void on_files_clicked(lv_event_t *e)
{
    app_state_post_cmd(PP_CMD_LIST, NULL);
    lv_screen_load(s_scr_files);
}

static void on_back_clicked(lv_event_t *e)
{
    lv_screen_load(s_scr_status);
}

static void on_control_back(lv_event_t *e)
{
    (void)e;
    lv_screen_load(s_scr_status);
}

static void on_control_clicked(lv_event_t *e)
{
    (void)e;
    snap_clear();                  /* drop any prior printer's frame */
    if (s_snap_ph) lv_label_set_text(s_snap_ph, "Loading webcam\xE2\x80\xA6");
    app_state_fetch_snapshot();    /* load immediately; the 7s timer keeps it live */
    lv_screen_load(s_scr_control);
}

/* Attention-banner button: answer the active printer's Connect dialog with the tapped label. */
static void on_attn_btn_clicked(lv_event_t *e)
{
    if (ui_locked_block()) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 2 || !s_attn_dialog_id) return;
    app_state_dialog_action(s_attn_dialog_id, s_attn_btn_text[idx]);
}

/* Tapping a file opens its detail/preview screen (does NOT start a print). */
static void on_file_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_file_count) return;

    strlcpy(s_sel_path, s_files[idx].path, sizeof(s_sel_path));
    lv_label_set_text(s_fd_name, s_files[idx].display[0] ? s_files[idx].display
                                                         : s_files[idx].path);
    thumb_clear();
    if (s_files[idx].thumb[0]) {
        lv_label_set_text(s_thumb_ph, "Loading preview...");
        app_state_fetch_thumb(s_files[idx].thumb);   /* -> ui_apply_thumb */
    } else {
        lv_label_set_text(s_thumb_ph, "No preview");
    }
    lv_screen_load(s_scr_filedetail);
}

/* ---------- small UI builders ---------- */
static lv_obj_t *make_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, PP_SURFACE, 0);
    lv_obj_set_style_border_color(c, PP_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 6, 0);   /* match Connect's 6px card radius */
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                             void *user_data, lv_obj_t **out_label)
{
    /* Connect-style ghost button: transparent fill, thin #4E4E4E outline, 4px radius,
     * white text; fills with surface-hi when pressed (matches Connect's Pause/Stop/Print). */
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 150, 56);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, PP_SURFACE_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, PP_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, PP_TEXT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);
    if (out_label) *out_label = l;
    return b;
}

/* Telemetry icons (orange), rasterized from Connect's inline hero SVGs. */
extern const lv_image_dsc_t pt_ic_nozzle;
extern const lv_image_dsc_t pt_ic_bed;
extern const lv_image_dsc_t pt_ic_speed;

/* One Connect-style telemetry pill: muted label on top, then an orange icon + the
 * big white value. icon may be NULL. Returns the value label for the applier. */
static lv_obj_t *detail_cell(lv_obj_t *parent, int x, int y, int w, const char *label,
                             const lv_image_dsc_t *icon)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, w, 56);
    lv_obj_align(cell, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(cell, PP_SURFACE, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_radius(cell, 6, 0);
    lv_obj_set_style_pad_all(cell, 8, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(cell);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);

    int vx = 0;
    if (icon) {
        lv_obj_t *ic = lv_image_create(cell);
        lv_image_set_src(ic, icon);
        lv_obj_align(ic, LV_ALIGN_BOTTOM_LEFT, 0, 2);
        vx = 34;   /* value sits to the right of the 28px icon */
    }
    lv_obj_t *v = lv_label_create(cell);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, PP_TEXT, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, vx, 0);
    return v;
}

static void build_status_screen(void)
{
    s_scr_status = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_status, PP_BG, 0);
    lv_obj_clear_flag(s_scr_status, LV_OBJ_FLAG_SCROLLABLE);

    /* Black top bar: [ PRUSA | TOUCH ] wordmark + connection dot + picker */
    lv_obj_t *bar = make_header(s_scr_status, NULL);   /* identical wordmark placement */

    lv_obj_t *pick = make_barbtn(bar, LV_SYMBOL_LIST, on_printers_clicked, NULL, 48);
    lv_obj_align(pick, LV_ALIGN_RIGHT_MID, -44, 0);

    s_conn_dot = lv_obj_create(bar);
    lv_obj_set_size(s_conn_dot, 18, 18);
    lv_obj_set_style_radius(s_conn_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_conn_dot, 0, 0);
    lv_obj_set_style_bg_color(s_conn_dot, PP_ERROR, 0);
    lv_obj_align(s_conn_dot, LV_ALIGN_RIGHT_MID, -12, 0);

    /* Portrait (480x800) lays the detail screen out single-column: hero across the top,
     * 2x2 telemetry, full-width job/attention card, 2x2 action buttons. Landscape keeps the
     * wide single-row layout. CW = full-width card (16px side margins). */
    const bool P = ui_portrait();
    const int  CW = scr_w() - 32;

    /* ---- hero: orange model tile + state badge + model line ---- */
    lv_obj_t *tile = lv_obj_create(s_scr_status);
    lv_obj_set_size(tile, 84, 84);
    lv_obj_align(tile, LV_ALIGN_TOP_LEFT, 16, 64);
    lv_obj_set_style_bg_color(tile, PP_ORANGE, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 8, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    s_detail_img = lv_image_create(tile);
    lv_obj_center(s_detail_img);

    /* name + state badge on one row (Connect hero) */
    lv_obj_t *herotop = lv_obj_create(s_scr_status);
    lv_obj_set_size(herotop, P ? scr_w() - 128 : 660, 38);
    lv_obj_align(herotop, LV_ALIGN_TOP_LEFT, 112, 66);
    /* State-tinted strip behind name+badge — mirrors the dashboard card header (recolored
     * per-state in ui_apply_status). */
    lv_obj_set_style_bg_opa(herotop, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(herotop, PP_STRIP_GRAY, 0);
    lv_obj_set_style_radius(herotop, 6, 0);
    lv_obj_set_style_border_width(herotop, 0, 0);
    lv_obj_set_style_pad_all(herotop, 0, 0);
    lv_obj_set_style_pad_hor(herotop, 10, 0);
    lv_obj_set_style_pad_column(herotop, 12, 0);
    s_herotop = herotop;
    lv_obj_clear_flag(herotop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(herotop, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(herotop, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_title_lbl = lv_label_create(herotop);            /* printer name (was in the bar) */
    lv_label_set_text(s_title_lbl, "Printer");
    lv_obj_set_style_text_color(s_title_lbl, PP_TEXT, 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_28, 0);

    s_badge = lv_obj_create(herotop);
    lv_obj_set_size(s_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_badge, 12, 0);
    lv_obj_set_style_pad_ver(s_badge, 4, 0);
    lv_obj_set_style_radius(s_badge, 4, 0);
    lv_obj_set_style_border_width(s_badge, 0, 0);
    lv_obj_set_style_bg_color(s_badge, PP_BADGE_GRAY, 0);
    lv_obj_clear_flag(s_badge, LV_OBJ_FLAG_SCROLLABLE);
    s_state_lbl = lv_label_create(s_badge);
    lv_label_set_text(s_state_lbl, "...");
    lv_obj_set_style_text_color(s_state_lbl, PP_TEXT, 0);
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(s_state_lbl);

    s_model_lbl = lv_label_create(s_scr_status);
    lv_label_set_text(s_model_lbl, "");
    lv_obj_set_style_text_color(s_model_lbl, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_model_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_model_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_model_lbl, P ? scr_w() - 128 : 540);
    lv_obj_align(s_model_lbl, LV_ALIGN_TOP_LEFT, 112, 114);

    /* ---- telemetry cells ---- landscape: 4 across; portrait: 2x2 grid ---- */
    if (P) {
        int cw2 = (CW - 12) / 2, xa = 16, xb = 16 + cw2 + 12, r0 = 160, r1 = 226;
        s_nozzle_lbl = detail_cell(s_scr_status, xa, r0, cw2, "NOZZLE", &pt_ic_nozzle);
        s_bed_lbl    = detail_cell(s_scr_status, xb, r0, cw2, "BED",    &pt_ic_bed);
        s_speed_lbl  = detail_cell(s_scr_status, xa, r1, cw2, "SPEED",  &pt_ic_speed);
        s_z_lbl      = detail_cell(s_scr_status, xb, r1, cw2, "Z AXIS", NULL);
    } else {
        s_nozzle_lbl = detail_cell(s_scr_status, 16,  160, 180, "NOZZLE",  &pt_ic_nozzle);
        s_bed_lbl    = detail_cell(s_scr_status, 208, 160, 180, "BED", &pt_ic_bed);
        s_speed_lbl  = detail_cell(s_scr_status, 400, 160, 180, "SPEED",   &pt_ic_speed);
        s_z_lbl      = detail_cell(s_scr_status, 592, 160, 192, "Z AXIS",  NULL);
    }

    /* ---- job / progress card ---- */
    lv_obj_t *jobcard = lv_obj_create(s_scr_status);
    s_jobcard = jobcard;
    const int CARDY = P ? 300 : 228;   /* below the 2x2 telemetry in portrait */
    lv_obj_set_size(jobcard, P ? CW : 768, 88);
    lv_obj_align(jobcard, LV_ALIGN_TOP_LEFT, 16, CARDY);
    lv_obj_set_style_bg_color(jobcard, PP_SURFACE, 0);
    lv_obj_set_style_border_width(jobcard, 0, 0);
    lv_obj_set_style_radius(jobcard, 6, 0);
    lv_obj_set_style_pad_all(jobcard, 12, 0);
    lv_obj_clear_flag(jobcard, LV_OBJ_FLAG_SCROLLABLE);

    s_job_lbl = lv_label_create(jobcard);
    lv_label_set_long_mode(s_job_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_job_lbl, P ? CW - 100 : 560);
    lv_label_set_text(s_job_lbl, "");
    lv_obj_set_style_text_color(s_job_lbl, PP_TEXT, 0);
    lv_obj_set_style_text_font(s_job_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_job_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    s_pct_lbl = lv_label_create(jobcard);
    lv_label_set_text(s_pct_lbl, "");
    lv_obj_set_style_text_color(s_pct_lbl, PP_TEXT, 0);
    lv_obj_set_style_text_font(s_pct_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_pct_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_bar = lv_bar_create(jobcard);
    lv_obj_set_size(s_bar, P ? CW - 48 : 600, 12);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, PP_SURFACE_HI, 0);
    lv_obj_set_style_bg_color(s_bar, PP_ORANGE, LV_PART_INDICATOR);

    s_eta_lbl = lv_label_create(jobcard);
    lv_label_set_text(s_eta_lbl, "");
    lv_obj_set_style_text_color(s_eta_lbl, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_eta_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_eta_lbl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    /* ---- attention dialog banner (overlays the job card when the printer needs attention) ---- */
    s_attn_card = lv_obj_create(s_scr_status);
    lv_obj_set_size(s_attn_card, P ? CW : 768, 130);
    lv_obj_align(s_attn_card, LV_ALIGN_TOP_LEFT, 16, CARDY);
    lv_obj_set_style_bg_color(s_attn_card, PP_STATE_YELLOW, 0);
    lv_obj_set_style_bg_opa(s_attn_card, LV_OPA_20, 0);
    lv_obj_set_style_border_color(s_attn_card, PP_STATE_YELLOW, 0);
    lv_obj_set_style_border_width(s_attn_card, 1, 0);
    lv_obj_set_style_radius(s_attn_card, 6, 0);
    lv_obj_set_style_pad_all(s_attn_card, 10, 0);
    lv_obj_clear_flag(s_attn_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_attn_card, LV_OBJ_FLAG_HIDDEN);

    s_attn_title = lv_label_create(s_attn_card);
    lv_label_set_text(s_attn_title, "");
    lv_obj_set_style_text_color(s_attn_title, PP_TEXT, 0);
    lv_obj_set_style_text_font(s_attn_title, &lv_font_montserrat_16, 0);
    lv_obj_align(s_attn_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_attn_text = lv_label_create(s_attn_card);
    lv_label_set_long_mode(s_attn_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_attn_text, (P ? CW : 768) - 28);
    lv_label_set_text(s_attn_text, "");
    lv_obj_set_style_text_color(s_attn_text, PP_TEXT, 0);
    lv_obj_set_style_text_font(s_attn_text, &lv_font_montserrat_14, 0);
    lv_obj_align(s_attn_text, LV_ALIGN_TOP_LEFT, 0, 24);

    for (int i = 0; i < 3; i++) {
        s_attn_btns[i] = make_button(s_attn_card, "", on_attn_btn_clicked, (void *)(intptr_t)i, &s_attn_btn_lbls[i]);
        lv_obj_set_size(s_attn_btns[i], 150, 34);
        lv_obj_align(s_attn_btns[i], LV_ALIGN_BOTTOM_LEFT, i * 160, 0);
        lv_obj_add_flag(s_attn_btns[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- action buttons (above the 60px bottom nav) ---- landscape: 4 in a row;
     * portrait: 2x2 grid (left column 16, right column flush to the right margin) ---- */
    lv_obj_t *pause_btn = make_button(s_scr_status, "PAUSE", on_pause_clicked, NULL, &s_btn_pause_lbl);
    lv_obj_t *stop_btn  = make_button(s_scr_status, "STOP", on_stop_clicked, NULL, NULL);
    lv_obj_t *files_btn = make_button(s_scr_status, "FILES", on_files_clicked, NULL, NULL);
    s_btn_control = make_button(s_scr_status, "CONTROL", on_control_clicked, NULL, NULL);
    if (P) {
        lv_obj_align(pause_btn,     LV_ALIGN_BOTTOM_LEFT,  16, -140);
        lv_obj_align(stop_btn,      LV_ALIGN_BOTTOM_RIGHT, -16, -140);
        lv_obj_align(files_btn,     LV_ALIGN_BOTTOM_LEFT,  16, -72);
        lv_obj_align(s_btn_control, LV_ALIGN_BOTTOM_RIGHT, -16, -72);
    } else {
        lv_obj_align(pause_btn,     LV_ALIGN_BOTTOM_LEFT, 16,  -72);
        lv_obj_align(stop_btn,      LV_ALIGN_BOTTOM_LEFT, 180, -72);
        lv_obj_align(files_btn,     LV_ALIGN_BOTTOM_LEFT, 344, -72);
        lv_obj_align(s_btn_control, LV_ALIGN_BOTTOM_LEFT, 508, -72);
    }
    lv_obj_add_flag(s_btn_control, LV_OBJ_FLAG_HIDDEN);
}
static void on_storage_toggle(lv_event_t *e)
{
    s_files_usb_mode = !s_files_usb_mode;
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (s_files_usb_mode) {
        lv_label_set_text(lbl, LV_SYMBOL_USB " USB");
        lv_label_set_text(s_files_banner, "Local files on USB drive");
        app_state_post_cmd(PP_CMD_LIST_USB, NULL);
    } else {
        lv_label_set_text(lbl, LV_SYMBOL_IMAGE " Printer");
        lv_label_set_text(s_files_banner, "Files on this printer");
        app_state_post_cmd(PP_CMD_LIST, NULL);
    }
}

static void build_files_screen(void)
{
    s_scr_files = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_files, PP_BG, 0);

    lv_obj_t *bar = make_header(s_scr_files, "Files");
    lv_obj_t *back = make_barbtn(bar, LV_SYMBOL_LEFT " Back", on_back_clicked, NULL, 100);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_t *toggle = make_barbtn(bar, LV_SYMBOL_IMAGE " Printer", on_storage_toggle, NULL, 120);
    lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -116, 0);

    /* Printer-context banner — makes it explicit that files live on the active printer. */
    lv_obj_t *banner = lv_obj_create(s_scr_files);
    lv_obj_set_size(banner, LV_PCT(100), 34);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_radius(banner, 0, 0);
    lv_obj_set_style_border_width(banner, 0, 0);
    lv_obj_set_style_bg_color(banner, PP_SURFACE, 0);
    lv_obj_set_style_pad_hor(banner, 16, 0);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    s_files_banner = lv_label_create(banner);
    lv_label_set_text(s_files_banner, "Files on this printer");
    lv_label_set_long_mode(s_files_banner, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_files_banner, 760);
    lv_obj_set_style_text_color(s_files_banner, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_files_banner, &lv_font_montserrat_14, 0);
    lv_obj_align(s_files_banner, LV_ALIGN_LEFT_MID, 0, 0);

    /* Scrollable column of Connect-style file rows. */
    s_file_list = lv_obj_create(s_scr_files);
    lv_obj_set_size(s_file_list, LV_PCT(100), scr_h() - 56 - 34 - 60);   /* header+banner+nav */
    lv_obj_align(s_file_list, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(s_file_list, PP_BG, 0);
    lv_obj_set_style_border_width(s_file_list, 0, 0);
    lv_obj_set_style_pad_all(s_file_list, 8, 0);
    lv_obj_set_style_pad_row(s_file_list, 8, 0);
    lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
}

/* ---------- file detail (preview + print) ---------- */
static void on_fd_back(lv_event_t *e)
{
    (void)e;
    lv_screen_load(s_scr_files);
}

static void on_fd_print(lv_event_t *e)
{
    (void)e;
    if (ui_locked_block()) return;
    if (s_sel_path[0]) {
        if (s_files_usb_mode) {
            app_state_post_cmd(PP_CMD_UPLOAD, s_sel_path);
        } else {
            app_state_post_cmd(PP_CMD_PRINT, s_sel_path);
        }
    }
    lv_screen_load(s_scr_status);
}

static void build_filedetail_screen(void)
{
    s_scr_filedetail = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_filedetail, PP_BG, 0);
    lv_obj_clear_flag(s_scr_filedetail, LV_OBJ_FLAG_SCROLLABLE);

    /* black header with file name + Back */
    lv_obj_t *bar = make_header(s_scr_filedetail, "");
    s_fd_name = lv_label_create(bar);
    lv_label_set_text(s_fd_name, "File");
    lv_label_set_long_mode(s_fd_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_fd_name, 560);
    lv_obj_set_style_text_color(s_fd_name, PP_WHITE, 0);
    lv_obj_set_style_text_font(s_fd_name, &lv_font_montserrat_20, 0);
    lv_obj_align(s_fd_name, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *back = make_barbtn(bar, LV_SYMBOL_LEFT " Back", on_fd_back, NULL, 100);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, -8, 0);

    /* preview card holds either the thumbnail or a placeholder label */
    lv_obj_t *card = make_card(s_scr_filedetail, 360, 300);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 70);

    s_thumb_ph = lv_label_create(card);
    lv_label_set_text(s_thumb_ph, "No preview");
    lv_obj_set_style_text_color(s_thumb_ph, PP_TEXT_MUTED, 0);
    lv_obj_center(s_thumb_ph);

    s_thumb_img = lv_image_create(card);
    lv_obj_set_size(s_thumb_img, 340, 280);   /* fixed viewport; image centered + scaled to fit */
    lv_obj_center(s_thumb_img);
    lv_obj_add_flag(s_thumb_img, LV_OBJ_FLAG_HIDDEN);

    /* PRINT action */
    lv_obj_t *print_btn = make_button(s_scr_filedetail, "PRINT", on_fd_print, NULL, NULL);
    lv_obj_set_size(print_btn, 220, 64);
    lv_obj_set_style_bg_color(print_btn, PP_ORANGE, 0);        /* orange = primary CTA */
    lv_obj_set_style_bg_opa(print_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(print_btn, 0, 0);
    lv_obj_set_style_bg_color(print_btn, PP_ORANGE_DARK, LV_STATE_PRESSED);
    lv_obj_align(print_btn, LV_ALIGN_BOTTOM_MID, 0, -24);
}

/* ---------- printer picker + add form ---------- */
static void on_printers_clicked(lv_event_t *e)
{
    refresh_printers_list();
    lv_screen_load(s_scr_printers);
}

static void on_add_open(lv_event_t *e)   /* "+ Add printer" -> the type picker */
{
    (void)e;
    lv_screen_load(s_scr_addpick);
}

/* Picker: a Local-printer type was chosen -> set up the field form for it. user_data = type. */
static void on_pick_local(lv_event_t *e)
{
    int type = (int)(intptr_t)lv_event_get_user_data(e);
    s_edit_idx = -1;
    lv_textarea_set_text(s_ta_name, "");
    lv_textarea_set_text(s_ta_host, "");
    lv_textarea_set_text(s_ta_key, "");
    lv_textarea_set_text(s_ta_serial, "");
    lv_obj_add_flag(s_btn_remove, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_setactive, LV_OBJ_FLAG_HIDDEN);
    configure_add_form(type);
    lv_screen_load(s_scr_addform);
}
static void on_pick_cancel(lv_event_t *e) { (void)e; lv_screen_load(s_scr_printers); }

static void on_edit_open(lv_event_t *e)  /* edit mode: skip the picker, prefill from store */
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    pp_printer_t p;
    if (!printer_store_get(idx, &p)) return;
    s_edit_idx = idx;
    bool bambu = (strncmp(p.host, "bambu:", 6) == 0);
    configure_add_form(bambu ? 2 : 0);   /* Klipper vs PrusaLink is cosmetic (auto-detected) */
    lv_textarea_set_text(s_ta_name, p.name);
    lv_textarea_set_text(s_ta_host, bambu ? p.host + 6 : p.host);
    lv_textarea_set_text(s_ta_key, p.api_key);
    lv_textarea_set_text(s_ta_serial, bambu ? p.uuid : "");
    lv_obj_remove_flag(s_btn_remove, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_btn_setactive, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(s_scr_addform);
}

static void on_add_save(lv_event_t *e)
{
    if (ui_locked_block()) return;
    pp_printer_t p = {0};
    strlcpy(p.name, lv_textarea_get_text(s_ta_name), sizeof(p.name));
    strlcpy(p.api_key, lv_textarea_get_text(s_ta_key), sizeof(p.api_key));
    const char *ip = lv_textarea_get_text(s_ta_host);
    if (s_add_type == 2) {                 /* Bambu LAN: host="bambu:<ip>", serial -> uuid */
        snprintf(p.host, sizeof(p.host), "bambu:%s", ip);
        strlcpy(p.uuid, lv_textarea_get_text(s_ta_serial), sizeof(p.uuid));
    } else {
        strlcpy(p.host, ip, sizeof(p.host));
    }
    p.port = 80;
    if (p.name[0] == '\0') strlcpy(p.name, ip, sizeof(p.name));
    /* Route the store write (NVS) through the net task — it can't run on this PSRAM-stacked
     * LVGL task. The net task re-publishes + refreshes the list when it lands. */
    if (ip[0]) {
        if (s_edit_idx < 0) {
            app_state_store_add(&p);          /* net task adds, selects, and publishes */
            lv_screen_load(s_scr_status);     /* optimistic; data fills on the next poll */
            return;
        }
        app_state_store_update(s_edit_idx, &p);
    }
    lv_screen_load(s_scr_printers);
}

static void on_remove(lv_event_t *e)
{
    if (ui_locked_block()) return;
    if (s_edit_idx >= 0) app_state_store_remove(s_edit_idx);   /* net task removes + refreshes */
    lv_screen_load(s_scr_printers);
}

static void on_setactive(lv_event_t *e)
{
    if (s_edit_idx >= 0) app_state_select_printer(s_edit_idx);
    lv_screen_load(s_scr_status);
}

/* Scheduled by the net task after a printer-store write so the Settings list reflects it. */
void ui_apply_printers(void *unused)
{
    (void)unused;
    if (s_pr_list) refresh_printers_list();
}

static void on_add_cancel(lv_event_t *e)
{
    lv_screen_load(s_scr_printers);
}

static void ta_focus_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_kb, ta);
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_keyboard_set_textarea(s_kb, NULL);
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_printers_list(void)
{
    lv_obj_clean(s_pr_list);
    int n = printer_store_count();
    int active = printer_store_active();
    /* --- Device settings (top) --- */
    lv_obj_t *hd = lv_list_add_text(s_pr_list, "DEVICE");
    lv_obj_set_style_text_color(hd, PP_TEXT_MUTED, 0);

    lv_obj_t *pf = lv_list_add_button(s_pr_list, LV_SYMBOL_SETTINGS, "Preferences");
    lv_obj_set_style_bg_color(pf, PP_SURFACE_HI, 0);
    lv_obj_set_style_text_color(pf, PP_TEXT, 0);
    lv_obj_add_event_cb(pf, on_prefs_open, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wf = lv_list_add_button(s_pr_list, LV_SYMBOL_WIFI, "Wi-Fi setup");
    lv_obj_set_style_bg_color(wf, PP_SURFACE_HI, 0);
    lv_obj_set_style_text_color(wf, PP_TEXT, 0);
    lv_obj_add_event_cb(wf, on_wifi_open, LV_EVENT_CLICKED, NULL);

    /* --- Printer management (beneath) --- */
    lv_obj_t *hp = lv_list_add_text(s_pr_list, "PRINTERS");
    lv_obj_set_style_text_color(hp, PP_TEXT_MUTED, 0);
    for (int i = 0; i < n; i++) {
        pp_printer_t p;
        if (!printer_store_get(i, &p)) continue;
        char buf[80];
        snprintf(buf, sizeof(buf), "%s%s  (%s)", (i == active) ? "* " : "",
                 p.name, p.host);
        lv_obj_t *btn = lv_list_add_button(s_pr_list, LV_SYMBOL_EDIT, buf);
        lv_obj_set_style_bg_color(btn, PP_SURFACE, 0);
        lv_obj_set_style_text_color(btn, PP_TEXT, 0);
        lv_obj_add_event_cb(btn, on_edit_open, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
    lv_obj_t *add = lv_list_add_button(s_pr_list, LV_SYMBOL_PLUS, "Add printer");
    lv_obj_set_style_bg_color(add, PP_SURFACE_HI, 0);
    lv_obj_set_style_text_color(add, PP_ORANGE, 0);
    lv_obj_add_event_cb(add, on_add_open, LV_EVENT_CLICKED, NULL);

    /* --- More --- */
    lv_obj_t *hm = lv_list_add_text(s_pr_list, "MORE");
    lv_obj_set_style_text_color(hm, PP_TEXT_MUTED, 0);
    lv_obj_t *fm = lv_list_add_button(s_pr_list, LV_SYMBOL_LIST, "Prusa Farm");
    lv_obj_set_style_bg_color(fm, PP_SURFACE_HI, 0);
    lv_obj_set_style_text_color(fm, PP_TEXT, 0);
    lv_obj_add_event_cb(fm, on_farm_open, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ab = lv_list_add_button(s_pr_list, LV_SYMBOL_LIST, "About / License");
    lv_obj_set_style_bg_color(ab, PP_SURFACE_HI, 0);
    lv_obj_set_style_text_color(ab, PP_TEXT, 0);
    lv_obj_add_event_cb(ab, on_about_open, LV_EVENT_CLICKED, NULL);
}

/* Black top bar carrying the persistent [ PRUSA | TOUCH ] wordmark (left) plus an
 * optional page title to its right — used on every screen for a consistent header. */
static lv_obj_t *make_header(lv_obj_t *parent, const char *text)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 56);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, PP_HEADER, 0);     /* Connect: black top bar */
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    make_wordmark(bar);                                /* persistent brand, left */

    if (text && text[0]) {
        lv_obj_t *t = lv_label_create(bar);
        lv_label_set_text(t, text);
        lv_obj_set_style_text_color(t, PP_TEXT, 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);        /* page title centered on screen */
    }
    return bar;
}

/* A borderless white icon/text button for placement on the black top bar
 * (Connect's top-bar controls have no button background). */
static lv_obj_t *make_barbtn(lv_obj_t *bar, const char *text, lv_event_cb_t cb,
                             void *user_data, lv_coord_t w)
{
    lv_obj_t *b = lv_button_create(bar);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, PP_SURFACE_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, PP_WHITE, 0);
    lv_obj_center(l);
    return b;
}

static void build_printers_screen(void)
{
    s_scr_printers = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_printers, PP_BG, 0);
    make_header(s_scr_printers, "Settings");

    s_pr_list = lv_list_create(s_scr_printers);
    lv_obj_set_size(s_pr_list, LV_PCT(100), scr_h() - 56 - 60);   /* header + nav */
    lv_obj_align(s_pr_list, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(s_pr_list, PP_BG, 0);
    lv_obj_set_style_border_width(s_pr_list, 0, 0);
}

static lv_obj_t *make_field(lv_obj_t *parent, const char *label, lv_coord_t y,
                            bool password, lv_obj_t **out_label)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, PP_TEXT_MUTED, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 16, y);
    if (out_label) *out_label = l;
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_set_width(ta, scr_w() - 146);   /* responsive: fits portrait (480) and landscape (800) */
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 130, y - 8);
    lv_obj_add_event_cb(ta, ta_focus_event, LV_EVENT_ALL, NULL);
    return ta;
}

/* Relabel the shared add form for the chosen type and show/hide the Bambu Serial field,
 * repositioning the action buttons so there's no gap when Serial is hidden. */
static void configure_add_form(int type)
{
    s_add_type = type;
    bool bambu = (type == 2);
    lv_label_set_text(s_lbl_host, bambu ? "Printer IP" : (type == 1 ? "Host:7125" : "IP / host"));
    lv_label_set_text(s_lbl_key,  bambu ? "Access Code" : "API key");
    if (bambu) { lv_obj_remove_flag(s_ta_serial, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s_lbl_serial, LV_OBJ_FLAG_HIDDEN); }
    else       { lv_obj_add_flag(s_ta_serial, LV_OBJ_FLAG_HIDDEN);    lv_obj_add_flag(s_lbl_serial, LV_OBJ_FLAG_HIDDEN); }
    int sy = bambu ? 248 : 204;   /* Save/Cancel sit below the last visible field */
    lv_obj_align(s_btn_save,   LV_ALIGN_TOP_LEFT, 130, sy);
    lv_obj_align(s_btn_cancel, LV_ALIGN_TOP_LEFT, 280, sy);
    int ey = bambu ? 306 : 262;   /* edit-mode actions below Save/Cancel */
    lv_obj_align(s_btn_setactive, LV_ALIGN_TOP_LEFT, 130, ey);
    lv_obj_align(s_btn_remove,    LV_ALIGN_TOP_LEFT, 280, ey);
}

static void build_addform_screen(void)
{
    s_scr_addform = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_addform, PP_BG, 0);
    make_header(s_scr_addform, "Add printer");

    s_ta_name   = make_field(s_scr_addform, "Name", 72, false, NULL);
    s_ta_host   = make_field(s_scr_addform, "IP / host", 116, false, &s_lbl_host);
    s_ta_key    = make_field(s_scr_addform, "API key", 160, true, &s_lbl_key);
    s_ta_serial = make_field(s_scr_addform, "Serial", 204, false, &s_lbl_serial);

    s_btn_save = lv_button_create(s_scr_addform);
    lv_obj_set_size(s_btn_save, 140, 50);
    lv_obj_set_style_bg_color(s_btn_save, PP_ORANGE, 0);
    lv_obj_add_event_cb(s_btn_save, on_add_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_btn_save);
    lv_label_set_text(sl, "Save");
    lv_obj_set_style_text_color(sl, PP_WHITE, 0);
    lv_obj_center(sl);

    s_btn_cancel = lv_button_create(s_scr_addform);
    lv_obj_set_size(s_btn_cancel, 140, 50);
    lv_obj_set_style_bg_color(s_btn_cancel, PP_SURFACE_HI, 0);
    lv_obj_add_event_cb(s_btn_cancel, on_add_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(s_btn_cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, PP_TEXT, 0);
    lv_obj_center(cl);

    /* Edit-mode actions (hidden in add mode) */
    s_btn_setactive = lv_button_create(s_scr_addform);
    lv_obj_set_size(s_btn_setactive, 140, 50);
    lv_obj_set_style_bg_color(s_btn_setactive, PP_SURFACE_HI, 0);
    lv_obj_set_style_border_color(s_btn_setactive, PP_ORANGE, 0);
    lv_obj_set_style_border_width(s_btn_setactive, 2, 0);
    lv_obj_add_event_cb(s_btn_setactive, on_setactive, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(s_btn_setactive);
    lv_label_set_text(al, "Set active");
    lv_obj_set_style_text_color(al, PP_TEXT, 0);
    lv_obj_center(al);

    s_btn_remove = lv_button_create(s_scr_addform);
    lv_obj_set_size(s_btn_remove, 140, 50);
    lv_obj_set_style_bg_color(s_btn_remove, PP_ERROR, 0);
    lv_obj_add_event_cb(s_btn_remove, on_remove, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(s_btn_remove);
    lv_label_set_text(rl, "Remove");
    lv_obj_set_style_text_color(rl, PP_WHITE, 0);
    lv_obj_center(rl);

    /* On-screen keyboard, hidden until a field is focused. */
    s_kb = lv_keyboard_create(s_scr_addform);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    configure_add_form(0);   /* default: PrusaLink layout (serial hidden, buttons placed) */
}

/* "Add a printer" type picker — mirrors the web two-tier flow. Cloud accounts need a keyboard
 * so they're added from the web page; the device handles Local printers directly. */
static void build_addpick_screen(void)
{
    s_scr_addpick = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_addpick, PP_BG, 0);
    make_header(s_scr_addpick, "Add a printer");

    lv_obj_t *col = lv_obj_create(s_scr_addpick);
    lv_obj_set_size(col, scr_w(), scr_h() - 52);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, 16, 0);
    lv_obj_set_style_pad_row(col, 9, 0);

    lv_obj_t *ch = lv_label_create(col);
    lv_label_set_text(ch, "CLOUD ACCOUNTS");
    lv_obj_set_style_text_color(ch, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(ch, &lv_font_montserrat_12, 0);
    lv_obj_t *cn = lv_label_create(col);
    lv_label_set_text(cn, "Prusa Connect & Bambu: sign in from the web page\n(its address is on the Wi-Fi screen).");
    lv_obj_set_style_text_color(cn, PP_TEXT_MUTED, 0);
    lv_label_set_long_mode(cn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(cn, scr_w() - 32);

    lv_obj_t *lh = lv_label_create(col);
    lv_label_set_text(lh, "LOCAL PRINTER");
    lv_obj_set_style_text_color(lh, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(lh, &lv_font_montserrat_12, 0);

    static const char *names[] = { "Prusa  (PrusaLink)", "Klipper  (Moonraker)", "Bambu Lab  (LAN)" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = make_button(col, names[i], on_pick_local, (void *)(intptr_t)i, NULL);
        lv_obj_set_width(b, scr_w() - 32);
    }
    lv_obj_t *cancel = make_button(col, "Cancel", on_pick_cancel, NULL, NULL);
    lv_obj_set_width(cancel, scr_w() - 32);
}

/* ---------- WiFi setup ---------- */
static void on_wifi_scan_clicked(lv_event_t *e)
{
    lv_obj_clean(s_wifi_list);
    lv_list_add_text(s_wifi_list, "Scanning...");
    app_state_wifi_scan();
}

static void on_wifi_ssid_pick(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_wifi_scan_count) {
        strlcpy(s_wifi_selected, s_wifi_ssids[idx], sizeof(s_wifi_selected));
        lv_label_set_text_fmt(s_wifi_sel_lbl, "Network: %s", s_wifi_selected);
    }
}

static void on_wifi_pass_focus(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_wifi_kb, lv_event_get_target(e));
        lv_obj_remove_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_wifi_connect_clicked(lv_event_t *e)
{
    if (s_wifi_selected[0]) {
        app_state_wifi_connect(s_wifi_selected, lv_textarea_get_text(s_wifi_ta_pass));
    }
    lv_screen_load(s_scr_status);
}

/* Update the Wi-Fi status line. Three states, in priority order: connected (show
 * the device IP + web-UI URL so the user can reach it from a computer), hotspot up,
 * or the setup tip. Cheap + idempotent — also called from ui_apply_status each poll
 * so the IP appears within a cycle of connecting while the screen is open. */
static void wifi_status_label_refresh(void)
{
    if (!s_wifi_ap_lbl) return;
    if (wifi_is_connected() && wifi_ip_str()[0]) {
        lv_obj_set_style_text_color(s_wifi_ap_lbl, PP_OK, 0);
        lv_label_set_text_fmt(s_wifi_ap_lbl,
            LV_SYMBOL_OK " Connected. From a computer on the same network, open "
            "http://%s/ to manage printers and update firmware.", wifi_ip_str());
    } else if (wifi_is_ap_active()) {
        lv_obj_set_style_text_color(s_wifi_ap_lbl, PP_TEXT_MUTED, 0);
        lv_label_set_text_fmt(s_wifi_ap_lbl,
            LV_SYMBOL_WARNING " No network. Hotspot \"%s\" is open - join it from a "
            "phone and open http://192.168.4.1 to set up Wi-Fi.", wifi_ap_ssid());
    } else {
        lv_obj_set_style_text_color(s_wifi_ap_lbl, PP_TEXT_MUTED, 0);
        lv_label_set_text(s_wifi_ap_lbl,
            "Tip: if no known network is found, the device opens a \"PrusaTouch-...\" "
            "hotspot at http://192.168.4.1 for setup.");
    }
}

/* Reset the Wi-Fi screen widgets + hotspot hint and kick off a scan. Shared by the
 * menu entry and the test-nav API so the screen is always correctly populated. */
static void wifi_screen_prepare(void)
{
    s_wifi_selected[0] = '\0';
    lv_label_set_text(s_wifi_sel_lbl, "Network: (tap Scan)");
    lv_textarea_set_text(s_wifi_ta_pass, "");
    lv_obj_clean(s_wifi_list);

    wifi_status_label_refresh();   /* connected IP / hotspot / setup tip */
    app_state_wifi_scan();         /* auto-scan */
    lv_list_add_text(s_wifi_list, "Scanning...");
}

static void on_wifi_open(lv_event_t *e)
{
    wifi_screen_prepare();
    lv_screen_load(s_scr_wifi);
}

void ui_apply_wifi_list(void *arg)
{
    pp_wifi_list_t *wl = (pp_wifi_list_t *)arg;
    lv_obj_clean(s_wifi_list);
    s_wifi_scan_count = wl->count;
    for (int i = 0; i < wl->count && i < PP_WIFI_MAX_SCAN; i++) {
        strlcpy(s_wifi_ssids[i], wl->ssids[i], sizeof(s_wifi_ssids[i]));
        lv_obj_t *b = lv_list_add_button(s_wifi_list, LV_SYMBOL_WIFI, wl->ssids[i]);
        lv_obj_set_style_bg_color(b, PP_SURFACE, 0);
        lv_obj_set_style_text_color(b, PP_TEXT, 0);
        lv_obj_add_event_cb(b, on_wifi_ssid_pick, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
    if (wl->count == 0) {
        lv_list_add_text(s_wifi_list, "No networks found — tap Scan");
    }
    free(wl);
}

static void build_wifi_screen(void)
{
    s_scr_wifi = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_wifi, PP_BG, 0);
    lv_obj_t *bar = make_header(s_scr_wifi, "Wi-Fi");

    lv_obj_t *scan = make_barbtn(bar, LV_SYMBOL_REFRESH " Scan", on_wifi_scan_clicked, NULL, 120);
    lv_obj_align(scan, LV_ALIGN_RIGHT_MID, -8, 0);

    /* Landscape: scan list (left) + selection form (right column at x=396). Portrait stacks
     * them: the compact form at top, the scan list filling the width below it. */
    const bool P = ui_portrait();
    const int  fx = P ? 16 : 396;          /* form column x */
    const int  fw = P ? scr_w() - 32 : 380;/* form field width */

    s_wifi_sel_lbl = lv_label_create(s_scr_wifi);
    lv_label_set_text(s_wifi_sel_lbl, "Network: (tap Scan)");
    lv_obj_set_style_text_color(s_wifi_sel_lbl, PP_TEXT, 0);
    lv_obj_align(s_wifi_sel_lbl, LV_ALIGN_TOP_LEFT, fx, 72);

    s_wifi_ta_pass = lv_textarea_create(s_scr_wifi);
    lv_textarea_set_one_line(s_wifi_ta_pass, true);
    lv_textarea_set_password_mode(s_wifi_ta_pass, true);
    lv_textarea_set_placeholder_text(s_wifi_ta_pass, "password");
    lv_obj_set_width(s_wifi_ta_pass, fw);
    lv_obj_align(s_wifi_ta_pass, LV_ALIGN_TOP_LEFT, fx, 104);
    lv_obj_add_event_cb(s_wifi_ta_pass, on_wifi_pass_focus, LV_EVENT_ALL, NULL);

    lv_obj_t *conn = lv_button_create(s_scr_wifi);
    lv_obj_set_size(conn, 160, 56);
    lv_obj_align(conn, LV_ALIGN_TOP_LEFT, fx, 156);
    lv_obj_set_style_bg_color(conn, PP_ORANGE, 0);
    lv_obj_add_event_cb(conn, on_wifi_connect_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(conn);
    lv_label_set_text(cl, "Connect");
    lv_obj_set_style_text_color(cl, PP_WHITE, 0);
    lv_obj_center(cl);

    s_wifi_ap_lbl = lv_label_create(s_scr_wifi);
    lv_label_set_text(s_wifi_ap_lbl, "");
    lv_label_set_long_mode(s_wifi_ap_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wifi_ap_lbl, fw);
    lv_obj_set_style_text_color(s_wifi_ap_lbl, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_wifi_ap_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_wifi_ap_lbl, LV_ALIGN_TOP_LEFT, fx, 232);

    /* Scan list: left half in landscape, full width below the form in portrait. */
    s_wifi_list = lv_list_create(s_scr_wifi);
    if (P) {
        lv_obj_set_size(s_wifi_list, scr_w(), scr_h() - 300);
        lv_obj_align(s_wifi_list, LV_ALIGN_TOP_LEFT, 0, 290);
    } else {
        lv_obj_set_size(s_wifi_list, 380, 480 - 56);
        lv_obj_align(s_wifi_list, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    lv_obj_set_style_bg_color(s_wifi_list, PP_BG, 0);
    lv_obj_set_style_border_width(s_wifi_list, 0, 0);

    s_wifi_kb = lv_keyboard_create(s_scr_wifi);
    lv_obj_add_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- bottom navigation (persistent, Panda-Touch style) ---------- */
static void nav_dash(lv_event_t *e)     { lv_screen_load(s_scr_dash); }
static void nav_detail(lv_event_t *e)   { lv_screen_load(s_scr_status); }
static void nav_files(lv_event_t *e)    { app_state_post_cmd(s_files_usb_mode ? PP_CMD_LIST_USB : PP_CMD_LIST, NULL); lv_screen_load(s_scr_files); }
static void nav_settings(lv_event_t *e) { refresh_printers_list(); lv_screen_load(s_scr_printers); }

static void make_nav(lv_obj_t *scr, int active)
{
    static const char *labels[4] = { "Fleet", "Printer", "Files", "Settings" };
    const lv_event_cb_t cbs[4] = { nav_dash, nav_detail, nav_files, nav_settings };
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, LV_PCT(100), 60);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, PP_SURFACE, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(bar);
        /* Each tab takes an equal share of the bar width — even in both orientations (fixed-
         * width tabs overflowed and looked uneven in 480px portrait). */
        lv_obj_set_height(b, 52);
        lv_obj_set_width(b, 0);
        lv_obj_set_flex_grow(b, 1);
        /* Connect-style minimalist nav: transparent items, the active one marked by an
         * orange underline (not a filled pill), inactive labels muted. */
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        if (i == active) {
            lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(b, PP_ORANGE, 0);
            lv_obj_set_style_border_width(b, 3, 0);
        } else {
            lv_obj_set_style_border_width(b, 0, 0);
        }
        lv_obj_add_event_cb(b, cbs[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_color(l, i == active ? PP_TEXT : PP_TEXT_MUTED, 0);
        lv_obj_center(l);
    }
}

/* ---------- fleet dashboard ---------- */
static void on_card_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_dash_count) {   /* index may be stale after a remove */
        app_state_select_printer(idx);
        /* Render the clicked printer's cached status immediately so the detail screen
         * doesn't show the previously-selected printer for the seconds it takes the next
         * cloud poll to land. ui_apply_status runs on this (LVGL) thread and frees copy. */
        if (s_dash_items) {
            pp_status_t *copy = malloc(sizeof(*copy));
            if (copy) { *copy = s_dash_items[idx]; ui_apply_status(copy); }
        }
    }
    lv_screen_load(s_scr_status);   /* open this printer's detail */
}

/* Printer-model renders (rasterized from Prusa Connect's SVG icons → LVGL images). */
extern const lv_image_dsc_t pt_core_one;
extern const lv_image_dsc_t pt_core_one_l;
extern const lv_image_dsc_t pt_mini;
extern const lv_image_dsc_t pt_mk4s;
extern const lv_image_dsc_t pt_xl;
extern const lv_image_dsc_t pt_fluidd;   /* Klipper / Moonraker printers */

/* Pick the model image for a friendly model string (NULL → show placeholder). */
static const lv_image_dsc_t *model_image(const char *model)
{
    if (!model || !model[0]) return NULL;
    if (strstr(model, "CORE One L")) return &pt_core_one_l;   /* before "CORE One" */
    if (strstr(model, "CORE One"))   return &pt_core_one;
    if (strstr(model, "MINI"))       return &pt_mini;
    if (strstr(model, "MK4S"))       return &pt_mk4s;
    if (strstr(model, "XL"))         return &pt_xl;
    if (strstr(model, "Klipper"))    return &pt_fluidd;       /* Moonraker backend */
    return NULL;
}

/* One Connect-style telemetry cell: muted uppercase label over a bold white value. */
static lv_obj_t *card_cell(lv_obj_t *parent, int x, int y, const char *label, const char *value)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, PP_TEXT, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, x, y + 17);
    return v;   /* the value label, for in-place dashboard updates */
}

/* Format the 4 telemetry values exactly as the card shows them (shared by build + in-place update). */
static void fmt_telemetry(const pp_status_t *s, char *nz, char *hb, char *sp, char *zx)
{
    if (s->online) {
        if ((int)s->target_nozzle >= 1) sprintf(nz, "%d/%d\xC2\xB0""C", (int)s->temp_nozzle, (int)s->target_nozzle);
        else sprintf(nz, "%d\xC2\xB0""C", (int)s->temp_nozzle);
        if ((int)s->target_bed >= 1) sprintf(hb, "%d/%d\xC2\xB0""C", (int)s->temp_bed, (int)s->target_bed);
        else sprintf(hb, "%d\xC2\xB0""C", (int)s->temp_bed);
        sprintf(sp, "%d%%", s->speed);
        sprintf(zx, "%.2fmm", s->axis_z);
    } else { strcpy(nz, "--"); strcpy(hb, "--"); strcpy(sp, "--"); strcpy(zx, "--"); }
}

/* Per-card widget handles captured at build time so a poll that changes only values can update
 * them in place (gist #11) instead of destroying + rebuilding every card (flicker + CPU). */
typedef struct {
    lv_obj_t *strip, *badge, *badge_lbl, *name_lbl, *model_lbl;
    lv_obj_t *v_noz, *v_speed, *v_bed, *v_z, *prog_bar, *prog_lbl;
} dash_refs_t;
static dash_refs_t s_dref[PP_MAX_PRINTERS];
static int      s_dref_n;        /* number of cards currently laid out */
static uint32_t s_dash_sig;      /* structural signature of the current layout */
static bool     s_dash_have;     /* a valid prior layout exists */

/* ---- Snapshot-cached cards ----
 * Scrolling the fleet was render-bound (~10 FPS): every frame software-rendered each card's
 * rounded-corner masks, ~10 labels of text, and (PNG-decoded!) thumbnail. Instead, each card's
 * live widget tree now lives on a hidden host screen and is rendered ONCE per data change into
 * a PSRAM bitmap; the visible grid holds plain lv_image widgets showing those bitmaps, so a
 * scroll frame is just a few opaque blits. Cards are pixel-identical (corners bake against the
 * grid background). If a bitmap can't be allocated, that slot falls back to a live card in the
 * grid, exactly the old behavior. */
#define DASH_CARD_W 380
#define DASH_CARD_H 170
static lv_obj_t      *s_card_host;                    /* hidden screen hosting the live cards  */
static lv_obj_t      *s_card_wrap[PP_MAX_PRINTERS];   /* per-slot wrapper on the host          */
static lv_obj_t      *s_card_img [PP_MAX_PRINTERS];   /* per-slot image widget in the grid     */
static lv_draw_buf_t *s_card_snap[PP_MAX_PRINTERS];   /* per-slot RGB565 bitmap (PSRAM, reused) */

/* Re-render slot's live card into its bitmap and refresh the grid image showing it. */
static void dash_snapshot_slot(int slot)
{
    if (slot < 0 || slot >= PP_MAX_PRINTERS) return;
    if (!s_card_wrap[slot] || !s_card_snap[slot] || !s_card_img[slot]) return;   /* live-card fallback slot */
    lv_obj_update_layout(s_card_wrap[slot]);
    if (lv_snapshot_take_to_draw_buf(s_card_wrap[slot], LV_COLOR_FORMAT_RGB565, s_card_snap[slot]) == LV_RESULT_OK) {
        lv_image_cache_drop(s_card_snap[slot]);       /* content changed under the same pointer */
        lv_image_set_src(s_card_img[slot], s_card_snap[slot]);
        lv_obj_invalidate(s_card_img[slot]);
    }
}

/* A publish landing mid-gesture would re-snapshot cards and invalidate images, stealing frames
 * from the scroll — park it and apply the newest one when the scroll (incl. momentum) settles. */
static bool       s_dash_scrolling;
static pp_dash_t *s_dash_pending;    /* newest publish deferred during a scroll (we own/free it) */

static void on_dash_scroll(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL_BEGIN) {
        s_dash_scrolling = true;
    } else if (code == LV_EVENT_SCROLL_END && s_dash_scrolling) {
        s_dash_scrolling = false;
        if (s_dash_pending) {           /* apply the publish that arrived mid-scroll */
            pp_dash_t *d = s_dash_pending;
            s_dash_pending = NULL;
            ui_apply_dashboard(d);      /* takes ownership and frees it */
        }
    }
}

/* Set a label only if the text actually differs; reports whether anything changed so the
 * caller can skip the (whole-card) re-snapshot when a publish was a visual no-op. */
static bool lbl_set_if_changed(lv_obj_t *lbl, const char *txt)
{
    if (!lbl || strcmp(lv_label_get_text(lbl), txt) == 0) return false;
    lv_label_set_text(lbl, txt);
    return true;
}

static bool update_dash_card(const dash_refs_t *r, const pp_status_t *s)
{
    bool ch = false;
    bool online = s->online;
    const char *st = online ? (s->state[0] ? s->state : "READY") : "OFFLINE";
    /* The state text uniquely determines the strip/badge tints ("OFFLINE" covers the online
     * flag), so colors only need refreshing when the badge text changes. */
    if (r->badge_lbl && strcmp(lv_label_get_text(r->badge_lbl), st) != 0) {
        lv_label_set_text(r->badge_lbl, st);
        if (r->strip) lv_obj_set_style_bg_color(r->strip, online ? pp_state_strip(s->state) : PP_STRIP_GRAY, 0);
        if (r->badge) lv_obj_set_style_bg_color(r->badge, online ? pp_state_badge(s->state) : PP_BADGE_GRAY, 0);
        ch = true;
    }
    ch |= lbl_set_if_changed(r->name_lbl, s->printer_name[0] ? s->printer_name : "Printer");
    ch |= lbl_set_if_changed(r->model_lbl, s->model[0] ? s->model : (online ? "Prusa printer" : ""));
    char nz[24], hb[24], sp[16], zx[16];
    fmt_telemetry(s, nz, hb, sp, zx);
    ch |= lbl_set_if_changed(r->v_noz, nz);
    ch |= lbl_set_if_changed(r->v_speed, sp);
    ch |= lbl_set_if_changed(r->v_bed, hb);
    ch |= lbl_set_if_changed(r->v_z, zx);
    if (s->has_job && r->prog_bar) {
        int pct = (int)(s->progress + 0.5f);
        if (lv_bar_get_value(r->prog_bar) != pct) {
            lv_bar_set_value(r->prog_bar, pct, LV_ANIM_OFF);
            ch = true;
        }
        if (r->prog_lbl) {
            char pb[8];
            snprintf(pb, sizeof(pb), "%d%%", pct);
            ch |= lbl_set_if_changed(r->prog_lbl, pb);
        }
    }
    return ch;
}

/* Structural fingerprint: which printers, in what order, online/job/firmware/thumbnail state.
 * Excludes the churning values (temps/progress/...) so those go through the in-place path. */
static uint32_t dash_sig(const pp_dash_t *d, const int *order, int n, bool hide_off)
{
    uint32_t h = 2166136261u;
#define MIX(x) do { h ^= (uint32_t)(x); h *= 16777619u; } while (0)
    MIX(d->conn_expired ? 1 : 0); MIX(hide_off ? 1 : 0);
    int shown = 0;
    for (int k = 0; k < n; k++) {
        int idx = order[k];
        if (hide_off && !d->items[idx].online) continue;
        const pp_status_t *s = &d->items[idx];
        MIX(idx); MIX(s->online ? 1 : 0); MIX(s->has_job ? 1 : 0); MIX(s->firmware[0] ? 1 : 0);
        MIX(s_card_thumbs[idx].buf ? 1 : 0);          /* thumb arrival forces a rebuild to show it */
        for (const char *p = s->job_thumb; *p; p++) MIX(*p);
        shown++;
    }
    MIX(shown);
    return h;
#undef MIX
}

/* Prusa Connect dark-card anatomy: state-tinted header strip (name + badge),
 * then a 3-column labeled telemetry grid; progress bar slot when printing. */
static void make_printer_card(lv_obj_t *parent, const pp_status_t *s, int idx, dash_refs_t *r)
{
    if (r) memset(r, 0, sizeof(*r));
    const bool online = s->online;
    const char *st = online ? (s->state[0] ? s->state : "READY") : "OFFLINE";

    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 380, 170);
    lv_obj_set_style_bg_color(c, PP_SURFACE, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_clip_corner(c, true, 0);    /* round the header strip too */
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, on_card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    if (!online) lv_obj_set_style_opa(c, LV_OPA_70, 0);   /* dim offline cards */

    /* ---- header strip: name (white) + flush state badge (muted tint, white text) ---- */
    lv_obj_t *head = lv_obj_create(c);
    lv_obj_set_size(head, 380, 34);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(head, 0, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_bg_color(head, online ? pp_state_strip(s->state) : PP_STRIP_GRAY, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    if (r) r->strip = head;

    lv_obj_t *badge = lv_obj_create(head);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, 34);
    lv_obj_set_style_pad_hor(badge, 12, 0);
    lv_obj_set_style_pad_ver(badge, 0, 0);
    lv_obj_set_style_radius(badge, 0, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_bg_color(badge, online ? pp_state_badge(s->state) : PP_BADGE_GRAY, 0);
    lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    if (r) r->badge = badge;
    lv_obj_t *bl = lv_label_create(badge);
    lv_label_set_text(bl, st);
    lv_obj_set_style_text_color(bl, PP_TEXT, 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    if (r) r->badge_lbl = bl;

    lv_obj_t *nm = lv_label_create(head);
    lv_label_set_text(nm, s->printer_name[0] ? s->printer_name : "Printer");
    lv_obj_set_style_text_color(nm, PP_TEXT, 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, 226);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 12, 0);
    if (r) r->name_lbl = nm;

    /* ---- identity row: thumbnail slot + model + firmware ---- */
    lv_obj_t *thumb = lv_obj_create(c);
    lv_obj_set_size(thumb, 48, 48);
    lv_obj_align(thumb, LV_ALIGN_TOP_LEFT, 12, 36);
    lv_obj_set_style_bg_color(thumb, PP_SURFACE_HI, 0);
    lv_obj_set_style_border_width(thumb, 0, 0);
    lv_obj_set_style_radius(thumb, 4, 0);
    lv_obj_set_style_pad_all(thumb, 0, 0);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);

    const lv_image_dsc_t *img = model_image(s->model);
    bool has_job_thumb = (s->has_job && s->job_thumb[0]);

    if (has_job_thumb) {
        if (strcmp(s->job_thumb, s_card_thumbs[idx].url) != 0) {
            /* New URL! Clear old one for this slot and fetch. */
            if (s_card_thumbs[idx].buf) {
                lv_image_cache_drop(&s_card_thumbs[idx].dsc);
                free(s_card_thumbs[idx].buf);
                s_card_thumbs[idx].buf = NULL;
            }
            strlcpy(s_card_thumbs[idx].url, s->job_thumb, sizeof(s_card_thumbs[idx].url));
            lv_memzero(&s_card_thumbs[idx].dsc, sizeof(lv_image_dsc_t));
            app_state_fetch_thumb_dash(s->job_thumb, idx);
        }

        if (s_card_thumbs[idx].buf) {
            lv_obj_t *jt = lv_image_create(thumb);
            lv_image_set_src(jt, &s_card_thumbs[idx].dsc);
            lv_image_header_t hdr;
            if (lv_image_decoder_get_info(&s_card_thumbs[idx].dsc, &hdr) == LV_RESULT_OK
                && hdr.w > 0 && hdr.h > 0) {
                uint32_t scale = (48u * 256u) / (hdr.w > hdr.h ? hdr.w : hdr.h);
                if (scale > 256) scale = 256;
                lv_image_set_scale(jt, scale);
            }
            lv_obj_center(jt);
        } else if (img) {
            lv_obj_t *mi = lv_image_create(thumb);
            lv_image_set_src(mi, img);
            lv_obj_center(mi);
        }
    } else {
        /* No job; clear slot cache if it was occupied. */
        if (s_card_thumbs[idx].url[0]) {
             if (s_card_thumbs[idx].buf) {
                lv_image_cache_drop(&s_card_thumbs[idx].dsc);
                free(s_card_thumbs[idx].buf);
                s_card_thumbs[idx].buf = NULL;
            }
            s_card_thumbs[idx].url[0] = '\0';
        }

        if (img) {
            lv_obj_t *mi = lv_image_create(thumb);
            lv_image_set_src(mi, img);
            lv_obj_center(mi);
        } else {
            lv_obj_t *ti = lv_label_create(thumb);
            lv_label_set_text(ti, LV_SYMBOL_IMAGE);
            lv_obj_set_style_text_color(ti, PP_TEXT_MUTED, 0);
            lv_obj_center(ti);
        }
    }

    lv_obj_t *md = lv_label_create(c);
    lv_label_set_text(md, s->model[0] ? s->model : (online ? "Prusa printer" : ""));
    lv_obj_set_style_text_color(md, PP_TEXT, 0);
    lv_obj_set_style_text_font(md, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(md, LV_LABEL_LONG_DOT);
    lv_obj_set_width(md, 300);
    lv_obj_align(md, LV_ALIGN_TOP_LEFT, 66, 40);
    if (r) r->model_lbl = md;

    if (s->firmware[0]) {
        lv_obj_t *fwl = lv_label_create(c);
        lv_label_set_text_fmt(fwl, "Firmware: %s", s->firmware);
        lv_obj_set_style_text_color(fwl, PP_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(fwl, &lv_font_montserrat_12, 0);
        lv_label_set_long_mode(fwl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(fwl, 300);
        lv_obj_align(fwl, LV_ALIGN_TOP_LEFT, 66, 59);
    }

    /* ---- 3-column labeled telemetry grid ---- */
    char nz[24], hb[24], sp[16], zx[16];
    fmt_telemetry(s, nz, hb, sp, zx);
    const int X1 = 14, X2 = 140, X3 = 266, R1 = 86, R2 = 124;
    lv_obj_t *vn = card_cell(c, X1, R1, "NOZZLE", nz);
    lv_obj_t *vs = card_cell(c, X2, R1, "SPEED",  sp);   /* Connect column order: NOZZLE / SPEED / BED */
    lv_obj_t *vb = card_cell(c, X3, R1, "BED",    hb);
    lv_obj_t *vz = card_cell(c, X1, R2, "Z AXIS", zx);
    if (r) { r->v_noz = vn; r->v_speed = vs; r->v_bed = vb; r->v_z = vz; }

    /* progress (when printing) fills the 2nd/3rd column of row 2 */
    if (s->has_job) {
        int pct = (int)(s->progress + 0.5f);
        lv_obj_t *pl = lv_label_create(c);
        lv_label_set_text(pl, "PROGRESS");
        lv_obj_set_style_text_color(pl, PP_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(pl, &lv_font_montserrat_12, 0);
        lv_obj_align(pl, LV_ALIGN_TOP_LEFT, X2, R2);

        lv_obj_t *bar = lv_bar_create(c);
        lv_obj_set_size(bar, 140, 10);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, X2, R2 + 20);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, PP_SURFACE_HI, 0);
        lv_obj_set_style_bg_color(bar, PP_ORANGE, LV_PART_INDICATOR);

        lv_obj_t *pv = lv_label_create(c);
        lv_label_set_text_fmt(pv, "%d%%", pct);
        lv_obj_set_style_text_color(pv, PP_TEXT, 0);
        lv_obj_set_style_text_font(pv, &lv_font_montserrat_16, 0);
        lv_obj_align(pv, LV_ALIGN_TOP_LEFT, X3, R2 + 14);
        if (r) { r->prog_bar = bar; r->prog_lbl = pv; }
    }
}

/* Build one dashboard slot: the live card on the hidden host plus a snapshot image in the
 * grid. Falls back to the old behavior (live card directly in the grid) if the bitmap can't
 * be allocated. The wrapper's plain PP_BG background is what the card's rounded corners bake
 * against, matching the grid background exactly. */
static void dash_build_slot(int slot, const pp_status_t *s, int idx)
{
    if (!s_card_snap[slot])
        s_card_snap[slot] = lv_draw_buf_create(DASH_CARD_W, DASH_CARD_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!s_card_host || !s_card_snap[slot]) {
        make_printer_card(s_dash_grid, s, idx, &s_dref[slot]);   /* fallback: live card */
        return;
    }
    lv_obj_t *wrap = lv_obj_create(s_card_host);
    lv_obj_set_size(wrap, DASH_CARD_W, DASH_CARD_H);
    lv_obj_set_style_bg_color(wrap, PP_BG, 0);
    lv_obj_set_style_radius(wrap, 0, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    make_printer_card(wrap, s, idx, &s_dref[slot]);
    s_card_wrap[slot] = wrap;

    lv_obj_t *img = lv_image_create(s_dash_grid);
    lv_obj_set_size(img, DASH_CARD_W, DASH_CARD_H);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, on_card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    s_card_img[slot] = img;
    dash_snapshot_slot(slot);
}

/* Wordmark: white-outlined box with [ PRUSA | TOUCH ] over a small "by NomadsGalaxy"
 * byline, stacked so it fits the standard header height. */
static void make_wordmark(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);   /* shrinks when byline hidden */
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(box, PP_WHITE, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 2, 0);
    lv_obj_set_style_pad_hor(box, 10, 0);
    lv_obj_set_style_pad_ver(box, 2, 0);
    lv_obj_set_style_pad_row(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(box, LV_ALIGN_LEFT_MID, 8, 0);
    /* Tapping the wordmark always returns to the fleet dashboard (home). */
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, nav_dash, LV_EVENT_CLICKED, NULL);

    /* top line: PRUSA | TOUCH */
    lv_obj_t *row = lv_obj_create(box);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);   /* clicks reach the box -> home */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *p = lv_label_create(row);
    lv_label_set_text(p, "PRUSA");
    lv_obj_set_style_text_color(p, PP_WHITE, 0);
    lv_obj_set_style_text_font(p, &lv_font_montserrat_16, 0);

    lv_obj_t *divr = lv_obj_create(row);
    lv_obj_set_size(divr, 2, 18);
    lv_obj_set_style_bg_color(divr, PP_WHITE, 0);
    lv_obj_set_style_border_width(divr, 0, 0);
    lv_obj_set_style_radius(divr, 0, 0);
    lv_obj_clear_flag(divr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, "TOUCH");
    lv_obj_set_style_text_color(t, PP_WHITE, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);

    /* bottom line: by NomadsGalaxy */
    lv_obj_t *by = lv_label_create(box);
    lv_label_set_text(by, "by NomadsGalaxy");
    lv_obj_set_style_text_color(by, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(by, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(by, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Track for the logo preference; hide the byline in single-line mode. */
    if (s_byline_count < (int)(sizeof(s_bylines) / sizeof(s_bylines[0])))
        s_bylines[s_byline_count++] = by;
    if (prefs_logo() == PP_LOGO_SINGLE) lv_obj_add_flag(by, LV_OBJ_FLAG_HIDDEN);
}

/* Show/hide all wordmark bylines to match the current logo preference. Scheduled on
 * the LVGL thread by app_state (after the NVS write on the net task). arg unused. */
void ui_apply_logo(void *unused)
{
    (void)unused;
    bool single = (prefs_logo() == PP_LOGO_SINGLE);
    for (int i = 0; i < s_byline_count; i++) {
        if (!s_bylines[i] || !lv_obj_is_valid(s_bylines[i])) continue;
        if (single) lv_obj_add_flag(s_bylines[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_remove_flag(s_bylines[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_apply_orient(void *unused)
{
    (void)unused;
    lv_display_rotation_t r;
    switch (prefs_orient()) {
        case PP_ORIENT_LANDSCAPE_FLIPPED: r = LV_DISPLAY_ROTATION_180; break;
        case PP_ORIENT_PORTRAIT:          r = LV_DISPLAY_ROTATION_90;  break;
        case PP_ORIENT_PORTRAIT_FLIPPED:  r = LV_DISPLAY_ROTATION_270; break;
        default:                          r = LV_DISPLAY_ROTATION_0;   break;
    }
    lv_display_set_rotation(lv_display_get_default(), r);
}

/* ---------- screen lock (opt-in) ----------
 * After N idle minutes the screen "locks": browsing stays open, but action callbacks call
 * ui_locked_block() which pops a PIN prompt and bails. The overlay lives on the top layer so
 * it floats over whichever screen is active, in either orientation. */
static bool        s_locked;
static lv_obj_t   *s_lock_modal;
static lv_obj_t   *s_lock_ta;
static lv_obj_t   *s_lock_msg;
static lv_obj_t   *s_lock_ind;
static lv_timer_t *s_lock_timer;

static void lock_release(void)
{
    s_locked = false;
    if (s_lock_ind)   lv_obj_add_flag(s_lock_ind, LV_OBJ_FLAG_HIDDEN);
    if (s_lock_modal) lv_obj_add_flag(s_lock_modal, LV_OBJ_FLAG_HIDDEN);
}

static void lock_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_locked) return;
    uint8_t m = prefs_lock_min();
    if (m == 0 || !prefs_scrpin()[0]) return;
    if (lv_display_get_inactive_time(NULL) > (uint32_t)m * 60000) {
        s_locked = true;
        if (s_lock_ind) lv_obj_remove_flag(s_lock_ind, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_apply_lock_cfg(void *unused)
{
    (void)unused;
    bool want = (prefs_lock_min() > 0 && prefs_scrpin()[0]);
    if (want && !s_lock_timer)        s_lock_timer = lv_timer_create(lock_timer_cb, 5000, NULL);
    else if (!want && s_lock_timer) { lv_timer_delete(s_lock_timer); s_lock_timer = NULL; lock_release(); }
}

static void on_pin_ok(lv_event_t *e)
{
    (void)e;
    if (strcmp(lv_textarea_get_text(s_lock_ta), prefs_scrpin()) == 0) lock_release();
    else { lv_label_set_text(s_lock_msg, "Wrong PIN, try again"); lv_textarea_set_text(s_lock_ta, ""); }
}
static void on_pin_cancel(lv_event_t *e)
{
    (void)e;
    if (s_lock_modal) lv_obj_add_flag(s_lock_modal, LV_OBJ_FLAG_HIDDEN);   /* stays locked; just dismiss */
}

static void lock_show_prompt(void)
{
    if (!s_lock_modal) return;
    lv_textarea_set_text(s_lock_ta, "");
    lv_label_set_text(s_lock_msg, "Enter PIN to unlock");
    lv_obj_remove_flag(s_lock_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lock_modal);
}

static bool ui_locked_block(void)
{
    if (!s_locked) return false;
    lock_show_prompt();
    return true;
}

/* Tapping the LOCKED badge brings up the PIN prompt without having to poke an action first. */
static void on_lock_badge(lv_event_t *e) { (void)e; lock_show_prompt(); }

/* Public: lock the screen immediately (e.g. a future "Lock now" affordance / sim preview).
 * Browsing stays available; the prompt appears on an action or a badge tap. */
void ui_lock_now(void)
{
    if (prefs_scrpin()[0]) {
        s_locked = true;
        if (s_lock_ind) lv_obj_remove_flag(s_lock_ind, LV_OBJ_FLAG_HIDDEN);
    }
}
void ui_show_lock_prompt(void) { lock_show_prompt(); }

static void build_lock_overlay(void)
{
    lv_obj_t *top = lv_layer_top();

    /* small "LOCKED" badge, top-right; hidden until the screen locks */
    s_lock_ind = lv_label_create(top);
    lv_label_set_text(s_lock_ind, LV_SYMBOL_BELL " LOCKED");
    lv_obj_set_style_text_color(s_lock_ind, PP_ORANGE, 0);
    lv_obj_set_style_text_font(s_lock_ind, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(s_lock_ind, PP_HEADER, 0);
    lv_obj_set_style_bg_opa(s_lock_ind, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_lock_ind, 6, 0);
    lv_obj_set_style_radius(s_lock_ind, 4, 0);
    lv_obj_align(s_lock_ind, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_add_flag(s_lock_ind, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lock_ind, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lock_ind, on_lock_badge, LV_EVENT_CLICKED, NULL);

    /* PIN-entry modal: full-screen backdrop + message + password field + number keypad */
    s_lock_modal = lv_obj_create(top);
    lv_obj_set_size(s_lock_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_lock_modal, PP_BG, 0);
    lv_obj_set_style_bg_opa(s_lock_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lock_modal, 0, 0);
    lv_obj_set_style_radius(s_lock_modal, 0, 0);
    lv_obj_clear_flag(s_lock_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lock_modal, LV_OBJ_FLAG_HIDDEN);

    s_lock_msg = lv_label_create(s_lock_modal);
    lv_label_set_text(s_lock_msg, "Enter PIN to unlock");
    lv_obj_set_style_text_color(s_lock_msg, PP_TEXT, 0);
    lv_obj_set_style_text_font(s_lock_msg, &lv_font_montserrat_20, 0);
    lv_obj_align(s_lock_msg, LV_ALIGN_TOP_MID, 0, 40);

    s_lock_ta = lv_textarea_create(s_lock_modal);
    lv_textarea_set_one_line(s_lock_ta, true);
    lv_textarea_set_password_mode(s_lock_ta, true);
    lv_textarea_set_placeholder_text(s_lock_ta, "PIN");
    lv_obj_set_width(s_lock_ta, 240);
    lv_obj_align(s_lock_ta, LV_ALIGN_TOP_MID, 0, 84);

    lv_obj_t *cancel = make_barbtn(s_lock_modal, LV_SYMBOL_CLOSE " Cancel", on_pin_cancel, NULL, 120);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -8, 8);

    lv_obj_t *kb = lv_keyboard_create(s_lock_modal);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, s_lock_ta);
    lv_obj_add_event_cb(kb, on_pin_ok, LV_EVENT_READY, NULL);   /* the keypad's check key = unlock */
}

static void build_dashboard_screen(void)
{
    s_scr_dash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_dash, PP_BG, 0);
    make_header(s_scr_dash, NULL);   /* same builder as every screen → wordmark never shifts */

    s_dash_grid = lv_obj_create(s_scr_dash);
    lv_obj_set_size(s_dash_grid, LV_PCT(100), scr_h() - 56 - 60);   /* fill between header + nav */
    lv_obj_align(s_dash_grid, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(s_dash_grid, PP_BG, 0);
    lv_obj_set_style_border_width(s_dash_grid, 0, 0);
    lv_obj_set_style_pad_all(s_dash_grid, 8, 0);
    lv_obj_set_flex_flow(s_dash_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_dash_grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    /* Defer publishes while the grid is scrolling (applied on scroll-end). */
    lv_obj_add_event_cb(s_dash_grid, on_dash_scroll, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_dash_grid, on_dash_scroll, LV_EVENT_SCROLL_END, NULL);

    /* Hidden screen (never loaded) hosting the live card widget trees the grid's bitmap
     * cards are snapshotted from. */
    s_card_host = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_card_host, PP_BG, 0);
}

/* Lower rank sorts earlier when grouping by status. */
static int dash_state_rank(const pp_status_t *s)
{
    if (!s->online) return 5;
    switch (pp_state_class(s->state)) {
    case PP_SC_ORANGE: return 0;   /* printing / attention */
    case PP_SC_YELLOW: return 1;   /* paused */
    case PP_SC_RED:    return 2;   /* error / stopped */
    case PP_SC_GREEN:  return 3;   /* finished */
    case PP_SC_OLIVE:  return 4;   /* ready */
    default:           return 4;   /* idle / busy */
    }
}

static const pp_status_t *s_dash_sort_items;   /* set before qsort (single-threaded UI) */
static int dash_order_cmp(const void *pa, const void *pb)
{
    const pp_status_t *a = &s_dash_sort_items[*(const int *)pa];
    const pp_status_t *b = &s_dash_sort_items[*(const int *)pb];
    switch (prefs_sort()) {
    case PP_SORT_NAME:
        return strcmp(a->printer_name, b->printer_name);
    case PP_SORT_MODEL: {
        int m = strcmp(a->model, b->model);
        return m ? m : strcmp(a->printer_name, b->printer_name);
    }
    case PP_SORT_PROGRESS: {
        int aj = a->online && a->has_job, bj = b->online && b->has_job;
        if (aj && bj) { float d = b->progress - a->progress; return (d > 0) - (d < 0); }
        if (aj != bj) return bj - aj;                    /* printing first */
        int r = dash_state_rank(a) - dash_state_rank(b);
        return r ? r : strcmp(a->printer_name, b->printer_name);
    }
    case PP_SORT_STATUS:
    default: {
        int r = dash_state_rank(a) - dash_state_rank(b);
        return r ? r : strcmp(a->printer_name, b->printer_name);
    }
    }
}

void ui_apply_dashboard(void *arg)
{
    pp_dash_t *d = (pp_dash_t *)arg;
    /* Mid-gesture: park the newest publish, applied from the scroll-end handler. The
     * lv_obj_is_scrolling() check keeps a lost SCROLL_END (e.g. a screen switch mid-throw)
     * from parking publishes forever. */
    if (s_dash_scrolling && s_dash_grid && lv_obj_is_scrolling(s_dash_grid)) {
        free(s_dash_pending);
        s_dash_pending = d;
        return;
    }
    s_dash_count = d->count;

    int n = d->count; if (n > PP_MAX_PRINTERS) n = PP_MAX_PRINTERS;
    if (!s_dash_items)   /* one-time PSRAM alloc — keeps ~30KB off the scarce internal heap (mbedTLS needs it) */
        s_dash_items = heap_caps_malloc(PP_MAX_PRINTERS * sizeof(pp_status_t), MALLOC_CAP_SPIRAM);
    if (s_dash_items) for (int i = 0; i < n; i++) s_dash_items[i] = d->items[i];   /* snapshot for instant card-open */
    int order[PP_MAX_PRINTERS];
    for (int i = 0; i < n; i++) order[i] = i;
    s_dash_sort_items = d->items;
    if (n > 1) qsort(order, n, sizeof(int), dash_order_cmp);

    bool hide_off = prefs_hide_offline();
    uint32_t sig = dash_sig(d, order, n, hide_off);

    /* Fast path (gist #11): structure unchanged -> update the existing cards' values in place.
     * No lv_obj_clean / rebuild means no flicker, preserved scroll, and far less CPU. */
    if (s_dash_have && sig == s_dash_sig) {
        int slot = 0;
        for (int k = 0; k < n && slot < s_dref_n; k++) {
            int idx = order[k];
            if (hide_off && !d->items[idx].online) continue;
            if (update_dash_card(&s_dref[slot], &d->items[idx]))
                dash_snapshot_slot(slot);   /* re-render the bitmap only when something visible changed */
            slot++;
        }
        free(d);
        return;
    }

    /* Slow path: the structure changed (count/order/online/job/firmware/thumbnail) -> rebuild.
     * Bitmap buffers (s_card_snap) are slot-sized and survive rebuilds; only the widget trees
     * (grid images + host cards) are recreated. */
    int32_t scroll_y = lv_obj_get_scroll_y(s_dash_grid);   /* keep scroll position across rebuild */
    lv_obj_clean(s_dash_grid);
    if (s_card_host) lv_obj_clean(s_card_host);
    memset(s_card_wrap, 0, sizeof(s_card_wrap));
    memset(s_card_img,  0, sizeof(s_card_img));
    s_dref_n = 0;

    /* Connect sign-in lapsed: prepend a full-width re-connect banner (flex ROW_WRAP gives it
     * its own row above the cards). No credential entry on-device — the user re-authenticates
     * from the web Account tab; local PrusaLink fallback keeps configured printers reachable. */
    if (d->conn_expired) {
        lv_obj_t *bn = lv_obj_create(s_dash_grid);
        lv_obj_set_width(bn, LV_PCT(100));
        lv_obj_set_height(bn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(bn, PP_ORANGE, 0);
        lv_obj_set_style_bg_opa(bn, LV_OPA_20, 0);
        lv_obj_set_style_border_color(bn, PP_ORANGE, 0);
        lv_obj_set_style_border_width(bn, 1, 0);
        lv_obj_set_style_radius(bn, 6, 0);
        lv_obj_set_style_pad_all(bn, 10, 0);
        lv_obj_clear_flag(bn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *bl = lv_label_create(bn);
        lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(bl, LV_PCT(100));
        lv_obj_set_style_text_color(bl, PP_TEXT, 0);
        if (wifi_is_connected() && wifi_ip_str()[0])
            lv_label_set_text_fmt(bl, LV_SYMBOL_WARNING "  Prusa Connect sign-in expired. "
                "Reconnect from http://%s/ \xE2\x86\x92 Account. Local printers stay reachable.",
                wifi_ip_str());
        else
            lv_label_set_text(bl, LV_SYMBOL_WARNING "  Prusa Connect sign-in expired. "
                "Reconnect from the web Account tab. Local printers stay reachable.");
    }

    int shown = 0;
    for (int k = 0; k < n; k++) {
        int idx = order[k];                              /* original store index */
        if (hide_off && !d->items[idx].online) continue;
        if (shown < PP_MAX_PRINTERS) dash_build_slot(shown, &d->items[idx], idx);
        else make_printer_card(s_dash_grid, &d->items[idx], idx, NULL);
        shown++;
    }
    if (shown == 0) {
        lv_obj_t *l = lv_label_create(s_dash_grid);
        lv_label_set_text(l, d->count == 0 ? "No printers yet — add one in Settings."
                                           : "No printers match the current filter.");
        lv_obj_set_style_text_color(l, PP_TEXT_MUTED, 0);
    }
    s_dref_n = shown;
    s_dash_sig = sig;
    s_dash_have = true;

    /* Restore scroll position. */
    lv_obj_update_layout(s_dash_grid);   /* ensure children positions are calculated */
    lv_obj_scroll_to_y(s_dash_grid, scroll_y, LV_ANIM_OFF);

    free(d);
}

/* ---------- About / attribution (satisfies SWAtt v1 UI requirement) ---------- */
static void on_about_back(lv_event_t *e) { lv_screen_load(s_scr_printers); }

static void on_about_open(lv_event_t *e) { lv_screen_load(s_scr_about); }

static void on_preheat_clicked(lv_event_t *e)
{
    if (ui_locked_block()) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_state_post_cmd_n(PP_CMD_PREHEAT, idx, 0, 0);
}

/* Jog buttons carry a 2-char token in user_data: "X+","X-","Y+","Y-","Z+","Z-", or "HM"
 * (home). We post backend-agnostic intent (PP_CMD_MOVE / PP_CMD_HOME); the net task picks
 * dedicated Connect commands vs gcode per the active printer's backend. */
static void on_jog_clicked(lv_event_t *e)
{
    if (ui_locked_block()) return;
    const char *tok = (const char *)lv_event_get_user_data(e);
    if (tok[0] == 'H') { app_state_post_cmd_n(PP_CMD_HOME, 0, 0, 0); return; }
    int axis = (tok[0] == 'X') ? 0 : (tok[0] == 'Y') ? 1 : 2;
    int dist = (tok[1] == '-') ? -10 : 10;          /* mm */
    int feed = (axis == 2) ? 600 : 3000;            /* Z slower */
    app_state_post_cmd_n(PP_CMD_MOVE, axis, dist * 100, feed);   /* i32a = dist*100 */
}

static void on_snapshot_clicked(lv_event_t *e)
{
    (void)e;
    if (s_snap_ph) lv_label_set_text(s_snap_ph, "Loading\xE2\x80\xA6");   /* "Loading…" */
    app_state_fetch_snapshot();   /* -> prusa_connect_fetch_snapshot -> ui_apply_snapshot */
}

static void build_control_screen(void)
{
    s_scr_control = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_control, PP_BG, 0);

    lv_obj_t *bar = make_header(s_scr_control, "Control");
    lv_obj_t *back = make_barbtn(bar, LV_SYMBOL_LEFT " Back", on_control_back, NULL, 100);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, -8, 0);

    /* Temperatures Card */
    lv_obj_t *temp_card = make_card(s_scr_control, 380, 180);
    lv_obj_align(temp_card, LV_ALIGN_TOP_LEFT, 16, 72);
    lv_obj_t *tl = lv_label_create(temp_card);
    lv_label_set_text(tl, "PREHEAT");
    lv_obj_set_style_text_color(tl, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_14, 0);

    const char *mats[] = { "PLA", "PETG", "ASA", "Cooldown" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = make_button(temp_card, mats[i], on_preheat_clicked, (void *)(intptr_t)i, NULL);
        lv_obj_set_size(b, 160, 50);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, (i % 2) * 180, 30 + (i / 2) * 60);
    }

    /* Jog Card — landscape: top-right beside PREHEAT; portrait: stacked below the Z controls. */
    lv_obj_t *jog_card = make_card(s_scr_control, 380, 240);
    if (ui_portrait()) lv_obj_align(jog_card, LV_ALIGN_TOP_LEFT, 16, 326);
    else               lv_obj_align(jog_card, LV_ALIGN_TOP_RIGHT, -16, 72);
    lv_obj_set_style_pad_all(jog_card, 0, 0);   /* predictable absolute coords */
    lv_obj_t *jl = lv_label_create(jog_card);
    lv_label_set_text(jl, "MOVE");
    lv_obj_set_style_text_color(jl, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(jl, &lv_font_montserrat_14, 0);
    lv_obj_align(jl, LV_ALIGN_TOP_LEFT, 12, 8);

    /* X / Y / home jog pad — clean 3x3 cross with gaps so outlines never touch */
    lv_obj_t *byp  = make_button(jog_card, LV_SYMBOL_UP   " Y+", on_jog_clicked, "Y+", NULL);
    lv_obj_t *bxm  = make_button(jog_card, LV_SYMBOL_LEFT " X-", on_jog_clicked, "X-", NULL);
    lv_obj_t *home = make_button(jog_card, LV_SYMBOL_HOME,       on_jog_clicked, "HM", NULL);
    lv_obj_t *bxp  = make_button(jog_card, LV_SYMBOL_RIGHT " X+",on_jog_clicked, "X+", NULL);
    lv_obj_t *bym  = make_button(jog_card, LV_SYMBOL_DOWN " Y-", on_jog_clicked, "Y-", NULL);
    lv_obj_set_size(byp, 76, 52); lv_obj_set_size(bxm, 76, 52); lv_obj_set_size(home, 76, 52);
    lv_obj_set_size(bxp, 76, 52); lv_obj_set_size(bym, 76, 52);
    lv_obj_align(byp,  LV_ALIGN_TOP_LEFT, 152, 40);
    lv_obj_align(bxm,  LV_ALIGN_TOP_LEFT, 66,  96);
    lv_obj_align(home, LV_ALIGN_TOP_LEFT, 152, 96);
    lv_obj_align(bxp,  LV_ALIGN_TOP_LEFT, 238, 96);
    lv_obj_align(bym,  LV_ALIGN_TOP_LEFT, 152, 152);

    /* Z controls */
    lv_obj_t *bzp = make_button(s_scr_control, "Z+ 10", on_jog_clicked, "Z+", NULL);
    lv_obj_t *bzm = make_button(s_scr_control, "Z- 10", on_jog_clicked, "Z-", NULL);
    lv_obj_set_size(bzp, 120, 50); lv_obj_set_size(bzm, 120, 50);
    lv_obj_align(bzp, LV_ALIGN_TOP_LEFT, 16, 260);
    lv_obj_align(bzm, LV_ALIGN_TOP_LEFT, 150, 260);

    /* Webcam card (bottom-right free area) — live snapshot from Connect, JPEG-decoded
     * on-device (CONFIG_LV_USE_TJPGD). Parity with the web UI's webcam modal. */
    lv_obj_t *cam_card = make_card(s_scr_control, 372, 150);
    lv_obj_align(cam_card, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_t *caml = lv_label_create(cam_card);
    lv_label_set_text(caml, "WEBCAM");
    lv_obj_set_style_text_color(caml, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(caml, &lv_font_montserrat_14, 0);
    lv_obj_align(caml, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *cam_btn = make_button(cam_card, "Load", on_snapshot_clicked, NULL, NULL);
    lv_obj_set_size(cam_btn, 84, 34);
    lv_obj_align(cam_btn, LV_ALIGN_TOP_RIGHT, 0, -4);

    s_snap_ph = lv_label_create(cam_card);
    lv_label_set_text(s_snap_ph, "Tap Load for the live camera");
    lv_obj_set_style_text_color(s_snap_ph, PP_TEXT_MUTED, 0);
    lv_obj_align(s_snap_ph, LV_ALIGN_BOTTOM_MID, 0, -8);

    s_snap_img = lv_image_create(cam_card);
    /* Shown 1:1 — Connect's camera thumbnail is ~250x140, which fits this card. TJPGD is a
     * partial/line decoder and does NOT support lv_image_set_scale (transform needs a full
     * buffer), so we must not scale it. */
    lv_obj_align(s_snap_img, LV_ALIGN_CENTER, 0, 8);
    lv_obj_add_flag(s_snap_img, LV_OBJ_FLAG_HIDDEN);
}

static void build_about_screen(void)
{
    s_scr_about = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_about, PP_BG, 0);
    lv_obj_clear_flag(s_scr_about, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bar = make_header(s_scr_about, "About / License");

    lv_obj_t *back = make_barbtn(bar, LV_SYMBOL_LEFT " Back", on_about_back, NULL, 100);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, -8, 0);

    /* Landscape: text left, QR right. Portrait: text on top, QR centered below it. */
    const bool P = ui_portrait();

    /* ---- product + license text ---- */
    lv_obj_t *title = lv_label_create(s_scr_about);
    lv_label_set_text(title, "Prusa Touch");
    lv_obj_set_style_text_color(title, PP_ORANGE, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 74);

    lv_obj_t *by = lv_label_create(s_scr_about);
    lv_label_set_text(by, "by NomadsGalaxy");
    lv_obj_set_style_text_color(by, PP_TEXT, 0);
    lv_obj_set_style_text_font(by, &lv_font_montserrat_16, 0);
    lv_obj_align(by, LV_ALIGN_TOP_LEFT, 16, 108);

    lv_obj_t *body = lv_label_create(s_scr_about);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, P ? scr_w() - 32 : 470);
    lv_label_set_text(body,
        "Firmware " PP_FW_VERSION "\n"
        "Open-firmware touchscreen for Prusa printers (PrusaLink).\n\n"
        "License: OCL v1.1 + SWAtt v1\n"
        "Built on PandaTouch_IDF (BigTreeTech, MIT).\n\n"
        "Independent community project - not affiliated with\n"
        "or endorsed by Prusa Research. \"Prusa\" and \"Prusa\n"
        "Connect\" are trademarks of Prusa Research.");
    lv_obj_set_style_text_color(body, PP_TEXT_MUTED, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 16, 140);

    /* ---- right column: GitHub QR + URL ---- */
    static const char *repo = "https://github.com/nomadsgalaxy/Prusa-Connect-Touch";
    lv_obj_t *qr = lv_qrcode_create(s_scr_about);
    lv_qrcode_set_size(qr, 170);
    lv_qrcode_set_dark_color(qr, PP_BLACK);
    lv_qrcode_set_light_color(qr, PP_WHITE);
    lv_qrcode_update(qr, repo, strlen(repo));
    lv_obj_set_style_border_color(qr, PP_WHITE, 0);
    lv_obj_set_style_border_width(qr, 6, 0);      /* white quiet-zone border */
    if (P) lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 340);
    else   lv_obj_align(qr, LV_ALIGN_TOP_RIGHT, -70, 110);

    lv_obj_t *qcap = lv_label_create(s_scr_about);
    lv_label_set_text(qcap, "Scan for the project on GitHub");
    lv_obj_set_style_text_color(qcap, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(qcap, &lv_font_montserrat_14, 0);
    if (P) lv_obj_align(qcap, LV_ALIGN_TOP_MID, 0, 524);
    else   lv_obj_align(qcap, LV_ALIGN_TOP_RIGHT, -40, 292);

    lv_obj_t *url = lv_label_create(s_scr_about);
    lv_label_set_text(url, "github.com/nomadsgalaxy/\nPrusa-Connect-Touch");
    lv_obj_set_style_text_color(url, PP_TEXT, 0);
    lv_obj_set_style_text_font(url, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(url, LV_TEXT_ALIGN_CENTER, 0);
    if (P) lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 548);
    else   lv_obj_align(url, LV_ALIGN_TOP_RIGHT, -55, 314);
}

/* ---------- Prusa Farm (org-wide printer + order status) ---------- */
static void on_farm_back(lv_event_t *e) { (void)e; lv_screen_load(s_scr_printers); }

static void build_farm_screen(void)
{
    s_scr_farm = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_farm, PP_BG, 0);
    lv_obj_clear_flag(s_scr_farm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bar = make_header(s_scr_farm, "Prusa Farm");
    lv_obj_t *back = make_barbtn(bar, LV_SYMBOL_LEFT " Back", on_farm_back, NULL, 100);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, -8, 0);

    s_farm_stat = lv_label_create(s_scr_farm);
    lv_label_set_text(s_farm_stat, "Loading Prusa Farm...");
    lv_obj_set_style_text_color(s_farm_stat, PP_TEXT, 0);
    lv_obj_set_style_text_font(s_farm_stat, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_farm_stat, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_farm_stat, scr_w() - 32);
    lv_obj_align(s_farm_stat, LV_ALIGN_TOP_LEFT, 16, 70);

    s_farm_list = lv_obj_create(s_scr_farm);
    lv_obj_set_size(s_farm_list, scr_w(), scr_h() - 132);
    lv_obj_align(s_farm_list, LV_ALIGN_TOP_LEFT, 0, 124);
    lv_obj_set_style_bg_color(s_farm_list, PP_BG, 0);
    lv_obj_set_style_border_width(s_farm_list, 0, 0);
    lv_obj_set_style_pad_all(s_farm_list, 16, 0);
    lv_obj_set_flex_flow(s_farm_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_farm_list, 8, 0);
}

void ui_apply_farm(void *arg)
{
    pp_farm_t *f = (pp_farm_t *)arg;
    if (!lv_obj_is_valid(s_farm_stat)) { free(f); return; }
    if (!f->valid && f->order_count == 0) {
        lv_label_set_text(s_farm_stat, "Prusa Farm unavailable. Set your Organization ID "
                          "in the web UI (Farm tab), then reopen.");
    } else {
        lv_label_set_text_fmt(s_farm_stat,
                              "Printers:  %d active   %d online   %d total%s   |   Orders: %d",
                              f->p_active, f->p_online, f->p_total,
                              f->p_error ? "   (errors!)" : "", f->order_count);
    }
    if (lv_obj_is_valid(s_farm_list)) {
        lv_obj_clean(s_farm_list);
        for (int i = 0; i < f->order_count; i++) {
            lv_obj_t *card = lv_obj_create(s_farm_list);
            lv_obj_set_width(card, LV_PCT(100));
            lv_obj_set_height(card, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(card, PP_SURFACE, 0);
            lv_obj_set_style_border_width(card, 0, 0);
            lv_obj_set_style_radius(card, 6, 0);
            lv_obj_set_style_pad_all(card, 10, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *l = lv_label_create(card);
            lv_obj_set_style_text_color(l, PP_TEXT, 0);
            lv_label_set_text_fmt(l, "%s   -   done %d/%d%s",
                                  f->orders[i].name[0] ? f->orders[i].name : "(order)",
                                  f->orders[i].done, f->orders[i].total,
                                  f->orders[i].attn ? "   needs attention" : "");
        }
    }
    free(f);
}

static void on_farm_open(lv_event_t *e)
{
    (void)e;
    if (lv_obj_is_valid(s_farm_stat)) lv_label_set_text(s_farm_stat, "Loading Prusa Farm...");
    if (lv_obj_is_valid(s_farm_list)) lv_obj_clean(s_farm_list);
    app_state_farm_refresh();
    lv_screen_load(s_scr_farm);
}

/* ---------- Preferences (sort / filter / logo) ---------- */
static void on_prefs_back(lv_event_t *e) { (void)e; lv_screen_load(s_scr_printers); }

/* All pref writes go through the net task (PSRAM-stack LVGL task can't touch flash). */
static void on_pref_sort_changed(lv_event_t *e)
{
    app_state_set_pref(PP_PREF_SORT, (int)lv_dropdown_get_selected(lv_event_get_target(e)));
}
static void on_pref_hideoff_changed(lv_event_t *e)
{
    app_state_set_pref(PP_PREF_HIDE_OFFLINE,
                       lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0);
}
static void on_pref_logo_changed(lv_event_t *e)
{
    app_state_set_pref(PP_PREF_LOGO, (int)lv_dropdown_get_selected(lv_event_get_target(e)));
}
static void on_pref_autoupd_changed(lv_event_t *e)
{
    app_state_set_pref(PP_PREF_AUTOUPDATE,
                       lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0);
}
/* Switching between a landscape class (0,1) and a portrait class (2,3) changes the screen
 * resolution, which needs a reboot to re-lay-out. Warn + confirm first; a same-class flip
 * (0<->1 / 2<->3) applies live with no reboot. */
static int s_pending_orient = -1;

static bool orient_is_portrait(int o) { return o == PP_ORIENT_PORTRAIT || o == PP_ORIENT_PORTRAIT_FLIPPED; }

static void orient_confirm_cb(lv_event_t *e)
{
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    if (s_pending_orient >= 0) app_state_set_pref(PP_PREF_ORIENT, s_pending_orient);   /* reboots */
    lv_msgbox_close(mbox);
}
static void orient_cancel_cb(lv_event_t *e)
{
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    lv_dropdown_set_selected(s_pref_orient_dd, (uint16_t)prefs_orient());   /* revert the picker */
    lv_msgbox_close(mbox);
}

static void on_pref_orient_changed(lv_event_t *e)
{
    int sel = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    if (orient_is_portrait(sel) == orient_is_portrait((int)prefs_orient())) {
        app_state_set_pref(PP_PREF_ORIENT, sel);   /* same class: live rotate, no reboot */
        return;
    }
    s_pending_orient = sel;
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, "Restart required");
    lv_msgbox_add_text(mbox, orient_is_portrait(sel)
        ? "Switching to portrait restarts the device to re-lay-out the screens. Continue?"
        : "Switching to landscape restarts the device to re-lay-out the screens. Continue?");
    lv_obj_t *ok = lv_msgbox_add_footer_button(mbox, "Restart");
    lv_obj_set_style_bg_color(ok, PP_ORANGE, 0);
    lv_obj_add_event_cb(ok, orient_confirm_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_t *cancel = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_add_event_cb(cancel, orient_cancel_cb, LV_EVENT_CLICKED, mbox);
}

static lv_obj_t *pref_label(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 24, y);
    return l;
}

/* Dark-theme a dropdown (closed button + open list) to match the Connect UI. */
static void dropdown_dark(lv_obj_t *dd)
{
    lv_obj_set_style_bg_color(dd, PP_SURFACE, 0);
    lv_obj_set_style_text_color(dd, PP_TEXT, 0);
    lv_obj_set_style_border_color(dd, PP_BORDER, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_radius(dd, 4, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_bg_color(list, PP_SURFACE, 0);
        lv_obj_set_style_text_color(list, PP_TEXT, 0);
        lv_obj_set_style_border_color(list, PP_BORDER, 0);
        lv_obj_set_style_bg_color(list, PP_ORANGE, LV_PART_SELECTED | LV_STATE_CHECKED);
    }
}

static void build_prefs_screen(void)
{
    s_scr_prefs = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_prefs, PP_BG, 0);
    lv_obj_clear_flag(s_scr_prefs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bar = make_header(s_scr_prefs, "Preferences");
    lv_obj_t *back = make_barbtn(bar, LV_SYMBOL_LEFT " Back", on_prefs_back, NULL, 100);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, -8, 0);

    /* Landscape uses a 2nd column (x=420) for orientation; portrait stacks it under the rest. */
    const bool P = ui_portrait();

    /* Sort fleet by */
    pref_label(s_scr_prefs, "Sort fleet by", 84);
    s_pref_sort_dd = lv_dropdown_create(s_scr_prefs);
    lv_dropdown_set_options(s_pref_sort_dd, "Status\nName\nModel\nCompletion %");
    lv_obj_set_width(s_pref_sort_dd, 320);
    lv_obj_align(s_pref_sort_dd, LV_ALIGN_TOP_LEFT, 24, 112);
    dropdown_dark(s_pref_sort_dd);
    lv_obj_add_event_cb(s_pref_sort_dd, on_pref_sort_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* Hide offline */
    pref_label(s_scr_prefs, "Hide offline printers", 188);
    s_pref_hideoff_sw = lv_switch_create(s_scr_prefs);
    lv_obj_align(s_pref_hideoff_sw, LV_ALIGN_TOP_LEFT, 24, 214);
    lv_obj_set_style_bg_color(s_pref_hideoff_sw, PP_ORANGE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_pref_hideoff_sw, on_pref_hideoff_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* Header logo */
    pref_label(s_scr_prefs, "Header logo", 290);
    s_pref_logo_dd = lv_dropdown_create(s_scr_prefs);
    lv_dropdown_set_options(s_pref_logo_dd, "PRUSA|TOUCH + byline\nPRUSA|TOUCH (single line)");
    lv_obj_set_width(s_pref_logo_dd, 320);
    lv_obj_align(s_pref_logo_dd, LV_ALIGN_TOP_LEFT, 24, 318);
    dropdown_dark(s_pref_logo_dd);
    lv_obj_add_event_cb(s_pref_logo_dd, on_pref_logo_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* Automatic updates (opt-in; off by default) */
    pref_label(s_scr_prefs, "Automatic firmware updates", 392);
    s_pref_autoupd_sw = lv_switch_create(s_scr_prefs);
    lv_obj_align(s_pref_autoupd_sw, LV_ALIGN_TOP_LEFT, 24, 418);
    lv_obj_set_style_bg_color(s_pref_autoupd_sw, PP_ORANGE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_pref_autoupd_sw, on_pref_autoupd_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* Screen orientation — landscape: right column; portrait: stacked under auto-updates */
    lv_obj_t *ol = lv_label_create(s_scr_prefs);
    lv_label_set_text(ol, "Screen orientation");
    lv_obj_set_style_text_color(ol, PP_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(ol, &lv_font_montserrat_14, 0);
    lv_obj_align(ol, LV_ALIGN_TOP_LEFT, P ? 24 : 420, P ? 484 : 84);
    s_pref_orient_dd = lv_dropdown_create(s_scr_prefs);
    lv_dropdown_set_options(s_pref_orient_dd, "Landscape\nLandscape (flipped)\nPortrait\nPortrait (flipped)");
    lv_obj_set_width(s_pref_orient_dd, 320);
    lv_obj_align(s_pref_orient_dd, LV_ALIGN_TOP_LEFT, P ? 24 : 420, P ? 512 : 112);
    dropdown_dark(s_pref_orient_dd);
    lv_obj_add_event_cb(s_pref_orient_dd, on_pref_orient_changed, LV_EVENT_VALUE_CHANGED, NULL);
}

static void on_prefs_open(lv_event_t *e)
{
    (void)e;
    lv_dropdown_set_selected(s_pref_sort_dd, (uint16_t)prefs_sort());
    lv_dropdown_set_selected(s_pref_logo_dd, (uint16_t)prefs_logo());
    if (prefs_hide_offline()) lv_obj_add_state(s_pref_hideoff_sw, LV_STATE_CHECKED);
    else                      lv_obj_remove_state(s_pref_hideoff_sw, LV_STATE_CHECKED);
    if (prefs_auto_update()) lv_obj_add_state(s_pref_autoupd_sw, LV_STATE_CHECKED);
    else                     lv_obj_remove_state(s_pref_autoupd_sw, LV_STATE_CHECKED);
    lv_dropdown_set_selected(s_pref_orient_dd, (uint16_t)prefs_orient());
    lv_screen_load(s_scr_prefs);
}

static void build_boot_screen(void)
{
    s_scr_boot = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_boot, lv_color_hex(0x111316), 0);
    lv_obj_clear_flag(s_scr_boot, LV_OBJ_FLAG_SCROLLABLE);

    /* PRUSA | TOUCH */
    lv_obj_t *l1 = lv_label_create(s_scr_boot);
    lv_label_set_text(l1, "PRUSA | TOUCH");
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(l1, lv_color_white(), 0);
    lv_obj_align(l1, LV_ALIGN_CENTER, 0, -40);

    /* By NomadsGalaxy */
    lv_obj_t *l2 = lv_label_create(s_scr_boot);
    lv_label_set_text(l2, "By NomadsGalaxy");
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l2, PP_ORANGE, 0);
    lv_obj_align(l2, LV_ALIGN_CENTER, 0, 10);

    /* Loading bar */
    s_boot_bar = lv_bar_create(s_scr_boot);
    lv_obj_set_size(s_boot_bar, 400, 12);
    lv_obj_align(s_boot_bar, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_bg_color(s_boot_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_boot_bar, PP_ORANGE, LV_PART_INDICATOR);
    lv_bar_set_value(s_boot_bar, 0, LV_ANIM_OFF);

    s_boot_status = lv_label_create(s_scr_boot);
    lv_label_set_text(s_boot_status, "Starting...");
    lv_obj_set_style_text_font(s_boot_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_boot_status, PP_TEXT_MUTED, 0);
    lv_obj_align(s_boot_status, LV_ALIGN_CENTER, 0, 110);
}

void ui_boot_update(int progress, const char *status)
{
    PT_LVGL_SCOPE_LOCK() {
        if (s_boot_bar) lv_bar_set_value(s_boot_bar, progress, LV_ANIM_OFF);
        if (s_boot_status && status) lv_label_set_text(s_boot_status, status);
    }
}

/* Auto-refresh the Control screen's webcam preview while it's on screen. The snapshot
 * decode (TJPGD) is cheap and the JPEG buffer lives in PSRAM, so this is light. */
static void webcam_refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_screen_active() == s_scr_control) app_state_fetch_snapshot();
}

void ui_init(void)
{
    card_thumbs_clear();
    /* Apply the saved orientation BEFORE building screens so resolution-aware sizing
     * (scr_w()/scr_h(), LV_PCT containers) lays out for portrait (480x800) when selected. */
    ui_apply_orient(NULL);
    build_boot_screen();
    lv_screen_load(s_scr_boot);

    build_dashboard_screen();
    build_status_screen();
    build_files_screen();
    build_filedetail_screen();
    build_printers_screen();
    build_addform_screen();
    build_addpick_screen();
    build_control_screen();
    build_wifi_screen();
    build_about_screen();
    build_prefs_screen();
    build_farm_screen();
    ui_apply_orient(NULL);   /* apply the saved screen orientation */
    /* persistent bottom nav on the primary screens */
    make_nav(s_scr_dash, 0);
    make_nav(s_scr_status, 1);
    make_nav(s_scr_files, 2);
    make_nav(s_scr_printers, 3);

    build_lock_overlay();    /* PIN-entry overlay on the top layer (hidden until locked) */
    ui_apply_lock_cfg(NULL); /* arm the idle-lock timer if the opt-in is configured */

    lv_timer_create(webcam_refresh_timer_cb, 7000, NULL);   /* live webcam on Control screen */
}

/* ---------- test/automation nav API ----------
 * ui_request_screen() is callable from any thread (e.g. the web server); it
 * marshals the actual lv_screen_load onto the LVGL thread via the BSP scheduler. */
static void ui_apply_nav(void *arg)
{
    char *name = (char *)arg;
    if (name) {
        if      (!strcmp(name, "dash")   || !strcmp(name, "fleet"))   lv_screen_load(s_scr_dash);
        else if (!strcmp(name, "status") || !strcmp(name, "printer")) lv_screen_load(s_scr_status);
        else if (!strcmp(name, "control"))                            lv_screen_load(s_scr_control);
        else if (!strcmp(name, "files"))  { app_state_post_cmd(s_files_usb_mode ? PP_CMD_LIST_USB : PP_CMD_LIST, NULL); lv_screen_load(s_scr_files); }
        else if (!strcmp(name, "printers") || !strcmp(name, "settings")) { refresh_printers_list(); lv_screen_load(s_scr_printers); }
        else if (!strcmp(name, "addpick"))                            lv_screen_load(s_scr_addpick);
        else if (!strcmp(name, "addform")) {   /* add mode, Bambu (shows the Serial field) */
            s_edit_idx = -1;
            lv_textarea_set_text(s_ta_name, ""); lv_textarea_set_text(s_ta_host, "");
            lv_textarea_set_text(s_ta_key, ""); lv_textarea_set_text(s_ta_serial, "");
            lv_obj_add_flag(s_btn_remove, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_btn_setactive, LV_OBJ_FLAG_HIDDEN);
            configure_add_form(2);
            lv_screen_load(s_scr_addform);
        }
        else if (!strcmp(name, "about"))                              lv_screen_load(s_scr_about);
        else if (!strcmp(name, "prefs"))                              on_prefs_open(NULL);
        else if (!strcmp(name, "farm"))                               on_farm_open(NULL);
        else if (!strcmp(name, "wifi"))   { wifi_screen_prepare(); lv_screen_load(s_scr_wifi); }
    }
    free(name);
}

void ui_request_screen(const char *name)
{
    if (!name || !name[0]) return;
    char *copy = malloc(24);
    if (!copy) return;
    strlcpy(copy, name, 24);
    if (pt_display_schedule_ui(ui_apply_nav, copy) != LV_RESULT_OK) free(copy);
}

const char *ui_current_screen(void)
{
    lv_obj_t *s = lv_screen_active();
    if (s == s_scr_dash)       return "dash";
    if (s == s_scr_status)     return "status";
    if (s == s_scr_control)    return "control";
    if (s == s_scr_files)      return "files";
    if (s == s_scr_filedetail) return "filedetail";
    if (s == s_scr_printers)   return "printers";
    if (s == s_scr_addform)    return "addform";
    if (s == s_scr_about)      return "about";
    if (s == s_scr_prefs)      return "prefs";
    if (s == s_scr_farm)       return "farm";
    if (s == s_scr_wifi)       return "wifi";
    return "unknown";
}

/* ---------- scheduled appliers (own + free arg) ---------- */
void ui_apply_status(void *arg)
{
    pp_status_t *s = (pp_status_t *)arg;
    char buf[64];

    if (s->printer_name[0]) {
        lv_label_set_text(s_title_lbl, s->printer_name);
        strlcpy(s_active_printer, s->printer_name, sizeof(s_active_printer));
    }
    strlcpy(s_active_model, s->model, sizeof(s_active_model));
    lv_obj_set_style_bg_color(s_conn_dot, s->online ? PP_OK : PP_ERROR, 0);
    wifi_status_label_refresh();   /* keep the Wi-Fi screen's IP line current */

    /* hero: model render on the orange tile, scaled to fill */
    const lv_image_dsc_t *mimg = model_image(s->model);
    if (mimg) {
        lv_image_set_src(s_detail_img, mimg);
        lv_image_set_scale(s_detail_img, 384);     /* 48px asset -> ~72px on tile */
        lv_obj_remove_flag(s_detail_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_detail_img, LV_OBJ_FLAG_HIDDEN);
    }

    /* hero: state-tinted strip + state badge (muted tint + white text) + model sub-line */
    lv_obj_set_style_bg_color(s_herotop, s->online ? pp_state_strip(s->state) : PP_STRIP_GRAY, 0);
    lv_obj_set_style_bg_color(s_badge, s->online ? pp_state_badge(s->state) : PP_BADGE_GRAY, 0);
    lv_label_set_text(s_state_lbl, s->online ? (s->state[0] ? s->state : "READY") : "OFFLINE");
    lv_label_set_text(s_model_lbl, s->model[0] ? s->model : "");

    /* telemetry cells */
    if (s->online) {
        if ((int)s->target_nozzle >= 1) snprintf(buf, sizeof(buf), "%d/%d\xC2\xB0""C", (int)s->temp_nozzle, (int)s->target_nozzle);
        else snprintf(buf, sizeof(buf), "%d\xC2\xB0""C", (int)s->temp_nozzle);
        lv_label_set_text(s_nozzle_lbl, buf);
        if ((int)s->target_bed >= 1) snprintf(buf, sizeof(buf), "%d/%d\xC2\xB0""C", (int)s->temp_bed, (int)s->target_bed);
        else snprintf(buf, sizeof(buf), "%d\xC2\xB0""C", (int)s->temp_bed);
        lv_label_set_text(s_bed_lbl, buf);
        snprintf(buf, sizeof(buf), "%d%%", s->speed);
        lv_label_set_text(s_speed_lbl, buf);
        snprintf(buf, sizeof(buf), "%.2fmm", s->axis_z);
        lv_label_set_text(s_z_lbl, buf);
    } else {
        lv_label_set_text(s_nozzle_lbl, "--");
        lv_label_set_text(s_bed_lbl, "--");
        lv_label_set_text(s_speed_lbl, "--");
        lv_label_set_text(s_z_lbl, "--");
    }

    if (s->has_job) {
        lv_label_set_text(s_job_lbl, s->job_name[0] ? s->job_name : "(printing)");
        lv_bar_set_value(s_bar, (int)(s->progress + 0.5f), LV_ANIM_ON);
        snprintf(buf, sizeof(buf), "%d%%", (int)(s->progress + 0.5f));
        lv_label_set_text(s_pct_lbl, buf);
        char eta[24];
        fmt_eta(s->time_remaining, eta, sizeof(eta));
        snprintf(buf, sizeof(buf), "ETA %s", eta);
        lv_label_set_text(s_eta_lbl, buf);
    } else {
        lv_label_set_text(s_job_lbl, s->online ? "No active print" : "");
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_pct_lbl, "");
        lv_label_set_text(s_eta_lbl, "");
    }

    /* Pause button reflects the paused/printing state. */
    bool paused = (strcmp(s->state, "PAUSED") == 0);
    lv_label_set_text(s_btn_pause_lbl, paused ? "RESUME" : "PAUSE");

    /* CONTROL button visibility based on capability probe. */
    if (s->has_control) lv_obj_remove_flag(s_btn_control, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_add_flag(s_btn_control, LV_OBJ_FLAG_HIDDEN);

    /* Attention dialog banner: when the printer has an active Connect dialog, surface its
     * title/text + action buttons over the (empty) job card and wire each button to DIALOG_ACTION. */
    if (s_attn_card) {
        if (s->dialog_id) {
            s_attn_dialog_id = s->dialog_id;
            lv_label_set_text(s_attn_title, s->dialog_title[0] ? s->dialog_title : "Attention");
            lv_label_set_text(s_attn_text, s->dialog_text);
            for (int i = 0; i < 3; i++) {
                if (i < s->dialog_btn_count && s->dialog_btns[i][0]) {
                    strlcpy(s_attn_btn_text[i], s->dialog_btns[i], sizeof(s_attn_btn_text[i]));
                    lv_label_set_text(s_attn_btn_lbls[i], s->dialog_btns[i]);
                    lv_obj_remove_flag(s_attn_btns[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    s_attn_btn_text[i][0] = '\0';
                    lv_obj_add_flag(s_attn_btns[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
            lv_obj_remove_flag(s_attn_card, LV_OBJ_FLAG_HIDDEN);
            if (s_jobcard) lv_obj_add_flag(s_jobcard, LV_OBJ_FLAG_HIDDEN);
        } else {
            s_attn_dialog_id = 0;
            lv_obj_add_flag(s_attn_card, LV_OBJ_FLAG_HIDDEN);
            if (s_jobcard) lv_obj_remove_flag(s_jobcard, LV_OBJ_FLAG_HIDDEN);
        }
    }

    free(s);
}

void ui_apply_files(void *arg)
{
    pp_file_list_t *list = (pp_file_list_t *)arg;

    lv_obj_clean(s_file_list);
    s_file_count = 0;

    /* Refresh the printer-context banner (these files belong to the active printer). */
    if (s_active_printer[0] && s_active_model[0]) {
        lv_label_set_text_fmt(s_files_banner, "Files on  %s   -   %s",
                              s_active_printer, s_active_model);
    } else if (s_active_printer[0]) {
        lv_label_set_text_fmt(s_files_banner, "Files on  %s", s_active_printer);
    }

    for (int i = 0; i < list->count && i < PP_MAX_FILES; i++) {
        if (!list->items[i].is_print) continue;   /* printable files only */
        s_files[s_file_count] = list->items[i];

        /* Connect-style row: file icon + bold name + muted meta (date · material). */
        lv_obj_t *row = lv_obj_create(s_file_list);
        lv_obj_set_size(row, LV_PCT(100), 58);
        lv_obj_set_style_bg_color(row, PP_SURFACE, 0);
        lv_obj_set_style_bg_color(row, PP_SURFACE_HI, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_file_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_file_count);

        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, LV_SYMBOL_FILE);
        lv_obj_set_style_text_color(ic, PP_ORANGE, 0);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, list->items[i].display);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nm, 700);
        lv_obj_set_style_text_color(nm, PP_TEXT, 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 36, 0);

        if (list->items[i].meta[0]) {
            lv_obj_t *mt = lv_label_create(row);
            lv_label_set_text(mt, list->items[i].meta);
            lv_obj_set_style_text_color(mt, PP_TEXT_MUTED, 0);
            lv_obj_set_style_text_font(mt, &lv_font_montserrat_12, 0);
            lv_obj_align(mt, LV_ALIGN_BOTTOM_LEFT, 36, 0);
        }
        s_file_count++;
    }
    if (s_file_count == 0) {
        lv_obj_t *empty = lv_label_create(s_file_list);
        lv_label_set_text(empty, "No printable files on this printer");
        lv_obj_set_style_text_color(empty, PP_TEXT_MUTED, 0);
    }
    free(list);
}

/* Display a fetched gcode thumbnail (PNG bytes). Takes ownership of the wrapper
 * and the PNG buffer; frees the wrapper, retains the buffer for the descriptor. */
void ui_apply_thumb(void *arg)
{
    pp_image_t *im = (pp_image_t *)arg;
    if (!im) return;

    /* Release whatever was on screen before. */
    thumb_clear();

    if (!im->data || im->len <= 0) {
        free(im->data);
        free(im);
        lv_label_set_text(s_thumb_ph, "Preview unavailable");
        return;
    }

    /* Adopt the PNG bytes; build a descriptor LVGL's lodepng decoder can read. */
    s_thumb_buf = im->data;
    int len = im->len;
    free(im);                  /* wrapper done; buffer now owned by s_thumb_buf */

    s_thumb_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_thumb_dsc.header.cf    = LV_COLOR_FORMAT_RAW;   /* encoded (PNG) source */
    s_thumb_dsc.header.w     = 0;                     /* filled by the decoder */
    s_thumb_dsc.header.h     = 0;
    s_thumb_dsc.data         = s_thumb_buf;
    s_thumb_dsc.data_size    = (uint32_t)len;

    /* Uniform downscale-to-fit (never upscale) within the 340x280 viewport. */
    lv_image_header_t hdr;
    uint32_t scale = LV_SCALE_NONE;   /* 256 = 1x */
    if (lv_image_decoder_get_info(&s_thumb_dsc, &hdr) == LV_RESULT_OK
        && hdr.w > 0 && hdr.h > 0) {
        uint32_t sx = (340u * LV_SCALE_NONE) / hdr.w;
        uint32_t sy = (280u * LV_SCALE_NONE) / hdr.h;
        scale = sx < sy ? sx : sy;
        if (scale > LV_SCALE_NONE) scale = LV_SCALE_NONE;
    }

    lv_obj_clear_flag(s_thumb_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_thumb_ph, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_thumb_img, &s_thumb_dsc);
    lv_image_set_scale(s_thumb_img, scale);
}

/* Webcam snapshot on the Control screen — same RAW-descriptor decode path as the gcode
 * thumbnail, but the bytes are JPEG (LVGL's TJPGD decoder, enabled via CONFIG_LV_USE_TJPGD).
 * Brings the touch to parity with the web's webcam modal. */
static void snap_clear(void)
{
    if (!s_snap_img) return;
    lv_image_set_src(s_snap_img, NULL);
    lv_image_cache_drop(&s_snap_dsc);
    if (s_snap_buf) { free(s_snap_buf); s_snap_buf = NULL; }
    lv_memzero(&s_snap_dsc, sizeof(s_snap_dsc));
    lv_obj_add_flag(s_snap_img, LV_OBJ_FLAG_HIDDEN);
    if (s_snap_ph) lv_obj_clear_flag(s_snap_ph, LV_OBJ_FLAG_HIDDEN);
}

/* Parse a JPEG's frame dimensions from its SOF marker. LVGL's TJPGD decoder reads w/h
 * from the image descriptor header for a memory source (it does NOT parse them itself),
 * so we must fill them in before handing the buffer over. Returns false if no SOF found. */
static bool jpeg_dims(const uint8_t *d, int len, uint16_t *w, uint16_t *h)
{
    int i = 2;   /* skip SOI (FFD8) */
    while (i + 9 < len) {
        if (d[i] != 0xFF) { i++; continue; }
        uint8_t m = d[i + 1];
        if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) ||
            (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF)) {   /* SOFn */
            *h = (uint16_t)((d[i + 5] << 8) | d[i + 6]);
            *w = (uint16_t)((d[i + 7] << 8) | d[i + 8]);
            return (*w > 0 && *h > 0);
        }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) { i += 2; continue; }   /* no length */
        if (m == 0xD9) break;                                             /* EOI */
        i += 2 + ((d[i + 2] << 8) | d[i + 3]);                            /* skip segment */
    }
    return false;
}

void ui_apply_snapshot(void *arg)
{
    pp_image_t *im = (pp_image_t *)arg;
    if (!im) return;
    if (!s_snap_img) { free(im->data); free(im); return; }   /* control screen not built */

    snap_clear();

    if (!im->data || im->len <= 0) {
        free(im->data); free(im);
        if (s_snap_ph) lv_label_set_text(s_snap_ph, "No camera / no recent frame");
        return;
    }

    s_snap_buf = im->data;
    int len = im->len;
    free(im);

    uint16_t jw = 0, jh = 0;
    if (!jpeg_dims(s_snap_buf, len, &jw, &jh)) {
        snap_clear();
        if (s_snap_ph) lv_label_set_text(s_snap_ph, "Snapshot unreadable");
        return;
    }

    s_snap_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_snap_dsc.header.cf     = LV_COLOR_FORMAT_RAW;   /* encoded (JPEG) source */
    s_snap_dsc.header.w      = jw;                    /* TJPGD reads dims from here */
    s_snap_dsc.header.h      = jh;
    s_snap_dsc.header.stride = (uint32_t)jw * 3;
    s_snap_dsc.data          = s_snap_buf;
    s_snap_dsc.data_size     = (uint32_t)len;
    (void)jw; (void)jh;   /* dims parsed for the header; displayed 1:1 (TJPGD can't be scaled) */

    lv_obj_clear_flag(s_snap_img, LV_OBJ_FLAG_HIDDEN);
    if (s_snap_ph) lv_obj_add_flag(s_snap_ph, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_snap_img, &s_snap_dsc);
}

void ui_apply_thumb_dash(void *arg)
{
    pp_thumb_dash_t *td = (pp_thumb_dash_t *)arg;
    if (!td) return;

    int i = td->index;
    if (i >= 0 && i < PP_MAX_PRINTERS && td->image && td->image->data) {
        /* Store in cache. */
        if (s_card_thumbs[i].buf) {
            lv_image_cache_drop(&s_card_thumbs[i].dsc);
            free(s_card_thumbs[i].buf);
        }
        s_card_thumbs[i].buf = td->image->data;

        s_card_thumbs[i].dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_card_thumbs[i].dsc.header.cf    = LV_COLOR_FORMAT_RAW;
        s_card_thumbs[i].dsc.header.w     = 0;
        s_card_thumbs[i].dsc.header.h     = 0;
        s_card_thumbs[i].dsc.data         = s_card_thumbs[i].buf;
        s_card_thumbs[i].dsc.data_size    = (uint32_t)td->image->len;

        free(td->image); /* wrapper done; buffer now owned by cache */

        /* Re-trigger dashboard refresh to show the new thumbnail. */
        app_state_refresh_dashboard();
    } else {
        if (td->image) { free(td->image->data); free(td->image); }
    }
    free(td);
}
