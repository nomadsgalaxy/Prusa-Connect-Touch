#pragma once
/*
 * Prusa-Touch — runtime printer store (NVS-backed, mutex-protected).
 *
 * Accessed from three tasks (net task, httpd web handlers, LVGL UI), so all
 * access is serialized and getters COPY OUT a pp_printer_t (never return live
 * pointers into the backing array, which remove() shifts).
 *
 * Printers are added on-device and persisted in NVS — never hardcoded. For bench
 * testing the store seeds one entry from Kconfig (PP_PRINTER_HOST/APIKEY) if NVS
 * is empty; those values live only in the local, gitignored sdkconfig.
 */
#include <stdbool.h>
#include "pandaprusa.h"   /* PP_MAX_PRINTERS */

void printer_store_init(void);                          /* load from NVS (+ seed) */
int  printer_store_count(void);
bool printer_store_get(int idx, pp_printer_t *out);     /* copy-out; true if valid */
bool printer_store_active_get(pp_printer_t *out);       /* copy active; true if any */
int  printer_store_active(void);                        /* active index, or -1     */
void printer_store_set_active(int idx);                 /* persists                */
int  printer_store_add(const pp_printer_t *p);          /* idx or -1; persists      */
bool printer_store_update(int idx, const pp_printer_t *p); /* edit in place; persists */
void printer_store_remove(int idx);                     /* persists                 */
void printer_store_clear(void);                          /* persists (wipes all)     */
