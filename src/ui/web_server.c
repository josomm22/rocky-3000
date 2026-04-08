/*
 * web_server — persistent HTTP server (port 80).
 *
 * Serves grind history and OTA firmware upload.
 * Started once at boot; runs for the life of the application.
 */

#include "web_server.h"
#include "grind_history.h"
#include "grind_controller.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include "web_bundle.h"   /* auto-generated: WEB_BUNDLE_GZ, WEB_BUNDLE_GZ_LEN */
#include "version.h"
#include "ota_checker.h"
#include "esp_err.h"

static const char *TAG = "web_srv";

/* ── OTA progress (shared with screen_ota poll timer) ────────── */
volatile int  web_server_ota_pct   = 0;
volatile bool web_server_ota_done  = false;
volatile bool web_server_ota_error = false;

void web_server_reset_ota_state(void)
{
    web_server_ota_pct   = 0;
    web_server_ota_done  = false;
    web_server_ota_error = false;
}

/* ── OTA receive buffer ───────────────────────────────────────── */
#define OTA_BUF_SIZE 4096
static char s_ota_buf[OTA_BUF_SIZE];

/* ═══════════════════════════════════════════════════════════════
 * History page — static strings
 * ═══════════════════════════════════════════════════════════════ */

/* Everything up to and including the opening of the inline <script> data var */
static const char HIST_HEAD[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Grind History</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:#111118;color:#e8e8f0;font-family:system-ui,sans-serif;"
         "padding:20px 16px;min-height:100vh}"
    "h1{color:#4fc3f7;font-size:1.4rem;margin-bottom:4px}"
    ".sub{color:#666680;font-size:.85rem;margin-bottom:20px}"
    ".stats{display:flex;gap:12px;margin-bottom:20px;flex-wrap:wrap}"
    ".stat{background:#1e1e2e;border-radius:12px;padding:16px 20px;"
          "flex:1;min-width:120px;text-align:center}"
    ".stat-val{font-size:1.6rem;font-weight:700;color:#4fc3f7}"
    ".stat-lbl{font-size:.75rem;color:#666680;margin-top:4px}"
    ".chart-wrap{background:#1e1e2e;border-radius:12px;padding:16px;"
                "margin-bottom:20px}"
    ".chart-title{font-size:.8rem;color:#666680;margin-bottom:10px;"
                 "display:flex;align-items:center;gap:16px}"
    ".legend{display:flex;gap:16px}"
    ".dot{width:10px;height:10px;border-radius:2px;display:inline-block;"
         "margin-right:4px;vertical-align:middle}"
    "canvas{width:100%;height:180px;display:block}"
    "table{width:100%;border-collapse:collapse;background:#1e1e2e;"
          "border-radius:12px;overflow:hidden}"
    "thead th{padding:10px 14px;text-align:left;font-size:.75rem;"
             "color:#666680;font-weight:500;border-bottom:1px solid #25253a}"
    "tbody td{padding:10px 14px;font-size:.9rem;border-bottom:1px solid #1a1a2a}"
    "tbody tr:last-child td{border-bottom:none}"
    ".pos{color:#ef5350}.neg{color:#66bb6a}.neu{color:#4fc3f7}"
    ".btn{display:inline-block;margin-top:16px;padding:10px 22px;"
         "background:#25253a;color:#4fc3f7;border:1px solid #4fc3f7;"
         "border-radius:8px;cursor:pointer;font-size:.9rem;text-decoration:none}"
    ".btn:hover{background:#1a1a2e}"
    ".empty{text-align:center;padding:40px;color:#666680}"
    "</style></head><body>"
    "<h1>&#9749; Grind History</h1>"
    "<p class='sub'>Shot log — target vs dispensed weight</p>"
    /* Live scale card — populated by /api/sensor polling */
    "<div class='stats'>"
      "<div class='stat'><div class='stat-val' id='sv-live'>--</div>"
        "<div class='stat-lbl'>Live (calibrated g)</div></div>"
      "<div class='stat'><div class='stat-val' id='sv-raw'>--</div>"
        "<div class='stat-lbl'>Raw (cal=1)</div></div>"
      "<div class='stat'><div class='stat-val' id='sv-cal'>--</div>"
        "<div class='stat-lbl'>Cal factor</div></div>"
    "</div>"
    /* inline data injected by server here */
    "<script>var shots=";

/* Everything after the injected data array */
static const char HIST_TAIL[] =
    ";</script>"
    "<script>"
    /* Stats */
    "var n=shots.length;"
    "var sumR=0,sumD=0;"
    "shots.forEach(function(s){sumR+=s.r;sumD+=(s.r-s.t);});"
    "var avgR=n?sumR/n:0,avgD=n?sumD/n:0;"
    "function fmt1(v){return v.toFixed(1);}"
    "function fmtD(v){return (v>=0?'+':'')+v.toFixed(2);}"
    /* Inject stats */
    "document.write("
      "'<div class=\\'stats\\'>"
        "<div class=\\'stat\\'><div class=\\'stat-val\\'>'+(n)+'</div>"
          "<div class=\\'stat-lbl\\'>Total shots</div></div>"
        "<div class=\\'stat\\'><div class=\\'stat-val\\'>'+fmt1(avgR)+'g</div>"
          "<div class=\\'stat-lbl\\'>Avg dispensed</div></div>"
        "<div class=\\'stat\\'><div class=\\'stat-val stat-delta\\'>'+fmtD(avgD)+'g</div>"
          "<div class=\\'stat-lbl\\'>Avg delta</div></div>"
      "'</div>');"
    /* Color the delta */
    "setTimeout(function(){"
      "var el=document.querySelector('.stat-delta');"
      "if(el){el.style.color=avgD>0.3?'#ef5350':avgD<-0.3?'#ffa726':'#66bb6a';}"
    "},0);"
    /* Chart */
    "document.write('<div class=\\'chart-wrap\\'>"
      "<div class=\\'chart-title\\'>Last '+(Math.min(n,30))+' shots"
        "<span class=\\'legend\\'>"
          "<span><span class=\\'dot\\' style=\\'background:#25253a;border:1px solid #555\\'></span>Target</span>"
          "<span><span class=\\'dot\\' style=\\'background:#4fc3f7\\'></span>Result</span>"
        "</span></div>"
      "<canvas id=\\'c\\'></canvas></div>');"
    /* Draw chart after DOM is ready */
    "window.addEventListener('load',function(){"
      "var cv=document.getElementById('c');"
      "if(!cv||!n)return;"
      "cv.width=cv.offsetWidth||600;cv.height=180;"
      "var ctx=cv.getContext('2d');"
      "var pts=shots.slice(-30);"
      "var maxG=0;"
      "pts.forEach(function(s){if(s.t>maxG)maxG=s.t;if(s.r>maxG)maxG=s.r;});"
      "maxG=Math.ceil(maxG*1.15/5)*5;"
      "var W=cv.width,H=cv.height,np=pts.length;"
      "var pad=4,grpW=Math.floor((W-pad)/np);"
      "var barW=Math.max(2,Math.floor(grpW/2)-2);"
      /* grid lines */
      "ctx.strokeStyle='#25253a';ctx.lineWidth=1;"
      "for(var g=5;g<=maxG;g+=5){"
        "var gy=H-(g/maxG)*H;"
        "ctx.beginPath();ctx.moveTo(0,gy);ctx.lineTo(W,gy);ctx.stroke();"
        "ctx.fillStyle='#444466';ctx.font='10px system-ui';"
        "ctx.fillText(g+'g',2,gy-2);}"
      /* bars */
      "pts.forEach(function(s,i){"
        "var x=pad+i*grpW;"
        "var th=Math.round((s.t/maxG)*H);"
        "var rh=Math.round((s.r/maxG)*H);"
        "ctx.fillStyle='#2e2e48';"
        "ctx.fillRect(x,H-th,barW,th);"
        "var delta=s.r-s.t;"
        "ctx.fillStyle=delta>0.5?'#ef5350':delta<-0.5?'#ffa726':'#4fc3f7';"
        "ctx.fillRect(x+barW+2,H-rh,barW,rh);"
      "});"
    "});"
    /* Table (last 20, newest first) */
    "if(n>0){"
      "var rows=shots.slice(-20).reverse();"
      "var html='<table><thead><tr>"
        "<th>#</th><th>Target</th><th>Dispensed</th><th>Delta</th>"
        "<th>Duration</th><th>At Cutoff</th><th>Pre-pulse</th>"
        "<th>Flow</th><th>Offset</th><th>Pulses</th>"
      "</tr></thead><tbody>';"
      "rows.forEach(function(s,i){"
        "var d=s.r-s.t;"
        "var cls=d>0.3?'pos':d<-0.3?'neg':'neu';"
        "var pcls=s.p>0?'pos':'';"
        "var ts=s.ts>0?new Date(s.ts*1000).toLocaleString():'';"
        "html+='<tr>"
          "<td title=\\''+ts+'\\'>'+( n-i)+'</td>"
          "<td>'+fmt1(s.t)+'g</td>"
          "<td>'+fmt1(s.r)+'g</td>"
          "<td class=\\''+cls+'\\'>'+(d>=0?'+':'')+d.toFixed(2)+'g</td>"
          "<td>'+(s.ms?(s.ms/1000).toFixed(1)+'s':'-')+'</td>"
          "<td>'+(s.wc?s.wc.toFixed(2)+'g':'-')+'</td>"
          "<td>'+(s.wp?s.wp.toFixed(2)+'g':'-')+'</td>"
          "<td>'+(s.f?s.f.toFixed(1)+'g/s':'-')+'</td>"
          "<td>'+s.o.toFixed(2)+'g</td>"
          "<td class=\\''+pcls+'\\'>'+(s.p||0)+'</td>"
        "</tr>';"
      "});"
      "html+='</tbody></table>';"
      "document.write(html);"
    "}else{"
      "document.write('<div class=\\'empty\\'>No grind history yet.</div>');"
    "}"
    "document.write('<a class=\\'btn\\' href=\\'/history\\'>&#8635; Refresh</a>');"
    "</script>"
    /* Live sensor polling — updates the Scale card every 500 ms */
    "<script>"
    "(function poll(){"
      "fetch('/api/sensor').then(function(r){return r.json();}).then(function(d){"
        "document.getElementById('sv-live').textContent=d.live_g.toFixed(2)+'g';"
        "document.getElementById('sv-raw').textContent=d.raw_g.toFixed(2);"
        "document.getElementById('sv-cal').textContent=d.cal.toFixed(4);"
      "}).catch(function(){});"
      "setTimeout(poll,500);"
    "})();"
    "</script></body></html>";

/* ── History handlers ─────────────────────────────────────────── */

static esp_err_t handle_history(httpd_req_t *req)
{
    grind_record_t recs[HISTORY_MAX];
    int n = grind_history_get(recs, HISTORY_MAX);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, HIST_HEAD, HTTPD_RESP_USE_STRLEN);

    /* Inject shots array as JS literal */
    httpd_resp_send_chunk(req, "[", 1);
    char buf[128];
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf),
                 "%s{\"t\":%.1f,\"r\":%.1f,\"wc\":%.2f,\"wp\":%.2f"
                 ",\"f\":%.2f,\"o\":%.2f,\"ms\":%u,\"p\":%u,\"ts\":%lu}",
                 i ? "," : "",
                 recs[i].target_g, recs[i].result_g,
                 recs[i].weight_at_cutoff_g, recs[i].weight_before_pulses_g,
                 recs[i].flow_g_s, recs[i].offset_g,
                 (unsigned)recs[i].grind_ms, (unsigned)recs[i].pulse_count,
                 (unsigned long)recs[i].timestamp);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "]", 1);

    httpd_resp_send_chunk(req, HIST_TAIL, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_history_delete_selected(httpd_req_t *req)
{
    char body[256] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    int indices[HISTORY_MAX];
    int count = 0;
    char *p = body;
    while (*p && count < HISTORY_MAX) {
        char *end;
        long idx = strtol(p, &end, 10);
        if (end == p) break;
        if (idx >= 0) indices[count++] = (int)idx;
        p = end;
        while (*p == ',') p++;
    }

    grind_history_delete_indices(indices, count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_history_clear(httpd_req_t *req)
{
    grind_history_clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_history_json(httpd_req_t *req)
{
    grind_record_t recs[HISTORY_MAX];
    int n = grind_history_get(recs, HISTORY_MAX);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"shots\":[", HTTPD_RESP_USE_STRLEN);
    char buf[128];
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf),
                 "%s{\"t\":%.1f,\"r\":%.1f,\"wc\":%.2f,\"wp\":%.2f"
                 ",\"f\":%.2f,\"o\":%.2f,\"ms\":%u,\"p\":%u,\"ts\":%lu}",
                 i ? "," : "",
                 recs[i].target_g, recs[i].result_g,
                 recs[i].weight_at_cutoff_g, recs[i].weight_before_pulses_g,
                 recs[i].flow_g_s, recs[i].offset_g,
                 (unsigned)recs[i].grind_ms, (unsigned)recs[i].pulse_count,
                 (unsigned long)recs[i].timestamp);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── Sensor endpoint ──────────────────────────────────────────── */

static esp_err_t handle_sensor(httpd_req_t *req)
{
    float live = grind_ctrl_get_live_weight();
    float cal  = grind_ctrl_get_cal_factor();
    float raw  = (cal > 0.0001f) ? live / cal : 0.0f;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"live_g\":%.2f,\"raw_g\":%.2f,\"cal\":%.4f}",
             live, raw, cal);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── OTA status / trigger endpoints ──────────────────────────── */

static esp_err_t handle_ota_status(httpd_req_t *req)
{
    static const char *state_names[] = {
        "idle", "checking", "available", "no_update",
        "downloading", "done", "error"
    };
    ota_check_state_t state = ota_checker_get_state();
    const char *version     = ota_checker_get_version();
    int http_status         = ota_checker_get_http_status();
    int open_err            = ota_checker_get_open_err();
    int tls_err             = ota_checker_get_tls_err();
    const char *state_str   = (state < 7) ? state_names[state] : "unknown";
    const char *err_str     = open_err ? esp_err_to_name((esp_err_t)open_err) : "ESP_OK";

    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"version\":\"%s\","
        "\"http_status\":%d,\"open_err\":%d,\"open_err_str\":\"%s\","
        "\"tls_err\":%d,\"progress\":%d}",
        state_str, version ? version : "",
        http_status, open_err, err_str, tls_err,
        ota_checker_get_progress());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t handle_ota_check(httpd_req_t *req)
{
    ota_checker_force_check();
    return handle_ota_status(req);
}

static esp_err_t handle_ota_apply(httpd_req_t *req)
{
    ota_checker_apply();
    return handle_ota_status(req);
}

/* ── OTA page (same HTML as before, now served from here) ────── */

static const char s_ota_html[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Firmware Update</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:#111118;color:#e8e8f0;font-family:system-ui,sans-serif;"
         "display:flex;flex-direction:column;align-items:center;justify-content:center;"
         "min-height:100vh;padding:16px;gap:16px}"
    ".card{background:#1e1e2e;border-radius:16px;padding:32px 28px;"
           "width:100%;max-width:440px;text-align:center}"
    "h1{color:#4fc3f7;font-size:1.4rem;margin-bottom:8px}"
    ".sub{color:#9e9eb0;font-size:.9rem;margin-bottom:24px;line-height:1.5}"
    ".drop{border:2px dashed #333350;border-radius:12px;padding:32px 16px;"
          "cursor:pointer;transition:border-color .15s,background .15s;"
          "margin-bottom:20px;position:relative}"
    ".drop:hover,.drop.over{border-color:#4fc3f7;background:#1a1a2e}"
    ".drop.has-file{border-style:solid;border-color:#4fc3f7}"
    ".drop-icon{font-size:2rem;margin-bottom:8px}"
    ".drop-hint{color:#666680;font-size:.85rem}"
    ".drop input{position:absolute;inset:0;opacity:0;cursor:pointer;width:100%;height:100%}"
    "#fi{color:#e8e8f0;font-size:.95rem;font-weight:600;margin-bottom:2px;display:none}"
    "#fs{color:#666680;font-size:.8rem;display:none}"
    "button{background:#4fc3f7;color:#111118;border:none;border-radius:10px;"
           "padding:14px 0;font-size:1rem;font-weight:700;cursor:pointer;width:100%;"
           "transition:background .15s}"
    "button:hover{background:#81d4fa}"
    "button:disabled{background:#25253a;color:#555570;cursor:not-allowed}"
    "#pw{display:none;margin-top:20px}"
    "progress{width:100%;height:10px;border-radius:5px;overflow:hidden;appearance:none}"
    "progress::-webkit-progress-bar{background:#25253a;border-radius:5px}"
    "progress::-webkit-progress-value{background:#4fc3f7;border-radius:5px;transition:width .3s}"
    "#pct{color:#9e9eb0;font-size:.8rem;margin-top:6px}"
    "#st{margin-top:16px;font-size:.95rem;min-height:22px;font-weight:500}"
    ".ok{color:#66bb6a}.err{color:#ef5350}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>&#9881; Firmware Update</h1>"
    "<p class='sub'>Current: <strong>" APP_VERSION_DISPLAY "</strong><br>"
    "Drop a <strong>.bin</strong> file below or click to browse, then tap Upload.</p>"
    "<div class='drop' id='dz' onclick='document.getElementById(\"f\").click()'>"
      "<div class='drop-icon'>&#128190;</div>"
      "<div id='fi'></div>"
      "<div id='fs'></div>"
      "<div class='drop-hint' id='dh'>Drop .bin here or click to browse</div>"
      "<input type='file' id='f' accept='.bin' onclick='event.stopPropagation()'>"
    "</div>"
    "<button id='btn' onclick='go()'>Upload</button>"
    "<div id='pw'>"
      "<progress id='bar' value='0' max='100'></progress>"
      "<div id='pct'></div>"
    "</div>"
    "<div id='st'></div>"
    "</div>"
    "<script>"
    "var dz=document.getElementById('dz');"
    "var fi=document.getElementById('fi');"
    "var fs=document.getElementById('fs');"
    "var dh=document.getElementById('dh');"
    "function fmt(b){return b>1048576?(b/1048576).toFixed(1)+' MB':(b/1024).toFixed(0)+' KB';}"
    "function setFile(file){"
      "if(!file)return;"
      "fi.textContent=file.name;fi.style.display='block';"
      "fs.textContent=fmt(file.size);fs.style.display='block';"
      "dh.style.display='none';"
      "dz.classList.add('has-file');}"
    "document.getElementById('f').onchange=function(){setFile(this.files[0]);};"
    "dz.addEventListener('dragover',function(e){e.preventDefault();dz.classList.add('over');});"
    "dz.addEventListener('dragleave',function(){dz.classList.remove('over');});"
    "dz.addEventListener('drop',function(e){"
      "e.preventDefault();dz.classList.remove('over');"
      "var f=e.dataTransfer.files[0];"
      "if(f){document.getElementById('f')._file=f;setFile(f);}});"
    "function go(){"
      "var inp=document.getElementById('f');"
      "var file=inp._file||(inp.files&&inp.files[0]);"
      "if(!file){document.getElementById('st').innerHTML="
        "'<span class=\\'err\\'>Select a .bin file first.</span>';return;}"
      "document.getElementById('btn').disabled=true;"
      "document.getElementById('pw').style.display='block';"
      "var st=document.getElementById('st');"
      "st.innerHTML='';"
      "var x=new XMLHttpRequest();"
      "x.upload.onprogress=function(e){"
        "if(e.lengthComputable){"
          "var p=Math.round(e.loaded/e.total*100);"
          "document.getElementById('bar').value=p;"
          "document.getElementById('pct').textContent=p+'%';}};"
      "x.onload=function(){"
        "document.getElementById('bar').value=100;"
        "document.getElementById('pct').textContent='100%';"
        "if(x.status===200){"
          "st.innerHTML='<span class=\\'ok\\'>&#10003; Update complete \u2014 rebooting\u2026</span>';"
        "}else{"
          "st.innerHTML='<span class=\\'err\\'>\u26a0 '+x.responseText+'</span>';"
          "document.getElementById('btn').disabled=false;}};"
      "x.onerror=function(){"
        "st.innerHTML='<span class=\\'err\\'>\u26a0 Network error</span>';"
        "document.getElementById('btn').disabled=false;};"
      "x.open('POST','/update');"
      "x.send(file);}"
    "</script>"
    "<div class='card'>"
    "<button id='cbtn' onclick='chk()'"
      " style='background:#25253a;color:#4fc3f7;border:1px solid #333350;"
      "border-radius:10px;padding:12px 0;font-size:.95rem;font-weight:600;"
      "cursor:pointer;width:100%;transition:background .15s'>"
      "&#128279; Check GitHub for Updates</button>"
    "<div id='cst' style='margin-top:12px;font-size:.9rem;min-height:20px'></div>"
    "</div>"
    "<script>"
    "var cpoll=null;"
    "function chk(){"
      "document.getElementById('cbtn').disabled=true;"
      "document.getElementById('cst').innerHTML='<span style=\"color:#9e9eb0\">Checking\u2026</span>';"
      "fetch('/api/ota/check',{method:'POST'})"
        ".then(function(r){return r.json();})"
        ".then(renderOta)"
        ".catch(function(){"
          "document.getElementById('cst').innerHTML='<span class=\"err\">\u26a0 Network error</span>';"
          "document.getElementById('cbtn').disabled=false;});"
      "if(!cpoll){cpoll=setInterval(function(){"
        "fetch('/api/ota/status').then(function(r){return r.json();}).then(renderOta).catch(function(){});"
      "},1000);}}"
    "function apply(){"
      "document.getElementById('cst').innerHTML='<span style=\"color:#9e9eb0\">Starting download\u2026</span>';"
      "fetch('/api/ota/apply',{method:'POST'})"
        ".then(function(r){return r.json();})"
        ".then(renderOta)"
        ".catch(function(){"
          "document.getElementById('cst').innerHTML='<span class=\"err\">\u26a0 Network error</span>';});"
      "if(!cpoll){cpoll=setInterval(function(){"
        "fetch('/api/ota/status').then(function(r){return r.json();}).then(renderOta).catch(function(){});"
      "},500);}}"
    "function renderOta(d){"
      "var el=document.getElementById('cst');"
      "if(d.state==='checking'){"
        "el.innerHTML='<span style=\"color:#9e9eb0\">Checking\u2026</span>';return;}"
      "if(d.state==='downloading'){"
        "var p=d.progress||0;"
        "el.innerHTML='<progress value=\"'+p+'\" max=\"100\"></progress>"
          "<div style=\"color:#9e9eb0;font-size:.8rem;margin-top:6px\">Downloading\u2026 '+p+'%</div>';return;}"
      "clearInterval(cpoll);cpoll=null;"
      "document.getElementById('cbtn').disabled=false;"
      "if(d.state==='available'){"
        "el.innerHTML='<div style=\"margin-bottom:10px\"><span class=\"ok\">&#10003; Update available: <strong>'+d.version+'</strong></span></div>"
          "<button onclick=\"apply()\" style=\"background:#66bb6a;color:#111118;border:none;"
          "border-radius:10px;padding:10px 0;font-size:.9rem;font-weight:700;"
          "cursor:pointer;width:100%\">&#11015; Install on Device</button>';}"
      "else if(d.state==='no_update'){"
        "el.innerHTML='<span class=\"ok\">&#10003; Firmware is up to date</span>';}"
      "else if(d.state==='done'){"
        "el.innerHTML='<span class=\"ok\">&#10003; Update installed \u2014 rebooting\u2026</span>';}"
      "else if(d.state==='error'){"
        "var msg='Failed';"
        "if(d.http_status>0){msg+=' (HTTP '+d.http_status+')';}"
        "else if(d.tls_err){msg+=' (TLS -0x'+(-d.tls_err).toString(16).padStart(4,'0')+')';}"
        "else if(d.open_err_str&&d.open_err_str!=='ESP_OK'){msg+=' ('+d.open_err_str+')';}"
        "el.innerHTML='<span class=\"err\">\u26a0 '+msg+'</span>';}"
      "else{el.innerHTML='<span style=\"color:#9e9eb0\">'+d.state+'</span>';}}"
    "</script></body></html>";

/* ── React app (/app) ─────────────────────────────────────────── */

static esp_err_t handle_app(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *)WEB_BUNDLE_GZ, (ssize_t)WEB_BUNDLE_GZ_LEN);
    return ESP_OK;
}

static esp_err_t handle_ota(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_ota_html, (ssize_t)strlen(s_ota_html));
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t handle_update(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition found");
        web_server_ota_error = true;
        return ESP_FAIL;
    }

    esp_ota_handle_t ota;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA begin failed");
        web_server_ota_error = true;
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int want = total - received;
        if (want > OTA_BUF_SIZE) want = OTA_BUF_SIZE;
        int n = httpd_req_recv(req, s_ota_buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { esp_ota_abort(ota); web_server_ota_error = true; return ESP_FAIL; }
        if (esp_ota_write(ota, s_ota_buf, n) != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            web_server_ota_error = true;
            return ESP_FAIL;
        }
        received += n;
        web_server_ota_pct = received * 100 / total;
    }

    if (esp_ota_end(ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA end failed (corrupt image?)");
        web_server_ota_error = true;
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Set boot partition failed");
        web_server_ota_error = true;
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    web_server_ota_done = true;

    /* Reboot after a short delay so the HTTP response has time to flush. */
    xTaskCreate(reboot_task, "ota_rst", 2048, NULL, 5, NULL);

    return ESP_OK;
}

/* ── Startup ──────────────────────────────────────────────────── */

static httpd_handle_t s_server = NULL;

httpd_handle_t web_server_get_handle(void) { return s_server; }

void web_server_start(void)
{
    httpd_config_t cfg       = HTTPD_DEFAULT_CONFIG();
    cfg.recv_wait_timeout    = 60;
    cfg.send_wait_timeout    = 10;
    cfg.max_uri_handlers     = 16;   /* room for portal routes too */
    cfg.uri_match_fn         = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }
    httpd_handle_t server = s_server;

    static const httpd_uri_t app_uri = {
        .uri = "/app", .method = HTTP_GET, .handler = handle_app
    };
    static const httpd_uri_t history_uri = {
        .uri = "/history", .method = HTTP_GET, .handler = handle_history
    };
    static const httpd_uri_t history_json_uri = {
        .uri = "/api/history", .method = HTTP_GET, .handler = handle_history_json
    };
    static const httpd_uri_t history_clear_uri = {
        .uri = "/api/history", .method = HTTP_DELETE, .handler = handle_history_clear
    };
    static const httpd_uri_t history_delete_selected_uri = {
        .uri = "/api/history/delete", .method = HTTP_POST, .handler = handle_history_delete_selected
    };
    static const httpd_uri_t sensor_uri = {
        .uri = "/api/sensor", .method = HTTP_GET, .handler = handle_sensor
    };
    static const httpd_uri_t ota_uri = {
        .uri = "/ota", .method = HTTP_GET, .handler = handle_ota
    };
    static const httpd_uri_t update_uri = {
        .uri = "/update", .method = HTTP_POST, .handler = handle_update
    };
    static const httpd_uri_t ota_status_uri = {
        .uri = "/api/ota/status", .method = HTTP_GET, .handler = handle_ota_status
    };
    static const httpd_uri_t ota_check_uri = {
        .uri = "/api/ota/check", .method = HTTP_POST, .handler = handle_ota_check
    };
    static const httpd_uri_t ota_apply_uri = {
        .uri = "/api/ota/apply", .method = HTTP_POST, .handler = handle_ota_apply
    };

    httpd_register_uri_handler(server, &app_uri);
    httpd_register_uri_handler(server, &history_uri);
    httpd_register_uri_handler(server, &history_json_uri);
    httpd_register_uri_handler(server, &history_clear_uri);
    httpd_register_uri_handler(server, &history_delete_selected_uri);
    httpd_register_uri_handler(server, &sensor_uri);
    httpd_register_uri_handler(server, &ota_uri);
    httpd_register_uri_handler(server, &update_uri);
    httpd_register_uri_handler(server, &ota_status_uri);
    httpd_register_uri_handler(server, &ota_check_uri);
    httpd_register_uri_handler(server, &ota_apply_uri);

    ESP_LOGI(TAG, "Web server started — /app  /history  /ota  /update");
}
