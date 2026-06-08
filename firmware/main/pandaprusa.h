#pragma once
/* Prusa-Touch — shared types across the firmware modules. */
#include <stdbool.h>
#include <stdint.h>

#define PP_FW_VERSION "0.3.3"

/* Which host API a printer speaks (auto-detected on first contact). */
typedef enum { PP_BK_UNKNOWN = 0, PP_BK_PRUSALINK, PP_BK_MOONRAKER } pp_backend_t;

/* A configured printer (see printers.h / printers.example.h). */
typedef struct {
    char name[24];
    char host[48];            /* IP or hostname                              */
    int  port;                /* usually 80                                  */
    char api_key[40];         /* X-Api-Key; empty => Digest fallback         */
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
    bool   has_control;       /* supports OctoPrint control endpoints        */
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

#define PP_MAX_FILES 60
#define PP_MAX_PRINTERS 64
