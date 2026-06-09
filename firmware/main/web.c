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
#include "ui.h"
#include "wc_test_jpg.h"   /* embedded camera frame for the /api/test/webcam decode self-test */
#include "prusa_connect.h"

static const char *TAG = "web";

/* ---- Prusa-themed single-page UI ---- */
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
"input{font-size:15px;padding:9px 10px;border-radius:6px;border:1px solid #4e4e4e;background:#2a2a2a;color:#f2f2f2;margin:4px 0;width:100%}"
/* Connect-style buttons: ghost by default (transparent + thin border), compact + inline.
   Orange solid is reserved for the primary action via class p. */
"button{font-size:14px;padding:7px 14px;border-radius:6px;border:1px solid #4e4e4e;background:transparent;color:#f2f2f2;font-weight:600;cursor:pointer;margin:3px 6px 3px 0;width:auto}"
"button:hover{background:#333;border-color:#6a6a6a}"
"button.p{background:var(--o);color:#fff;border-color:var(--o)}button.p:hover{background:#e84a00}"
".ctlp{border-top:1px solid #3a3a3a;margin-top:12px;padding-top:10px}.ctlp .lbl{font-size:11px;color:#a7a7a7;letter-spacing:.05em;margin:2px 0 4px}"
".bar{height:10px;background:#4e4e4e;border-radius:5px;overflow:hidden;margin-top:8px}.bar>i{display:block;height:100%;background:var(--o)}"
".muted{color:#a7a7a7}</style></head><body>"
"<header>PRUSA CONNECT TOUCH</header>"
"<nav><a class=on onclick=\"t(0)\">Status</a><a onclick=\"t(1)\">Printers</a>"
"<a onclick=\"t(2)\">Wi-Fi</a><a onclick=\"t(5)\">Account</a><a onclick=\"t(6)\">Farm</a><a onclick=\"t(3)\">Firmware</a><a onclick=\"t(4)\">Screen</a></nav>"
"<div class=\"tab on\" id=t0><div id=stlist></div><div class=card id=dev></div></div>"
"<div class=tab id=t1><div class=card><b id=pftitle>Add printer</b>"
"<input id=pn placeholder=Name><input id=ph placeholder='IP / host'>"
"<input id=pk placeholder='API key (blank = keep when editing)'>"
"<button class=p onclick=savp()>Save</button> <button onclick=newp()>New</button></div>"
"<div id=plist></div>"
"<div class=card><b>Backup & Restore</b>"
"<p class=muted>Export your fleet config to a file, or import a saved config (replaces current fleet).</p>"
"<button onclick=expc()>Export Config</button>"
"<input type=file id=icf accept=.json style=margin-top:12px><button onclick=impc()>Import Config</button></div></div>"
"<div class=tab id=t2><div class=card><b>Wi-Fi</b>"
"<input id=ws placeholder=SSID><input id=wp type=password placeholder=Password>"
"<button class=p onclick=savew()>Save &amp; connect</button></div></div>"
"<div class=tab id=t5><div class=card><b>Prusa Connect</b>"
"<div id=acstat class=muted>Not linked.</div>"
"<button id=lob style='display:none;background:#5a2d2d;width:auto;padding:6px 14px' onclick=logout()>Log out</button>"
"<div id=loginform><input id=ae placeholder=Email><input id=ap type=password placeholder=Password>"
"<label style='display:block;margin:6px 0;font-size:13px'><input id=arem type=checkbox checked> Stay signed in (store password on device for automatic re-login)</label>"
"<button class=p onclick=connl()>Link Account</button></div>"
"<div id=totpform style=display:none><p class=muted>2FA required. Enter your TOTP code:</p>"
"<input id=tc placeholder=123456><button class=p onclick=connt()>Verify</button></div>"
"<div id=acclist style=margin-top:16px></div></div></div>"
"<div class=tab id=t6><div class=card><b>Prusa Farm</b>"
"<div class=muted>Org-wide printer + order status. Find your Organization ID at connect-farm.prusa3d.com.</div>"
"<input id=forg placeholder='Organization ID'><button class=p onclick=lf()>Load Farm</button></div>"
"<div id=fstat></div><div id=forders></div></div>"
"<div class=tab id=t3>"
"<div class=card><b>Auto-update from GitHub</b>"
"<div id=gh class=muted>Tap Check.</div>"
"<button onclick=chk()>Check for updates</button>"
"<button class=p id=ub style=display:none onclick=applyu()>Update now</button></div>"
"<div class=card><b>Manual firmware upload</b>"
"<p class=muted>Upload <b>prusa-touch-app.bin</b> (the ~1.7 MB update image) from the GitHub release "
"&mdash; not the ~16 MB full/USB image. The device reboots after updating.</p>"
"<input type=file id=fw accept=.bin><button class=p onclick=ota()>Flash</button>"
"<div id=otalog class=muted></div></div></div>"
"<div class=tab id=t4><div class=card><b>Live screen</b> "
"<button onclick=shot()>Refresh</button>"
"<div class=muted>What the touchscreen is showing right now.</div>"
"<img id=shot style='max-width:100%;border:1px solid #4e4e4e;margin-top:8px'></div></div>"
"<div id=snapm style='display:none;position:fixed;inset:0;background:rgba(0,0,0,.82);z-index:99;align-items:center;justify-content:center' onclick=\"if(event.target==this)snapx()\">"
"<div style='background:#1c1e21;padding:14px;border-radius:10px;max-width:92%'>"
"<div style='display:flex;justify-content:space-between;align-items:center;gap:16px;margin-bottom:10px'><b id=snapt>Snapshot</b><span><button onclick=snapr()>Refresh</button><button onclick=snapx()>Close</button></span></div>"
"<img id=snapi style='max-width:84vw;max-height:70vh;border-radius:6px;background:#000;min-width:200px;min-height:120px' onerror=\"this.alt='No snapshot available for this printer'\"></div></div>"
"<script>"
"var FL=[];var SNAPU='';"
"function t(i){for(let n=0;n<7;n++){let el=document.getElementById('t'+n);if(el)el.className='tab'+(n==i?' on':'')}"
"document.querySelectorAll('nav a').forEach(function(a){a.className=a.getAttribute('onclick')==('t('+i+')')?'on':''});if(i==1)lp();if(i==4)shot();if(i==5)la();if(i==6)lf_init()}"
"function shot(){document.getElementById('shot').src='/api/screen.bmp?t='+Date.now()}"
"async function st(){let L=await fetch('/api/fleet').then(x=>x.json());FL=L;"
"const sc=s=>{s=(s||'').toUpperCase();if(s=='PRINTING'||s=='ATTENTION')return'orange';if(s=='PAUSED')return'yellow';if(s=='FINISHED')return'green';if(s=='READY')return'olive';if(s=='ERROR'||s=='STOPPED')return'red';if(s=='BUSY'||s=='PREPARING')return'blue';return'gray'};"
"document.getElementById('stlist').innerHTML=L.map((r,i)=>{const c=r.online?sc(r.state):'gray';return '<div class=\"card '+c+'\">'+"
"'<div class=c-head>'+r.name+'<div class=c-badge>'+(r.online?r.state:'OFFLINE')+'</div></div>'+"
"'<div class=c-body>'+"
"'<div class=c-grid>'+"
"'<div class=c-cell><b>NOZZLE</b><div>'+(r.online?r.nozzle+(r.tnozzle>0?'/'+r.tnozzle:''):'--')+'&deg;C</div></div>'+"
"'<div class=c-cell><b>HEATBED</b><div>'+(r.online?r.bed+(r.tbed>0?'/'+r.tbed:''):'--')+'&deg;C</div></div>'+"
"'<div class=c-cell><b>SPEED</b><div>'+(r.online?r.speed+'%':'--')+'</div></div>'+"
"'<div class=c-cell><b>Z AXIS</b><div>'+(r.online?r.z.toFixed(2)+'mm':'--')+'</div></div>'+"
"'<div class=c-cell style=grid-column:span 2><b>PROGRESS</b><div>'+(r.printing?r.progress+'%':'--')+'</div></div>'+"
"'</div>'+"
"(r.printing?('<p class=muted style=margin:12px 0 4px 0>'+r.job+'</p><div class=bar><i style=width:'+r.progress+'%></i></div>'):'')+"
"(r.ctl?cp(r,i):'')+"
"'</div></div>'}).join('')||'<div class=card style=padding:18px>No printers yet.</div>';"
"try{let d=await fetch('/api/info').then(x=>x.json());document.getElementById('dev').innerHTML="
"'<span class=muted>'+d.name+' '+d.fw+' &middot; heap '+Math.round(d.heap_free/1024)+'KB &middot; up '+d.uptime_s+'s</span>'}catch(e){}}"
/* Per-printer control panel on each fleet card — mirrors the touchscreen Control screen.
   All onclick args are numeric (printer index, axis 0/1/2, signed distance, feedrate) so
   nothing needs quote-escaping inside these C string literals. */
"function csnap(i){var p=FL[i];if(!p||!p.uuid)return;SNAPU=p.uuid;document.getElementById('snapt').textContent=p.name+' \\u2014 webcam';snapr();document.getElementById('snapm').style.display='flex';}"
"function snapr(){document.getElementById('snapi').src='/api/connect/snapshot?uuid='+SNAPU+'&t='+Date.now();}"
"function snapx(){document.getElementById('snapm').style.display='none';document.getElementById('snapi').src='';}"
"function cp(r,i){return '<div class=ctlp>'+'<button onclick=csnap('+i+')>&#128247; Webcam</button>'+(r.printing?"
"'<div class=lbl>JOB</div><button onclick=cpause('+i+')>Pause</button><button onclick=cstop('+i+')>Stop</button>':"
"'<div class=lbl>PREHEAT</div><div><button onclick=cpre('+i+',215,60)>PLA</button><button onclick=cpre('+i+',230,85)>PETG</button><button onclick=cpre('+i+',260,100)>ASA</button><button onclick=cpre('+i+',0,0)>Cooldown</button></div>'+"
"'<div class=lbl style=margin-top:8px>MOVE</div><div><button onclick=chome('+i+')>&#8962; Home</button><button onclick=cjog('+i+',0,-10,3000)>X-</button><button onclick=cjog('+i+',0,10,3000)>X+</button><button onclick=cjog('+i+',1,-10,3000)>Y-</button><button onclick=cjog('+i+',1,10,3000)>Y+</button><button onclick=cjog('+i+',2,-10,600)>Z-</button><button onclick=cjog('+i+',2,10,600)>Z+</button></div>')+'</div>';}"
"function cgo(i,op,qs){let u=FL[i]&&FL[i].uuid;if(!u)return;return fetch('/api/connect/control?uuid='+u+'&op='+op+(qs||''),{method:'POST'}).then(()=>setTimeout(st,1200)).catch(()=>{});}"
"function cpause(i){cgo(i,'pause')}"
"function cstop(i){if(confirm('Stop the print on '+(FL[i]&&FL[i].name)+'?'))cgo(i,'stop')}"
"function cpre(i,n,b){cgo(i,'preheat','&n='+n+'&b='+b)}"
"function chome(i){cgo(i,'home')}"
"function cjog(i,a,d,f){if(a==2)cgo(i,'movez','&z='+d+'&f='+f);else cgo(i,'move','&'+(a==0?'x':'y')+'='+d+'&f='+f)}"
"async function logout(){if(!confirm('Log out of Prusa Connect? This clears the saved account.'))return;await fetch('/api/connect/logout',{method:'POST'});acclist.innerHTML='';la()}"
"async function la(){let r=await fetch('/api/connect/info').then(x=>x.json());"
"acstat.textContent=r.auth?'Linked':'Not linked.';"
"lob.style.display=r.auth?'inline-block':'none';"
"loginform.style.display=r.auth?'none':'block';totpform.style.display='none';"
"if(r.auth){"
"let teams=await fetch('/api/connect/teams').then(x=>x.json());"
"let def=await fetch('/api/connect/default_team').then(x=>x.text());"
"acclist.innerHTML='<p class=muted>Your Teams / Farm Mode:</p>'+teams.map(t=>'<div class=card><b>'+t.name+'</b> ('+t.role+')'+"
"(t.id==def?' <span style=\"background:var(--o);color:#fff;padding:2px 6px;border-radius:4px;font-size:10px;margin-left:8px\">DEFAULT</span>':'')+"
"'<div id=tlist_'+t.id+' style=margin-top:8px><button onclick=\"lt(\\''+t.id+'\\')\">Load Printers</button> '+"
"'<button class=p onclick=\"sd(\\''+t.id+'\\')\">Set Default</button></div></div>').join('')||'<div class=card style=padding:12px>No teams found.</div>'}"
"}"
"async function sd(id){await fetch('/api/connect/default_team',{method:'POST',body:id});la()}"
"async function lt(tid){let L=await fetch('/api/connect/team_printers?id='+tid).then(x=>x.json());"
"document.getElementById('tlist_'+tid).innerHTML='<button class=p style=margin-bottom:8px onclick=\"addAll(\\''+tid+'\\')\">Add All from Team</button>'+"
"L.map(p=>'<div class=card style=\"margin:4px 0;padding:8px 12px\"><b>'+p.name+'</b> <span class=muted>'+p.model+'</span> '+"
"'<button onclick=\"addc(\\''+p.uuid+'\\',\\''+p.name+'\\')\">Add</button></div>').join('')}"
"async function addAll(tid){if(!confirm('Add all printers from this team?'))return;"
"let L=await fetch('/api/connect/team_printers?id='+tid).then(x=>x.json());"
"for(let p of L)await addc(p.uuid,p.name);lp()}"
"async function connl(){acstat.textContent='Logging in...';"
"let r=await fetch('/api/connect/login',{method:'POST',body:JSON.stringify({e:ae.value,p:ap.value,rem:arem.checked})});"
"let j=await r.json();if(j.res=='totp'){loginform.style.display='none';totpform.style.display='block';acstat.textContent='2FA Required'}else if(j.res=='ok'){la()}else alert('Login failed')}"
"async function connt(){let r=await fetch('/api/connect/totp',{method:'POST',body:JSON.stringify({c:tc.value})});"
"if((await r.json()).res=='ok')la();else alert('Verification failed')}"
"async function addc(id,name){await fetch('/api/printers',{method:'POST',body:JSON.stringify({name:name,host:'cloud:'+id,key:'connect'})});lp();alert('Added!')}"
"let PL=[],EI=-1;"
"function newp(){EI=-1;pn.value=ph.value=pk.value='';pftitle.textContent='Add printer'}"
"async function lp(){PL=await fetch('/api/printers').then(x=>x.json());"
"document.getElementById('plist').innerHTML=PL.map(p=>'<div class=card style=\"padding:10px 12px\">'+(p.active?'\\u2605 ':'')+'<b>'+p.name+'</b> <span class=muted>'+(p.host.indexOf('cloud:')==0?'\\u2601 Prusa Connect':p.host+(p.haskey?'':' (no key)'))+'</span> '"
"+'<button class=p onclick=usep('+p.i+')>Use</button> <button onclick=editp('+p.i+')>Edit</button> <button onclick=delp('+p.i+')>Remove</button></div>').join('')}"
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
/* The full/USB image is 16 MB and can never fit the 4.5 MB OTA slot - catch it client-side. */
"if(f.size>5*1024*1024){document.getElementById('otalog').textContent=f.name+' is '+Math.round(f.size/1048576)+' MB - that is the full/USB image. Use prusa-touch-app.bin (~1.7 MB).';return}"
"document.getElementById('otalog').textContent='Uploading '+f.name+'...';"
"try{let r=await fetch('/update',{method:'POST',body:f});"
"document.getElementById('otalog').textContent=await r.text()}"
"catch(e){document.getElementById('otalog').textContent='Upload failed - connection lost. Verify the file is prusa-touch-app.bin and retry.'}}"
"let GU='';"
"async function chk(){document.getElementById('gh').textContent='Checking...';let n=0;"
"const poll=async()=>{let r=await fetch('/api/update/check').then(x=>x.json());"
"if(r.checking&&n++<6){setTimeout(poll,2000);return;}"
"document.getElementById('gh').textContent='Current '+r.current+' / latest '+(r.latest||'?')+(r.available?' \\u2014 update available!':' \\u2014 up to date');"
"GU=r.url;document.getElementById('ub').style.display=r.available?'inline-block':'none'};poll()}"
"async function applyu(){if(!GU)return;document.getElementById('gh').innerHTML='Updating... <div class=bar id=upb><i style=width:0%></i></div>';"
"await fetch('/api/update/apply',{method:'POST',body:JSON.stringify({url:GU})});"
/* Poll until done or error. idle (-1) means the update task hasn't started yet, so keep
   waiting (bounded) instead of freezing the bar; fetch errors during reboot are expected. */
"let idle=0;const p=async()=>{let r;try{r=await fetch('/api/update/progress').then(x=>x.json())}catch(e){setTimeout(p,1000);return}"
"if(r.state=='error'){document.getElementById('gh').textContent='Update failed: '+(r.msg||'unknown error');return}"
"if(r.state=='done'){document.getElementById('upb').firstChild.style.width='100%';document.getElementById('gh').textContent='Update complete - rebooting...';return}"
"if(r.progress>=0){idle=0;document.getElementById('upb').firstChild.style.width=r.progress+'%'}"
"else if(++idle>10){document.getElementById('gh').textContent='Update did not start - check the device log.';return}"
"setTimeout(p,1000)};setTimeout(p,800)}"
"function lf_init(){let o=localStorage.getItem('farmorg');if(o){forg.value=o;lf()}}"
"async function lf(){let o=forg.value.trim();if(!o){alert('Enter your Organization ID');return}localStorage.setItem('farmorg',o);fetch('/api/connect/setorg',{method:'POST',body:o});"
"fstat.innerHTML='<div class=card style=padding:12px>Loading...</div>';forders.innerHTML='';"
"try{let s=await fetch('/api/connect/farm?org='+o).then(x=>x.json());let p=s.data.stats.printers;"
"fstat.innerHTML='<div class=card><div class=c-head>Printers</div><div class=c-body><div class=c-grid>'+"
"'<div class=c-cell><b>ACTIVE</b><div>'+p.active+'</div></div>'+"
"'<div class=c-cell><b>ONLINE</b><div>'+p.online+'</div></div>'+"
"'<div class=c-cell><b>ERROR</b><div>'+p.error+'</div></div>'+"
"'<div class=c-cell><b>TOTAL</b><div>'+p.total+'</div></div></div></div></div>'}"
"catch(e){fstat.innerHTML='<div class=card style=padding:12px>Farm stats unavailable</div>'}"
"try{let d=await fetch('/api/connect/orders?org='+o).then(x=>x.json());let ns=d.data.order.orders.edges.map(e=>e.node);"
"let act=ns.filter(n=>n.state==='PROCESSING');"   /* only Processing orders (not Finished/Cancelled/Draft) */
"let rows=act.map(n=>{let j=n.jobCounts;let tot=j.created+j.printing+j.done+j.cancelled+j.needAttention;let dn=n.jobsCountCompleted||0;let pct=tot?Math.round(dn/tot*100):0;"
"return '<div class=card style=padding:10px><b>#'+n.number+' '+n.name+'</b><br><span class=muted>'+(n.completionDate||'')+' - done '+dn+'/'+tot+(j.printing?', '+j.printing+' printing':'')+(j.needAttention?', '+j.needAttention+' attn':'')+'</span><div class=bar><i style=width:'+pct+'%></i></div></div>'}).join('')||'<div class=card style=padding:12px class=muted>No active orders</div>';"
"forders.innerHTML='<div class=card style=padding:10px><b>Active Orders</b> <span class=muted>'+act.length+'</span></div>'+rows}"
"catch(e){forders.innerHTML='<div class=card style=padding:12px>Orders unavailable</div>'}}"
"st();la();setInterval(st,3000);"
"</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* The page ships inside the firmware image; a cached copy survives firmware
     * updates and keeps running stale JS against new endpoints. Never cache. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
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
        if (p.local_host[0]) cJSON_AddStringToObject(e, "local", p.local_host);  /* LAN fallback learned from Connect */
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
            app_state_wifi_connect(s->valuestring, cJSON_IsString(p) ? p->valuestring : "");
        }
        cJSON_Delete(j);
    }
    free(body);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no partition"); return ESP_FAIL; }
    if (req->content_len <= 0 || (size_t)req->content_len > part->size) {
        /* Almost certainly the 16 MB full/USB image. Refuse before reading the
         * body and close the socket so the browser stops streaming instead of
         * pushing the rest at a dead endpoint (the old apparent "hang"). */
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "File too large for OTA - upload prusa-touch-app.bin (~1.7 MB), not the full/USB image");
        return ESP_FAIL;
    }
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(part, req->content_len, &ota);
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err)); return ESP_FAIL; }
    char buf[1024];
    size_t remaining = (size_t)req->content_len;
    int timeouts = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 5) continue;   /* transient stall, retry bounded */
        if (r <= 0) {
            esp_ota_abort(ota);
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrupted");
            return ESP_FAIL;
        }
        timeouts = 0;
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_FAIL;
        }
        remaining -= r;
    }
    err = esp_ota_end(ota);   /* validates the received image */
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            err == ESP_ERR_OTA_VALIDATE_FAILED
                ? "Invalid image - upload prusa-touch-app.bin from the release, not full.bin"
                : esp_err_to_name(err));
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err)); return ESP_FAIL; }
    httpd_resp_sendstr(req, "OK - rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static ota_check_t s_upd;
static bool s_upd_have;
static volatile bool s_upd_busy;
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
    if (!s_upd_busy) { s_upd_busy = true; spawn = true; }
    busy = s_upd_busy; snap = s_upd; have = s_upd_have;
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

static esp_err_t update_progress_get(httpd_req_t *req)
{
    int p = ota_update_get_progress();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "progress", p);
    cJSON_AddStringToObject(o, "state", p == -2 ? "error" : p == -1 ? "idle"
                                      : p >= 100 ? "done" : "running");
    cJSON_AddStringToObject(o, "msg", ota_update_get_error());
    char *js = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(o);
    return ESP_OK;
}

static void ota_apply_task(void *arg) { ota_update_apply(arg); free(arg); vTaskDelete(NULL); }

static esp_err_t update_apply_post(httpd_req_t *req)
{
    char *body = recv_body(req);
    if (body) {
        cJSON *j = cJSON_Parse(body);
        if (j) {
            const cJSON *u = cJSON_GetObjectItem(j, "url");
            if (cJSON_IsString(u)) xTaskCreate(ota_apply_task, "ota", 8192, strdup(u->valuestring), 5, NULL);
            cJSON_Delete(j);
        }
        free(body);
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", "Prusa Connect Touch");
    cJSON_AddStringToObject(o, "fw", PP_FW_VERSION);
    cJSON_AddNumberToObject(o, "heap_free", (double)esp_get_free_heap_size());
    /* Internal RAM is the scarce resource (TLS/mbedTLS allocates here, not PSRAM). Surface
     * it so OOM regressions are visible — total free heap is mostly PSRAM and hides this. */
    cJSON_AddNumberToObject(o, "heap_internal", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "heap_internal_min", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
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
    int n = 0; app_state_get_fleet(arr, PP_MAX_PRINTERS, &n);
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
        cJSON_AddStringToObject(e, "uuid", arr[i].uuid);      /* control target (cloud) */
        cJSON_AddBoolToObject(e, "cloud", arr[i].is_cloud);
        cJSON_AddBoolToObject(e, "ctl", arr[i].has_control);  /* show control panel */
        cJSON_AddItemToArray(a, e);
    }
    char *js = cJSON_PrintUnformatted(a);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(a); heap_caps_free(arr);
    return ESP_OK;
}

static esp_err_t config_export_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < printer_store_count(); i++) {
        pp_printer_t p; if (!printer_store_get(i, &p)) continue;
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
    if (!cJSON_IsArray(j)) { if (j) cJSON_Delete(j); free(body); return ESP_FAIL; }
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
    cJSON_Delete(j); free(body);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t connect_info_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "auth", prusa_connect_is_authenticated());
    char *js = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t connect_login_post(httpd_req_t *req)
{
    char *body = recv_body(req); if (!body) return ESP_FAIL;
    cJSON *j = cJSON_Parse(body); pp_connect_status_t res = PP_CONNECT_ERROR;
    if (j) {
        const cJSON *e = cJSON_GetObjectItem(j, "e"), *p = cJSON_GetObjectItem(j, "p");
        const cJSON *rem = cJSON_GetObjectItem(j, "rem");
        /* Set the remember flag BEFORE login so its success path persists creds (or not). */
        prusa_connect_set_remember(cJSON_IsBool(rem) ? cJSON_IsTrue(rem) : true);
        if (cJSON_IsString(e) && cJSON_IsString(p)) res = prusa_connect_login(e->valuestring, p->valuestring);
        cJSON_Delete(j);
    }
    free(body);
    cJSON *r = cJSON_CreateObject();
    if (res == PP_CONNECT_AUTH_OK) cJSON_AddStringToObject(r, "res", "ok");
    else if (res == PP_CONNECT_NEED_TOTP) cJSON_AddStringToObject(r, "res", "totp");
    else cJSON_AddStringToObject(r, "res", "err");
    char *js = cJSON_PrintUnformatted(r); httpd_resp_sendstr(req, js); free(js); cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t connect_totp_post(httpd_req_t *req)
{
    char *body = recv_body(req); if (!body) return ESP_FAIL;
    cJSON *j = cJSON_Parse(body); pp_connect_status_t res = PP_CONNECT_ERROR;
    if (j) {
        const cJSON *c = cJSON_GetObjectItem(j, "c");
        if (cJSON_IsString(c)) res = prusa_connect_submit_totp(c->valuestring);
        cJSON_Delete(j);
    }
    free(body);
    cJSON *r = cJSON_CreateObject(); cJSON_AddStringToObject(r, "res", res == PP_CONNECT_AUTH_OK ? "ok" : "err");
    char *js = cJSON_PrintUnformatted(r); httpd_resp_sendstr(req, js); free(js); cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t connect_fleet_get(httpd_req_t *req)
{
    pp_status_t *arr = heap_caps_malloc(64 * sizeof(pp_status_t), MALLOC_CAP_SPIRAM);
    int n = 0;
    if (prusa_connect_get_fleet(arr, 64, &n) != ESP_OK) { if (arr) heap_caps_free(arr); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fail"); }
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", arr[i].printer_name);
        cJSON_AddStringToObject(e, "model", arr[i].model);
        cJSON_AddStringToObject(e, "uuid", arr[i].uuid);
        cJSON_AddStringToObject(e, "state", arr[i].state);
        cJSON_AddBoolToObject(e, "online", arr[i].online);
        cJSON_AddStringToObject(e, "team", arr[i].team);
        cJSON_AddStringToObject(e, "fw", arr[i].firmware);
        cJSON_AddNumberToObject(e, "nozzle", arr[i].temp_nozzle);
        cJSON_AddNumberToObject(e, "bed", arr[i].temp_bed);
        cJSON_AddItemToArray(a, e);
    }
    char *js = cJSON_PrintUnformatted(a);
    httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(a); heap_caps_free(arr);
    return ESP_OK;
}

/* Teams come per-printer in /app/printers (team_id/team_name), so derive the
 * distinct team list from the fleet — no separate (broken) mobile teams endpoint. */
static esp_err_t connect_teams_get(httpd_req_t *req)
{
    pp_status_t *arr = heap_caps_malloc(64 * sizeof(pp_status_t), MALLOC_CAP_SPIRAM);
    int n = 0;
    if (!arr || prusa_connect_get_fleet(arr, 64, &n) != ESP_OK) { if (arr) heap_caps_free(arr); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fail"); }
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        if (!arr[i].team[0]) continue;
        bool seen = false;
        cJSON *t = NULL;
        cJSON_ArrayForEach(t, a) {
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
            if (cJSON_IsString(nm) && strcmp(nm->valuestring, arr[i].team) == 0) { seen = true; break; }
        }
        if (seen) continue;
        cJSON *e = cJSON_CreateObject();
        char idbuf[16]; snprintf(idbuf, sizeof(idbuf), "%d", arr[i].team_id);
        cJSON_AddStringToObject(e, "id", idbuf);
        cJSON_AddStringToObject(e, "name", arr[i].team);
        cJSON_AddStringToObject(e, "role", "member");
        cJSON_AddItemToArray(a, e);
    }
    char *js = cJSON_PrintUnformatted(a);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(a); heap_caps_free(arr);
    return ESP_OK;
}

static esp_err_t connect_logout_post(httpd_req_t *req)
{
    prusa_connect_logout();
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t connect_farmprobe_get(httpd_req_t *req)
{
    char buf[300];
    prusa_connect_farm_probe(buf, sizeof(buf));
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t connect_setorg_post(httpd_req_t *req)
{
    char buf[64]; int r = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (r <= 0) return ESP_FAIL;
    buf[r] = '\0';
    prusa_connect_set_org(buf);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t connect_farm_get(httpd_req_t *req)
{
    char q[160], org[48] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "org", org, sizeof(org));
    char *j = prusa_connect_get_farm_stats(org);
    if (!j) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fail");
    httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, j); free(j);
    return ESP_OK;
}

static esp_err_t connect_orders_get(httpd_req_t *req)
{
    char q[160], org[48] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "org", org, sizeof(org));
    char *j = prusa_connect_get_orders(org);
    if (!j) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fail");
    httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, j); free(j);
    return ESP_OK;
}

/* DEBUG/TEST: set screen orientation (0=landscape, 1=flipped 180°) — drives the same
 * app_state path as the on-device dropdown, so it can be smoke-tested remotely. */
static esp_err_t test_orient_get(httpd_req_t *req)
{
    char q[64], v[8] = {0};
    int o = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "o", v, sizeof(v)) == ESP_OK)
        o = atoi(v);
    if (o < 0 || o > 3) o = 0;   /* 0=landscape 1=180 2=portrait 3=portrait-flipped */
    app_state_set_pref(PP_PREF_ORIENT, o);
    char body[20]; snprintf(body, sizeof(body), "{\"orient\":%d}", o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* DEBUG: dump the raw per-printer Connect JSON (for inspecting dialog_info / network_info). */
static esp_err_t connect_printer_raw_get(httpd_req_t *req)
{
    char q[160], uuid[48] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "uuid", uuid, sizeof(uuid)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing uuid");
    char *j = prusa_connect_get_printer_raw(uuid);
    if (!j) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fail");
    httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, j); free(j);
    return ESP_OK;
}

static esp_err_t connect_ctrlprobe_get(httpd_req_t *req)
{
    char q[200], uuid[48] = {0}, cmd[48] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "uuid", uuid, sizeof(uuid)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing uuid");
    httpd_query_key_value(q, "cmd", cmd, sizeof(cmd));   /* optional */
    char buf[300];
    prusa_connect_ctrl_probe(uuid, cmd, buf, sizeof(buf));
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* Printer control from the web UI (mirrors the touchscreen Control screen). uuid +
 * op are query params (no spaces); for op=gcode/jog the G-code line is the POST body.
 * jog wraps the move in G91/G90 (relative) and restores absolute, exactly like the
 * touch's on_jog_clicked, so the web and device drive the printer identically. */
static esp_err_t connect_control_post(httpd_req_t *req)
{
    char q[200], uuid[48] = {0}, op[16] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "uuid", uuid, sizeof(uuid)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing uuid");
    httpd_query_key_value(q, "op", op, sizeof(op));

    /* Numeric params (optional, per op). */
    char v[16];
    int n = (httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) ? atoi(v) : 0;   /* nozzle °C */
    int b = (httpd_query_key_value(q, "b", v, sizeof(v)) == ESP_OK) ? atoi(v) : 0;   /* bed °C    */
    int f = (httpd_query_key_value(q, "f", v, sizeof(v)) == ESP_OK) ? atoi(v) : 3000;/* feedrate  */
    float dx = (httpd_query_key_value(q, "x", v, sizeof(v)) == ESP_OK) ? atof(v) : 0;
    float dy = (httpd_query_key_value(q, "y", v, sizeof(v)) == ESP_OK) ? atof(v) : 0;
    float dz = (httpd_query_key_value(q, "z", v, sizeof(v)) == ESP_OK) ? atof(v) : 0;

    /* Modern Connect cloud printers use dedicated commands (the web fleet is all cloud).
     * Klipper/Moonraker control stays on the touchscreen's backend-aware gcode path. */
    esp_err_t rc = ESP_FAIL;
    if (!strcmp(op, "pause"))        rc = prusa_connect_pause(uuid);
    else if (!strcmp(op, "resume"))  rc = prusa_connect_resume(uuid);
    else if (!strcmp(op, "stop"))    rc = prusa_connect_stop(uuid);
    else if (!strcmp(op, "preheat")) { prusa_connect_set_nozzle_temp(uuid, n); rc = prusa_connect_set_bed_temp(uuid, b); }
    else if (!strcmp(op, "home"))    rc = prusa_connect_home(uuid, "XYZ");
    else if (!strcmp(op, "move"))    rc = prusa_connect_move(uuid, f, dx, dy);
    else if (!strcmp(op, "movez"))   rc = prusa_connect_move_z(uuid, f, dz);
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad op");

    char out[32]; snprintf(out, sizeof(out), "{\"ok\":%s}", rc == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

/* Proxy a cloud printer's webcam snapshot to the browser. The device fetches the JPEG
 * from Connect with its Bearer token (the browser can't — token lives on the device) and
 * streams the bytes back as image/jpeg. 404 if the printer has no camera / no frame, so
 * the <img onerror> can hide itself cleanly. */
static esp_err_t connect_snapshot_get(httpd_req_t *req)
{
    char q[200], uuid[48] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "uuid", uuid, sizeof(uuid)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing uuid");
    uint8_t *buf = NULL; int len = 0;
    if (prusa_connect_fetch_snapshot(uuid, &buf, &len) != ESP_OK || !buf || len <= 0) {
        if (buf) free(buf);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "", 0);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)buf, len);
    free(buf);
    return ESP_OK;
}

/* Self-test: decode + display an embedded real 250x140 camera frame on the Control screen.
 * Proves the on-device JPEG path (TJPGD + MEMFS + 1:1 render) works without a cloud token —
 * the live fetch is verified separately by the /api/connect/snapshot proxy. */
static esp_err_t test_webcam_get(httpd_req_t *req)
{
    pp_image_t *im = malloc(sizeof(*im));
    if (im) {
        im->len  = wc_test_jpg_len;
        im->data = malloc(wc_test_jpg_len);
        if (im->data) {
            memcpy(im->data, wc_test_jpg, wc_test_jpg_len);
            if (pt_display_schedule_ui(ui_apply_snapshot, im) != LV_RESULT_OK) { free(im->data); free(im); }
        } else { free(im); }
    }
    ui_request_screen("control");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* Self-test: render a representative active-orders Farm view on the touch Farm screen,
 * proving ui_apply_farm displays active orders without needing live GraphQL data (the
 * fetch/parse path is exercised separately by /api/connect/farm + /orders). Caller should
 * already be on the Farm screen; pushes the data only (no nav) so a real refresh can't race. */
/* Schema probe: POST a raw GraphQL body, get the connect-api response (device has no CORS).
 * Used to introspect the farm Order type (order state + piece-count field names). */
static esp_err_t test_gql_post(httpd_req_t *req)
{
    char *body = recv_body(req);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
    char *resp = prusa_connect_graphql_raw(body);
    free(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp ? resp : "{\"error\":\"null\"}");
    free(resp);
    return ESP_OK;
}

static esp_err_t test_farm_get(httpd_req_t *req)
{
    pp_farm_t *f = malloc(sizeof(*f));
    if (f) {
        memset(f, 0, sizeof(*f));
        f->valid = true;
        f->p_active = 1; f->p_online = 5; f->p_error = 0; f->p_total = 5;
        f->order_count = 2;
        strlcpy(f->orders[0].name, "Sample order #1042", sizeof(f->orders[0].name));
        f->orders[0].done = 2; f->orders[0].total = 8; f->orders[0].attn = 0;
        strlcpy(f->orders[1].name, "Sample batch", sizeof(f->orders[1].name));
        f->orders[1].done = 0; f->orders[1].total = 4; f->orders[1].attn = 1;
        if (pt_display_schedule_ui(ui_apply_farm, f) != LV_RESULT_OK) free(f);
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t connect_team_printers_get(httpd_req_t *req)
{
    char q[128], id[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "id", id, sizeof(id)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing id");
    
    int want = atoi(id);
    pp_status_t *arr = heap_caps_malloc(64 * sizeof(pp_status_t), MALLOC_CAP_SPIRAM);
    int n = 0;
    if (!arr || prusa_connect_get_fleet(arr, 64, &n) != ESP_OK) {
        if (arr) heap_caps_free(arr);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fail");
    }
    /* Filter the fleet to this team (team_id is per-printer in /app/printers). */
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        if (arr[i].team_id != want) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", arr[i].printer_name);
        cJSON_AddStringToObject(e, "model", arr[i].model);
        cJSON_AddStringToObject(e, "uuid", arr[i].uuid);
        cJSON_AddItemToArray(a, e);
    }
    char *js = cJSON_PrintUnformatted(a);
    httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(a); heap_caps_free(arr);
    return ESP_OK;
}

static esp_err_t connect_default_team_get(httpd_req_t *req)
{
    httpd_resp_sendstr(req, prusa_connect_get_default_team());
    return ESP_OK;
}

static esp_err_t connect_default_team_post(httpd_req_t *req)
{
    char buf[64];
    int r = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (r <= 0) return ESP_FAIL;
    buf[r] = '\0';
    prusa_connect_set_default_team(buf);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t screen_get(httpd_req_t *req)
{
    esp_lcd_panel_handle_t panel = pt_get_panel(); void *fb = NULL;
    if (!panel || esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no fb");
    const int W = 800, H = 480; uint8_t hdr[54] = { 'B','M', 0 };
    uint32_t imgsize = W*H*3, v = 54+imgsize; memcpy(&hdr[2],&v,4); hdr[10]=54; v=40; memcpy(&hdr[14],&v,4);
    int32_t iw=W, ih=H; memcpy(&hdr[18],&iw,4); memcpy(&hdr[22],&ih,4); hdr[26]=1; hdr[28]=24;
    httpd_resp_set_type(req, "image/bmp");
    if (httpd_resp_send_chunk(req, (const char*)hdr, 54) != ESP_OK) return ESP_FAIL;
    uint8_t *row = malloc(W*3); const uint16_t *src = (const uint16_t*)fb;
    esp_err_t e = ESP_OK;
    for (int y=H-1; y>=0 && e==ESP_OK; y--) {
        for (int x=0; x<W; x++) {
            uint16_t c = src[y*W+x];
            row[x*3+0]=(c&0x1F)<<3; row[x*3+1]=((c>>5)&0x3F)<<2; row[x*3+2]=((c>>11)&0x1F)<<3;
        }
        e = httpd_resp_send_chunk(req, (const char*)row, W*3);   /* abort the stream if the client hung up */
    }
    free(row);
    if (e == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);   /* terminate cleanly only on success */
    return e;
}

static esp_err_t ui_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "screen", ui_current_screen());
    char *js = cJSON_PrintUnformatted(o); httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, js);
    free(js); cJSON_Delete(o); return ESP_OK;
}

static esp_err_t ui_nav_get(httpd_req_t *req)
{
    char q[64]={0}, s[24]={0}; if (httpd_req_get_url_query_str(req, q, 64) == ESP_OK) httpd_query_key_value(q, "screen", s, 24);
    if (s[0]) ui_request_screen(s);
    httpd_resp_sendstr(req, "ok"); return ESP_OK;
}

void web_start(void)
{
    s_upd_mtx = xSemaphoreCreateMutex();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard; cfg.max_uri_handlers = 48; cfg.stack_size = 20480;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) return;
    httpd_uri_t rs[] = {
        { "/", HTTP_GET, root_get }, { "/api/status", HTTP_GET, status_get },
        { "/api/printers", HTTP_GET, printers_get }, { "/api/printers", HTTP_POST, printers_post },
        { "/api/printers/update", HTTP_POST, printers_update_post }, { "/api/printers/remove", HTTP_POST, printers_remove_post },
        { "/api/printers/active", HTTP_POST, printers_active_post },
        { "/api/config/export", HTTP_GET, config_export_get }, { "/api/config/import", HTTP_POST, config_import_post },
        { "/api/connect/info", HTTP_GET, connect_info_get }, { "/api/connect/login", HTTP_POST, connect_login_post },
        { "/api/connect/totp", HTTP_POST, connect_totp_post }, { "/api/connect/fleet", HTTP_GET, connect_fleet_get },
        { "/api/connect/teams", HTTP_GET, connect_teams_get }, { "/api/connect/team_printers", HTTP_GET, connect_team_printers_get },
        { "/api/connect/logout", HTTP_POST, connect_logout_post },
        { "/api/connect/farmprobe", HTTP_GET, connect_farmprobe_get },
        { "/api/connect/ctrlprobe", HTTP_GET, connect_ctrlprobe_get },
        { "/api/connect/control", HTTP_POST, connect_control_post },
        { "/api/connect/snapshot", HTTP_GET, connect_snapshot_get },
        { "/api/test/webcam", HTTP_GET, test_webcam_get },
        { "/api/test/farm", HTTP_GET, test_farm_get },
        { "/api/test/gql", HTTP_POST, test_gql_post },
        { "/api/test/printer", HTTP_GET, connect_printer_raw_get },
        { "/api/test/orient", HTTP_GET, test_orient_get },
        { "/api/connect/farm", HTTP_GET, connect_farm_get },
        { "/api/connect/setorg", HTTP_POST, connect_setorg_post },
        { "/api/connect/orders", HTTP_GET, connect_orders_get },
        { "/api/connect/default_team", HTTP_GET, connect_default_team_get }, { "/api/connect/default_team", HTTP_POST, connect_default_team_post },
        { "/api/wifi", HTTP_POST, wifi_post }, { "/update", HTTP_POST, ota_post },
        { "/api/update/check", HTTP_GET, update_check_get }, { "/api/update/apply", HTTP_POST, update_apply_post },
        { "/api/update/progress", HTTP_GET, update_progress_get }, { "/api/info", HTTP_GET, info_get },
        { "/api/fleet", HTTP_GET, fleet_get }, { "/api/screen.bmp", HTTP_GET, screen_get },
        { "/api/ui", HTTP_GET, ui_get }, { "/api/ui/nav", HTTP_GET, ui_nav_get }
    };
    for (size_t i=0; i<sizeof(rs)/sizeof(rs[0]); i++) httpd_register_uri_handler(srv, &rs[i]);
}
