/* Prusa-Touch — web interface: settings, live status, and firmware OTA. */
#include "web.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "pandatouch_display.h"   /* pt_get_panel */
#include "cJSON.h"

#include "pandaprusa.h"
#include "app_state.h"
#include "printer_store.h"
#include "wifi.h"
#include "ota_update.h"
#include "ui.h"                  /* ui_request_screen / ui_current_screen (test nav) */

static const char *TAG = "web";

/* ---- Prusa-themed single-page UI (tabs: Status / Printers / Wi-Fi / Firmware) ---- */
static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Prusa Connect Touch</title><style>"
":root{--o:#FD5000}*{box-sizing:border-box;font-family:system-ui,Arial}"
"body{margin:0;background:#1c1e21;color:#f2f2f2}"
"header{background:#111316;color:#fff;padding:14px 18px;font-size:20px;font-weight:700;border-bottom:2px solid var(--o)}"
"nav{display:flex;background:#111316;border-bottom:1px solid #3a3a3a}"
"nav a{padding:12px 18px;color:#bbb;cursor:pointer;text-decoration:none}"
"nav a.on{color:var(--o);border-bottom:2px solid var(--o)}"
".tab{display:none;padding:18px;max-width:800px;margin:0 auto}.tab.on{display:block}"
".card{background:#2a2a2a;border-radius:6px;margin:12px 0;overflow:hidden;border:0}"
".c-head{height:34px;display:flex;align-items:center;padding-left:12px;font-size:18px;font-weight:600;color:#fff}"
".c-badge{margin-left:auto;height:34px;padding:0 12px;display:flex;align-items:center;font-size:14px;font-weight:700}"
".c-body{padding:12px}.c-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px}"
".c-cell b{display:block;font-size:10px;color:#a7a7a7;margin-bottom:2px}"
".c-cell div{font-size:16px;color:#fff}"
".green .c-head{background:#303D2D}.green .c-badge{background:#435239}"
".olive .c-head{background:#2E3731}.olive .c-badge{background:#404B3F}"
".gray .c-head{background:#323336}.gray .c-badge{background:#454545}"
".orange .c-head{background:#3D312B}.orange .c-badge{background:#554237}"
".blue .c-head{background:#2B333D}.blue .c-badge{background:#3C444F}"
".yellow .c-head{background:#3E3B2D}.yellow .c-badge{background:#564F39}"
".red .c-head{background:#3D2C2A}.red .c-badge{background:#553B35}"
"input,button{font-size:16px;padding:10px;border-radius:8px;border:1px solid #3a3a3a;background:#2a2a2a;color:#f2f2f2;margin:4px 0;width:100%}"
"button{background:var(--o);color:#fff;border:0;font-weight:700;cursor:pointer}"
".bar{height:10px;background:#4e4e4e;border-radius:5px;overflow:hidden;margin-top:8px}.bar>i{display:block;height:100%;background:var(--o)}"
".muted{color:#a7a7a7}</style></head><body>"
"<header>PRUSA CONNECT TOUCH</header>"
"<nav><a class=on onclick=\"t(0)\">Status</a><a onclick=\"t(1)\">Printers</a>"
"<a onclick=\"t(2)\">Wi-Fi</a><a onclick=\"t(3)\">Firmware</a><a onclick=\"t(4)\">Screen</a></nav>"
"<div class=tab id=t0><div id=stlist></div><div class=card id=dev></div></div>"
"<div class=tab id=t1><div class=card><b id=pftitle>Add printer</b>"
"<input id=pn placeholder=Name><input id=ph placeholder='IP / host'>"
"<input id=pk placeholder='API key (blank = keep when editing)'>"
"<button onclick=savp()>Save</button> <button onclick=newp()>New</button></div>"
"<div id=plist></div>"
"<div class=card><b>Backup & Restore</b>"
"<p class=muted>Export your fleet config to a file, or import a saved config (replaces current fleet).</p>"
"<button onclick=expc()>Export Config</button>"
"<input type=file id=icf accept=.json style=margin-top:12px><button onclick=impc()>Import Config</button></div></div>"
"<div class=tab id=t2><div class=card><b>Wi-Fi</b>"
"<input id=ws placeholder=SSID><input id=wp type=password placeholder=Password>"
"<button onclick=savew()>Save &amp; connect</button></div></div>"
"<div class=tab id=t3>"
"<div class=card><b>Auto-update from GitHub</b>"
"<div id=gh class=muted>Tap Check.</div>"
"<button onclick=chk()>Check for updates</button>"
"<button id=ub style=display:none onclick=applyu()>Update now</button></div>"
"<div class=card><b>Manual firmware upload</b>"
"<p class=muted>Upload a Prusa-Touch .bin. The device reboots into it.</p>"
"<input type=file id=fw accept=.bin><button onclick=ota()>Flash</button>"
"<div id=otalog class=muted></div></div></div>"
"<div class=tab id=t4><div class=card><b>Live screen</b> "
"<button onclick=shot()>Refresh</button>"
"<div class=muted>What the touchscreen is showing right now.</div>"
"<img id=shot style='max-width:100%;border:1px solid #4e4e4e;margin-top:8px'></div></div>"
"<script>"
"function t(i){for(let n=0;n<5;n++){document.getElementById('t'+n).className='tab'+(n==i?' on':'');"
"document.querySelectorAll('nav a')[n].className=(n==i?'on':'')}if(i==1)lp();if(i==4)shot()}"
"function shot(){document.getElementById('shot').src='/api/screen.bmp?t='+Date.now()}"
"async function st(){let L=await fetch('/api/fleet').then(x=>x.json());"
"const sc=s=>{s=(s||'').toUpperCase();if(s=='PRINTING'||s=='ATTENTION')return'orange';if(s=='PAUSED')return'yellow';if(s=='FINISHED')return'green';if(s=='READY')return'olive';if(s=='ERROR'||s=='STOPPED')return'red';if(s=='BUSY'||s=='PREPARING')return'blue';return'gray'};"
"document.getElementById('stlist').innerHTML=L.map(r=>{const c=r.online?sc(r.state):'gray';return '<div class=\"card '+c+'\">'+"
"'<div class=c-head>'+r.name+'<div class=c-badge>'+(r.online?r.state:'OFFLINE')+'</div></div>'+"
"'<div class=c-body>'+"
"'<div class=c-grid>'+"
"'<div class=c-cell><b>NOZZLE</b><div>'+(r.online?r.nozzle+(r.tnozzle>0?'/'+r.tnozzle:''):'--')+'&deg;C</div></div>'+"
"'<div class=c-cell><b>HEATBED</b><div>'+(r.online?r.bed+(r.tbed>0?'/'+r.tbed:''):'--')+'&deg;C</div></div>'+"
"'<div class=c-cell><b>SPEED</b><div>'+(r.online?r.speed+'%':'--')+'</div></div>'+"
"'<div class=c-cell><b>Z AXIS</b><div>'+(r.online?r.z.toFixed(2)+'mm':'--')+'&deg;C</div></div>'+"
"'<div class=c-cell style=grid-column:span 2><b>PROGRESS</b><div>'+(r.printing?r.progress+'%':'--')+'</div></div>'+"
"'</div>'+"
"(r.printing?('<p class=muted style=margin:12px 0 4px 0>'+r.job+'</p><div class=bar><i style=width:'+r.progress+'%></i></div>'):'')+"
"'</div></div>'}).join('')||'<div class=card style=padding:18px>No printers yet.</div>';"
"try{let d=await fetch('/api/info').then(x=>x.json());document.getElementById('dev').innerHTML="
"'<span class=muted>'+d.name+' '+d.fw+' &middot; heap '+Math.round(d.heap_free/1024)+'KB &middot; up '+d.uptime_s+'s</span>'}catch(e){}}"
"let PL=[],EI=-1;"
"function newp(){EI=-1;pn.value=ph.value=pk.value='';pftitle.textContent='Add printer'}"
"async function lp(){PL=await fetch('/api/printers').then(x=>x.json());"
"document.getElementById('plist').innerHTML=PL.map(p=>'<div class=card>'+(p.active?'\\u2605 ':'')+'<b>'+p.name+'</b> <span class=muted>'+p.host+(p.haskey?'':' (no key)')+'</span> '"
"+'<button onclick=usep('+p.i+')>Use</button> <button onclick=editp('+p.i+')>Edit</button> <button onclick=delp('+p.i+')>Remove</button></div>').join('')}"
"function editp(i){let p=PL.find(x=>x.i==i);if(!p)return;EI=i;pn.value=p.name;ph.value=p.host;pk.value='';pftitle.textContent='Edit '+p.name+' (key blank = keep)'}"
"async function savp(){let m=EI<0?{name:pn.value,host:ph.value,key:pk.value}:{i:EI,name:pn.value,host:ph.value,key:pk.value};"
"let r=await fetch(EI<0?'/api/printers':'/api/printers/update',{method:'POST',body:JSON.stringify(m)});"
"if(r.status>=400)alert(await r.text());else{newp();lp()}}"
"async function delp(i){await fetch('/api/printers/remove',{method:'POST',body:JSON.stringify({i:i})});if(EI==i)newp();lp()}"
"async function usep(i){await fetch('/api/printers/active',{method:'POST',body:JSON.stringify({i:i})});lp()}"
"async function expc(){let r=await fetch('/api/config/export').then(x=>x.json());"
"let b=new Blob([JSON.stringify(r,null,2)],{type:'application/json'});"
"let a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='prusa-touch-config.json';a.click()}"
"async function impc(){let f=icf.files[0];if(!f)return;if(!confirm('Replace ALL printers with config from '+f.name+'?'))return;"
"let r=await fetch('/api/config/import',{method:'POST',body:f});"
"if(r.status>=400)alert(await r.text());else{lp();alert('Import success!')}}"
"async function savew(){await fetch('/api/wifi',{method:'POST',body:JSON.stringify({ssid:ws.value,pass:wp.value})});alert('Saved; connecting...')}"
"async function ota(){let f=document.getElementById('fw').files[0];if(!f)return;"
"document.getElementById('otalog').textContent='Uploading '+f.name+'...';"
"let r=await fetch('/update',{method:'POST',body:f});"
"document.getElementById('otalog').textContent=await r.text()}"
"let GU='';"
"async function chk(){document.getElementById('gh').textContent='Checking...';let n=0;"
"const poll=async()=>{let r=await fetch('/api/update/check').then(x=>x.json());"
"if(r.checking&&n++<6){setTimeout(poll,2000);return;}"
"document.getElementById('gh').textContent='Current '+r.current+' / latest '+(r.latest||'?')+(r.available?' \\u2014 update available!':' \\u2014 up to date');"
"GU=r.url;document.getElementById('ub').style.display=r.available?'inline-block':'none'};poll()}"
"async function applyu(){if(!GU)return;document.getElementById('gh').textContent='Updating; device will reboot...';"
"await fetch('/api/update/apply',{method:'POST',body:JSON.stringify({url:GU})})}"
"st();setInterval(st,3000);"
"</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    pp_status_t s;
    app_state_get(&s);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", s.printer_name);
    cJSON_AddBoolToObject(o, "online", s.online);
    cJSON_AddStringToObject(o, "state", s.state[0] ? s.state : "READY");
    cJSON_AddNumberToObject(o, "nozzle", (int)s.temp_nozzle);
    cJSON_AddNumberToObject(o, "tnozzle", (int)s.target_nozzle);
    cJSON_AddNumberToObject(o, "bed", (int)s.temp_bed);
    cJSON_AddNumberToObject(o, "tbed", (int)s.target_bed);
    cJSON_AddBoolToObject(o, "has_job", s.has_job);
    cJSON_AddNumberToObject(o, "progress", (int)(s.progress + 0.5f));
    cJSON_AddStringToObject(o, "job", s.job_name);
    char *js = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t printers_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    int active = printer_store_active();
    for (int i = 0; i < printer_store_count(); i++) {
        pp_printer_t p;
        if (!printer_store_get(i, &p)) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "i", i);
        cJSON_AddStringToObject(e, "name", p.name);
        cJSON_AddStringToObject(e, "host", p.host);   /* key intentionally omitted */
        cJSON_AddBoolToObject(e, "active", i == active);
        cJSON_AddBoolToObject(e, "haskey", p.api_key[0] != '\0');
        cJSON_AddItemToArray(arr, e);
    }
    char *js = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js);
    cJSON_Delete(arr);
    return ESP_OK;
}

/* Read the whole request body into a heap buffer (caller frees). */
static char *recv_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 4096) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[len] = '\0';
    return buf;
}

static esp_err_t printers_post(httpd_req_t *req)
{
    char *body = recv_body(req);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad/oversized body"); return ESP_FAIL; }
    {
        cJSON *j = cJSON_Parse(body);
        if (j) {
            pp_printer_t p = {0};
            const cJSON *n = cJSON_GetObjectItem(j, "name");
            const cJSON *h = cJSON_GetObjectItem(j, "host");
            const cJSON *k = cJSON_GetObjectItem(j, "key");
            if (cJSON_IsString(h)) strlcpy(p.host, h->valuestring, sizeof(p.host));
            strlcpy(p.name, cJSON_IsString(n) && n->valuestring[0] ? n->valuestring : p.host, sizeof(p.name));
            if (cJSON_IsString(k)) strlcpy(p.api_key, k->valuestring, sizeof(p.api_key));
            p.port = 80;
            if (p.host[0]) {
                if (printer_store_add(&p) < 0) {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Printer limit reached");
                    cJSON_Delete(j);
                    free(body);
                    return ESP_FAIL;
                }
                app_state_printers_changed();
            }
            cJSON_Delete(j);
        }
        free(body);
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* Edit an existing printer by index. An empty/omitted "key" keeps the stored key. */
static esp_err_t printers_update_post(httpd_req_t *req)
{
    char *body = recv_body(req);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body);
    if (j) {
        const cJSON *iv = cJSON_GetObjectItem(j, "i");
        int idx = cJSON_IsNumber(iv) ? (int)iv->valuedouble : -1;
        pp_printer_t p;
        if (idx >= 0 && printer_store_get(idx, &p)) {   /* start from existing (keeps key) */
            const cJSON *n = cJSON_GetObjectItem(j, "name");
            const cJSON *h = cJSON_GetObjectItem(j, "host");
            const cJSON *k = cJSON_GetObjectItem(j, "key");
            if (cJSON_IsString(n) && n->valuestring[0]) strlcpy(p.name, n->valuestring, sizeof(p.name));
            if (cJSON_IsString(h) && h->valuestring[0]) strlcpy(p.host, h->valuestring, sizeof(p.host));
            if (cJSON_IsString(k) && k->valuestring[0]) strlcpy(p.api_key, k->valuestring, sizeof(p.api_key));
            printer_store_update(idx, &p);
            app_state_printers_changed();
        }
        cJSON_Delete(j);
    }
    free(body);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t printers_remove_post(httpd_req_t *req)
{
    char *body = recv_body(req);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body);
    if (j) {
        const cJSON *iv = cJSON_GetObjectItem(j, "i");
        if (cJSON_IsNumber(iv)) { printer_store_remove((int)iv->valuedouble); app_state_printers_changed(); }
        cJSON_Delete(j);
    }
    free(body);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t printers_active_post(httpd_req_t *req)
{
    char *body = recv_body(req);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body);
    if (j) {
        const cJSON *iv = cJSON_GetObjectItem(j, "i");
        if (cJSON_IsNumber(iv)) printer_store_set_active((int)iv->valuedouble);
        cJSON_Delete(j);
    }
    free(body);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char *body = recv_body(req);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad/oversized body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body);
    if (j) {
        const cJSON *s = cJSON_GetObjectItem(j, "ssid");
        const cJSON *p = cJSON_GetObjectItem(j, "pass");
        if (cJSON_IsString(s) && s->valuestring[0]) {
            /* Funnel WiFi (re)connect through the net task — the single owner of
             * esp_wifi_set_config/connect — instead of calling it on this httpd task. */
            app_state_wifi_connect(s->valuestring, cJSON_IsString(p) ? p->valuestring : "");
        }
        cJSON_Delete(j);
    }
    free(body);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* Firmware OTA: body is the raw .bin; write to the inactive OTA slot, validate it
 * is genuinely a Prusa-Touch image, then set boot + reboot. Without secure boot or
 * rollback, accepting a valid-but-wrong image (e.g. stock BTT, wrong board) would
 * brick the device, so we check the embedded app identity before committing. */
static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || (size_t)req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad/oversized image");
        return ESP_FAIL;
    }
    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, req->content_len, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        return ESP_FAIL;
    }
    char buf[1024];
    size_t remaining = (size_t)req->content_len, total = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (r <= 0) { esp_ota_abort(ota); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed"); return ESP_FAIL; }
        if (esp_ota_write(ota, buf, r) != ESP_OK) { esp_ota_abort(ota); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed"); return ESP_FAIL; }
        remaining -= r; total += r;
    }
    if (esp_ota_end(ota) != ESP_OK) {   /* validates image structure/checksum */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid image");
        return ESP_FAIL;
    }
    /* Identity gate: confirm it's a Prusa-Touch app before making it bootable. */
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK ||
        strncmp(desc.project_name, "prusa-touch", sizeof(desc.project_name)) != 0) {
        ESP_LOGW(TAG, "OTA rejected: not a Prusa-Touch image (project_name=%.*s)",
                 (int)sizeof(desc.project_name), desc.project_name);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "rejected: not a Prusa-Touch firmware image");
        return ESP_FAIL;   /* previous slot stays bootable */
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA wrote %u bytes to %s; rebooting", (unsigned)total, part->label);
    httpd_resp_sendstr(req, "OK — flashed, rebooting into new firmware");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ---- GitHub auto-update. The GitHub query is blocking (~seconds over TLS), so it
 * runs on its own task and the handler returns a cached snapshot immediately. ---- */
static ota_check_t       s_upd;
static bool              s_upd_have;
static volatile bool     s_upd_busy;
static SemaphoreHandle_t s_upd_mtx;

static void upd_check_task(void *arg)
{
    ota_check_t tmp;
    bool ok = ota_update_check(&tmp);
    xSemaphoreTake(s_upd_mtx, portMAX_DELAY);
    if (ok) { s_upd = tmp; s_upd_have = true; }
    s_upd_busy = false;
    xSemaphoreGive(s_upd_mtx);
    vTaskDelete(NULL);
}

static esp_err_t update_check_get(httpd_req_t *req)
{
    ota_check_t snap; bool have, busy, spawn = false;
    xSemaphoreTake(s_upd_mtx, portMAX_DELAY);
    if (!s_upd_busy) { s_upd_busy = true; spawn = true; }   /* atomic test-and-set */
    busy = s_upd_busy;
    snap = s_upd; have = s_upd_have;
    xSemaphoreGive(s_upd_mtx);
    if (spawn) xTaskCreate(upd_check_task, "upd_chk", 8192, NULL, 4, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "checking", busy);
    cJSON_AddStringToObject(o, "current", have ? snap.current : PP_FW_VERSION);
    cJSON_AddStringToObject(o, "latest", have ? snap.latest : "");
    cJSON_AddBoolToObject(o, "available", have ? snap.available : false);
    cJSON_AddStringToObject(o, "url", have ? snap.url : "");
    char *js = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(o);
    return ESP_OK;
}

static void ota_apply_task(void *arg)
{
    char *url = arg;
    ota_update_apply(url);   /* reboots on success */
    free(url);
    vTaskDelete(NULL);
}

static esp_err_t update_apply_post(httpd_req_t *req)
{
    char *body = recv_body(req);
    char *url = NULL;
    if (body) {
        cJSON *j = cJSON_Parse(body);
        if (j) {
            const cJSON *u = cJSON_GetObjectItem(j, "url");
            if (cJSON_IsString(u) && u->valuestring[0]) url = strdup(u->valuestring);
            cJSON_Delete(j);
        }
        free(body);
    }
    if (url) {
        xTaskCreate(ota_apply_task, "ota", 8192, url, 5, NULL);
        httpd_resp_sendstr(req, "Updating — the device will reboot into the new firmware.");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no url");
    }
    return ESP_OK;
}

/* ---- device API ---- */
static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", "Prusa Connect Touch");
    cJSON_AddStringToObject(o, "fw", PP_FW_VERSION);
    cJSON_AddStringToObject(o, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(o, "model", "BTT K-Touch / Panda Touch (ESP32-S3)");
    cJSON_AddStringToObject(o, "screen", "800x480");
    cJSON_AddNumberToObject(o, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    char *js = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t fleet_get(httpd_req_t *req)
{
    pp_status_t *arr = heap_caps_malloc(PP_MAX_PRINTERS * sizeof(pp_status_t), MALLOC_CAP_SPIRAM);
    if (!arr) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");

    int n = 0;
    app_state_get_fleet(arr, PP_MAX_PRINTERS, &n);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", arr[i].printer_name);
        cJSON_AddBoolToObject(e, "online", arr[i].online);
        cJSON_AddStringToObject(e, "state", arr[i].state);
        cJSON_AddNumberToObject(e, "nozzle", (int)arr[i].temp_nozzle);
        cJSON_AddNumberToObject(e, "tnozzle", (int)arr[i].target_nozzle);
        cJSON_AddNumberToObject(e, "bed", (int)arr[i].temp_bed);
        cJSON_AddNumberToObject(e, "tbed", (int)arr[i].target_bed);
        cJSON_AddBoolToObject(e, "printing", arr[i].has_job);
        cJSON_AddNumberToObject(e, "progress", (int)(arr[i].progress + 0.5f));
        cJSON_AddStringToObject(e, "job", arr[i].job_name);
        cJSON_AddNumberToObject(e, "speed", arr[i].speed);
        cJSON_AddNumberToObject(e, "z", arr[i].axis_z);
        cJSON_AddItemToArray(a, e);
    }
    char *js = cJSON_PrintUnformatted(a);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(a);
    heap_caps_free(arr);
    return ESP_OK;
}

static esp_err_t config_export_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < printer_store_count(); i++) {
        pp_printer_t p;
        if (!printer_store_get(i, &p)) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", p.name);
        cJSON_AddStringToObject(e, "host", p.host);
        cJSON_AddNumberToObject(e, "port", p.port);
        cJSON_AddStringToObject(e, "key", p.api_key);
        cJSON_AddItemToArray(arr, e);
    }
    char *js = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t config_import_post(httpd_req_t *req)
{
    char *body = recv_body(req);
    if (!body) return ESP_FAIL;
    cJSON *j = cJSON_Parse(body);
    if (!cJSON_IsArray(j)) {
        if (j) cJSON_Delete(j);
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected JSON array");
    }
    printer_store_clear();
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, j) {
        pp_printer_t p = {0};
        const cJSON *n = cJSON_GetObjectItem(e, "name");
        const cJSON *h = cJSON_GetObjectItem(e, "host");
        const cJSON *po = cJSON_GetObjectItem(e, "port");
        const cJSON *k = cJSON_GetObjectItem(e, "key");
        if (cJSON_IsString(h)) strlcpy(p.host, h->valuestring, sizeof(p.host));
        strlcpy(p.name, cJSON_IsString(n) && n->valuestring[0] ? n->valuestring : p.host, sizeof(p.name));
        p.port = cJSON_IsNumber(po) ? (int)po->valuedouble : 80;
        if (cJSON_IsString(k)) strlcpy(p.api_key, k->valuestring, sizeof(p.api_key));
        if (p.host[0]) printer_store_add(&p);
    }
    app_state_printers_changed();
    cJSON_Delete(j);
    free(body);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* Live screen mirror: stream the panel framebuffer as a 24-bit BMP (what's
 * literally on the display right now). RGB565 -> BGR888, bottom-up. */
static esp_err_t screen_get(httpd_req_t *req)
{
    esp_lcd_panel_handle_t panel = pt_get_panel();
    void *fb = NULL;
    if (!panel || esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no framebuffer");
        return ESP_FAIL;
    }
    const int W = 800, H = 480;
    const uint32_t imgsize = (uint32_t)W * 3 * H;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t v;
    v = 54 + imgsize;            memcpy(&hdr[2],  &v, 4);   /* file size      */
    v = 54;                      memcpy(&hdr[10], &v, 4);   /* pixel offset   */
    v = 40;                      memcpy(&hdr[14], &v, 4);   /* DIB header size*/
    int32_t iw = W, ih = H;      memcpy(&hdr[18], &iw, 4); memcpy(&hdr[22], &ih, 4);
    uint16_t planes = 1, bpp = 24; memcpy(&hdr[26], &planes, 2); memcpy(&hdr[28], &bpp, 2);
    memcpy(&hdr[34], &imgsize, 4);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_send_chunk(req, (const char *)hdr, 54);

    uint8_t *row = malloc((size_t)W * 3);
    if (!row) { httpd_resp_send_chunk(req, NULL, 0); return ESP_FAIL; }
    const uint16_t *src = (const uint16_t *)fb;
    for (int y = H - 1; y >= 0; y--) {            /* BMP is bottom-up */
        const uint16_t *p = &src[(size_t)y * W];
        for (int x = 0; x < W; x++) {
            uint16_t c = p[x];                    /* RGB565 */
            row[x * 3 + 0] = (uint8_t)((c & 0x1F) << 3);          /* B */
            row[x * 3 + 1] = (uint8_t)(((c >> 5) & 0x3F) << 2);   /* G */
            row[x * 3 + 2] = (uint8_t)(((c >> 11) & 0x1F) << 3);  /* R */
        }
        httpd_resp_send_chunk(req, (const char *)row, (size_t)W * 3);
    }
    free(row);
    httpd_resp_send_chunk(req, NULL, 0);          /* end of stream */
    return ESP_OK;
}

/* ---- automation/testing nav API ---- */
static esp_err_t ui_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "screen", ui_current_screen());
    char *js = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js);
    cJSON_Delete(o);
    return ESP_OK;
}

/* GET /api/ui/nav?screen=<dash|status|control|files|printers|wifi|about>
 * Navigation is async (marshaled to the LVGL thread), so "current" in the reply
 * reflects the pre-nav screen — re-query /api/ui (or grab /api/screen.bmp) after. */
static esp_err_t ui_nav_get(httpd_req_t *req)
{
    char q[64] = {0}, screen[24] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "screen", screen, sizeof(screen));
    }
    if (screen[0]) ui_request_screen(screen);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", screen[0] != 0);
    cJSON_AddStringToObject(o, "requested", screen);
    cJSON_AddStringToObject(o, "current", ui_current_screen());
    char *js = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js);
    cJSON_Delete(o);
    return ESP_OK;
}

void web_start(void)
{
    s_upd_mtx = xSemaphoreCreateMutex();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 16384;   /* headroom for TLS handshakes in Connect handlers */
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    httpd_uri_t routes[] = {
        { .uri="/",             .method=HTTP_GET,  .handler=root_get },
        { .uri="/api/status",   .method=HTTP_GET,  .handler=status_get },
        { .uri="/api/printers", .method=HTTP_GET,  .handler=printers_get },
        { .uri="/api/printers", .method=HTTP_POST, .handler=printers_post },
        { .uri="/api/printers/update", .method=HTTP_POST, .handler=printers_update_post },
        { .uri="/api/printers/remove", .method=HTTP_POST, .handler=printers_remove_post },
        { .uri="/api/printers/active", .method=HTTP_POST, .handler=printers_active_post },
        { .uri="/api/config/export", .method=HTTP_GET,  .handler=config_export_get },
        { .uri="/api/config/import", .method=HTTP_POST, .handler=config_import_post },
        { .uri="/api/wifi",     .method=HTTP_POST, .handler=wifi_post },
        { .uri="/update",       .method=HTTP_POST, .handler=ota_post },
        { .uri="/api/update/check", .method=HTTP_GET,  .handler=update_check_get },
        { .uri="/api/update/apply", .method=HTTP_POST, .handler=update_apply_post },
        { .uri="/api/info",         .method=HTTP_GET,  .handler=info_get },
        { .uri="/api/fleet",        .method=HTTP_GET,  .handler=fleet_get },
        { .uri="/api/screen.bmp",   .method=HTTP_GET,  .handler=screen_get },
        { .uri="/api/ui",           .method=HTTP_GET,  .handler=ui_get },
        { .uri="/api/ui/nav",       .method=HTTP_GET,  .handler=ui_nav_get },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++) {
        httpd_register_uri_handler(srv, &routes[i]);
    }
    ESP_LOGI(TAG, "web interface started on :80");
}
