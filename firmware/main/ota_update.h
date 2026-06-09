#pragma once
/* Prusa-Touch — GitHub Releases auto-updater (self-OTA from the project's repo). */
#include <stdbool.h>

typedef struct {
    char current[24];   /* this firmware's version            */
    char latest[40];    /* latest release tag on GitHub        */
    char url[300];      /* download URL of the .bin asset      */
    bool available;     /* latest != current and a .bin exists */
} ota_check_t;

/* Query the configured GitHub repo's latest release. Returns true on success
 * (fills *out). Needs WiFi up. */
bool ota_update_check(ota_check_t *out);

/* Download bin_url and OTA-flash it, then reboot. Runs to completion (blocking);
 * call from a dedicated task, not the LVGL thread. */
void ota_update_apply(const char *bin_url);

/* Current update progress (0..100), -1 if no update in progress, or -2 if the
 * last update failed (see ota_update_get_error for why). */
int ota_update_get_progress(void);

/* Human-readable reason for the last failure ("" when none). */
const char *ota_update_get_error(void);

/* Start the background opt-in auto-updater task. It stays idle unless the user
 * enables "Automatic updates" in Preferences (default off); when enabled and
 * online it periodically checks GitHub and OTA-flashes a newer release. */
void ota_update_start_auto(void);
