#pragma once
/* Prusa-Touch — shared types across the firmware modules. */
#include <stdbool.h>
#include <stdint.h>

/* Fork test builds: bump the last digit every test image so About / web UI / /api/info show
 * exactly which build is flashed. NOTE: 0.4.9.x parses below upstream's v0.5.0 release, so the
 * Firmware tab's GitHub check will offer an "update" — don't take it while testing (and leave
 * auto-update off, its default). */
#define PP_FW_VERSION "0.4.9.6"

/* Which host API a printer speaks (auto-detected on first contact). */
typedef enum { PP_BK_UNKNOWN = 0, PP_BK_PRUSALINK, PP_BK_MOONRAKER, PP_BK_PRUSA_CONNECT, PP_BK_BAMBU } pp_backend_t;

/* A configured printer (see printers.h / printers.example.h). */
typedef struct {
    char name[24];
    char host[48];            /* Primary: IP/host OR cloud:uuid              */
    int  port;                /* usually 80                                  */
    char api_key[40];         /* X-Api-Key; empty => Digest fallback         */
    char uuid[40];            /* detected UUID (for matching/cloud)          */
    char local_host[48];      /* Fallback: local IP if cloud primary         */
} pp_printer_t;

/* Snapshot of printer + job telemetry from GET /api/v1/status (+ /api/v1/job). */
typedef struct {
    bool   online;            /* last poll succeeded                         */
    char   printer_name[24];  /* active printer's friendly name              */
    char   state[16];         /* IDLE/PRINTING/PAUSED/FINISHED/ERROR/...      */
    float  temp_nozzle;
    float  target_nozzle;
    float  temp_bed;
    float  target_bed;
    int    speed;             /* print speed %                               */
    float  axis_z;            /* Z height mm                                 */
    bool   has_job;
    int    job_id;
    float  progress;          /* percent 0..100                              */
    int    time_remaining;    /* seconds (-1 if unknown)                     */
    int    time_printing;     /* seconds                                     */
    char   job_name[96];      /* display_name of the file being printed      */
    char   job_thumb[160];     /* refs.thumbnail URL (from /api/v1/job)       */
    char   model[28];         /* friendly model, e.g. "Original Prusa MK4S"  */
    char   firmware[24];      /* firmware version (empty if unavailable)     */
    char   uuid[40];          /* printer UUID (Prusa Connect)                */
    char   team[40];          /* Connect team/org name (cloud printers)      */
    int    team_id;           /* Connect team id (cloud printers)            */
    bool   has_control;       /* supports OctoPrint control endpoints        */
    bool   is_cloud;          /* last update came from Connect cloud         */
    char   local_ip[20];      /* printer's LAN IP (from Connect network_info) — PrusaLink fallback */
    char   link_key[40];      /* printer's PrusaLink API key (from Connect)  — PrusaLink fallback */
    /* Attention dialog (Connect dialog_info; populated for the active printer in ATTENTION) */
    int    dialog_id;         /* Connect dialog id (0 = no active dialog)    */
    char   dialog_title[32];  /* e.g. "Warning"                              */
    char   dialog_text[160];  /* the message body                            */
    char   dialog_btns[3][24];/* button labels (Connect sends an array)      */
    int    dialog_btn_count;  /* number of valid entries in dialog_btns      */
} pp_status_t;

/* One entry from a folder listing. */
typedef struct {
    char path[160];           /* path usable in print/start (display or name)*/
    char display[96];         /* human-friendly name                         */
    char thumb[128];          /* refs.thumbnail URL (PNG; auth'd) or empty   */
    char meta[40];            /* Connect-style row metadata (date + material)*/
    uint32_t mtime;           /* m_timestamp (epoch) — newest-first sort key  */
    bool is_folder;
    bool is_print;            /* type == PRINT_FILE                          */
} pp_file_t;

/* PNG bytes handed from the net task to the LVGL thread for decode/display. */
typedef struct { uint8_t *data; int len; } pp_image_t;

/* Prusa Farm snapshot (org-scoped) handed from the net task to the LVGL thread. */
typedef struct {
    bool valid;
    int  p_active, p_online, p_error, p_total;   /* printer counts */
    int  order_count;
    struct { char name[40]; int done, total, attn; } orders[8];
} pp_farm_t;

#define PP_MAX_FILES 60
#define PP_MAX_PRINTERS 16   /* fleet cap; sized to keep per-printer arrays off the scarce internal heap */
