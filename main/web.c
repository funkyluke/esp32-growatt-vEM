#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "settings.h"
#include "shelly.h"
#include "wifi.h"
#include "web.h"

static const char *TAG = "web";

static char s_power_json[640];

/* ---- dashboard ---- */
static const char *INDEX_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Shelly 3EM Power</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:720px;margin:2rem auto;padding:0 1rem;color:#222}"
    "h1{font-size:1.4rem}.sub{color:#888;font-size:.85rem}"
    ".grid{display:grid;grid-template-columns:repeat(3,1fr);gap:1rem;margin:1rem 0}"
    ".phase{background:#f6f6f6;border-radius:10px;padding:1rem;text-align:center}"
    ".phase .v{font-size:1.6rem;font-weight:700}.phase .u{color:#666;font-size:.75rem}"
    ".total{background:#eaf3fb;border-radius:10px;padding:1rem;text-align:center;margin-top:.5rem}"
    ".total .v{font-size:2rem;font-weight:800}.dim{color:#aaa}"
    ".badge{display:inline-block;padding:.1rem .6rem;border-radius:1rem;font-weight:700;font-size:.75rem;color:#fff;background:#999}"
    ".badge.fresh{background:#2e9e4f}.badge.aging{background:#e09a00}.badge.stale,.badge.lost{background:#c62828}"
    "body.stale .grid,body.stale .total{opacity:.4}"
    "</style></head>"
    "<body><div class='sub' style='float:right'><a href='/settings'>&#9881; Settings</a></div>"
    "<h1>&#9889; Shelly 3EM <span class='dim' id='shellyId'>...</span></h1>"
    "<div class='sub'><span class='badge' id='fresh'>connecting...</span> <span id='age'></span>"
    " &middot; uptime <span id='up'>--</span></div>"
    "<div class='grid'>"
    "<div class='phase'><div class='u'>L1 power</div><div class='v' id='p0'>--</div><div class='u' id='i0'>--</div></div>"
    "<div class='phase'><div class='u'>L2 power</div><div class='v' id='p1'>--</div><div class='u' id='i1'>--</div></div>"
    "<div class='phase'><div class='u'>L3 power</div><div class='v' id='p2'>--</div><div class='u' id='i2'>--</div></div>"
    "</div>"
    "<div class='total'><div class='u'>Total power</div><div class='v' id='tot'>--</div></div>"
    "<script>"
    "function set(id,v){var e=document.getElementById(id);if(v!=null)e.textContent=v;}"
    "function fmt(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);"
    "return (d?d+'d ':'')+(d||h?h+'h ':'')+m+'m '+(s%60)+'s';}"
    "function badge(c,t,age){var e=document.getElementById('fresh');e.className='badge '+c;e.textContent=t;"
    "document.body.classList.toggle('stale',c=='stale'||c=='lost'||c=='none');set('age',age);}"
    "function poll(){fetch('/api/power').then(r=>r.json()).then(d=>{"
    "set('shellyId',d.id);set('up',fmt(d.uptime_s));"
    "var a=d.age_ms>=0?(d.age_ms/1000).toFixed(1)+' s old':'no data yet';"
    "if(d.state=='fresh')badge('fresh','FRESH',a);"
    "else if(d.state=='aging')badge('aging','AGING',a);"
    "else if(d.state=='stale')badge('stale','STALE',a);"
    "else badge('none','NO DATA',a);"
    "if(d.phases){"
    "set('p0',d.phases[0].power!=null?d.phases[0].power.toFixed(0)+' W':'--');"
    "set('p1',d.phases[1].power!=null?d.phases[1].power.toFixed(0)+' W':'--');"
    "set('p2',d.phases[2].power!=null?d.phases[2].power.toFixed(0)+' W':'--');"
    "set('i0',d.phases[0].current!=null?d.phases[0].current.toFixed(2)+' A':'--');"
    "set('i1',d.phases[1].current!=null?d.phases[1].current.toFixed(2)+' A':'--');"
    "set('i2',d.phases[2].current!=null?d.phases[2].current.toFixed(2)+' A':'--');"
    "set('tot',d.total!=null?d.total.toFixed(0)+' W':'--');"
    "}}).catch(()=>{badge('lost','UNREACHABLE','device not answering');});}"
    "poll();setInterval(poll,2000);"
    "</script></body></html>";

/* ---- settings page ---- */
static const char *SETTINGS_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Settings</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:720px;margin:2rem auto;padding:0 1rem;color:#222}"
    "h1{font-size:1.4rem}details{background:#f6f6f6;border-radius:10px;margin:.8rem 0;padding:.2rem 1rem}"
    "summary{cursor:pointer;font-weight:700;padding:.7rem 0}"
    "label{display:block;margin:.8rem 0;font-weight:600}"
    "input{display:block;width:100%;box-sizing:border-box;padding:.5rem;margin:.3rem 0;font-size:1rem;border:1px solid #bbb;border-radius:6px}"
    "small{display:block;color:#777;font-weight:400}"
    "button{padding:.6rem 1.4rem;font-size:1rem;border:0;border-radius:6px;background:#1565c0;color:#fff;cursor:pointer}"
    "#msg{margin:1rem 0;padding:.6rem 1rem;border-radius:6px;display:none}"
    ".ok{background:#e3f4e8;display:block!important}.err{background:#fde8e8;display:block!important}"
    "</style></head><body>"
    "<div style='float:right'><a href='/'>&larr; Dashboard</a></div><h1>&#9881; Settings</h1>"
    "<div id='msg'></div><div id='cats'>loading...</div>"
    "<button onclick='save()'>Save</button>"
    "<script>"
    "var F={};"
    "function E(t,p,x){var e=document.createElement(t);for(var k in p)e[k]=p[k];"
    "(x||[]).forEach(function(c){e.appendChild(typeof c=='string'?document.createTextNode(c):c)});return e;}"
    "function msg(t,c){var m=document.getElementById('msg');m.textContent=t;m.className=c||'ok';}"
    "function load(){fetch('/api/settings').then(function(r){return r.json()}).then(function(d){"
    "var box=document.getElementById('cats');box.innerHTML='';F={};"
    "d.categories.forEach(function(c,ci){var det=E('details',{open:ci==0},[E('summary',{},[c.name])]);"
    "c.fields.forEach(function(f){F[f.key]=f;"
    "var i=E('input',{type:f.type,id:'f_'+f.key});"
    "if(f.type=='number'){i.min=f.min;i.max=f.max;i.value=f.value;}"
    "else{i.maxLength=f.maxlen;if(f.secret){i.placeholder=f.is_set?'(unchanged)':'(not set)';i.autocomplete='new-password';}else i.value=f.value;}"
    "det.appendChild(E('label',{},[f.label,i,E('small',{},[f.help])]));});"
    "box.appendChild(det);});"
    "if(d.wifi_apply)msg(d.wifi_apply);}).catch(function(){msg('Device not answering','err');});}"
    "function save(){var o={};for(var k in F){var v=document.getElementById('f_'+k).value;"
    "o[k]=F[k].type=='number'?Number(v):v;}"
    "fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)})"
    ".then(function(r){return r.json()}).then(function(d){"
    "if(!d.ok){msg(d.error,'err');return;}"
    "if(d.wifi_pending){msg('Switching WiFi... this page may drop for up to 30 s.');setTimeout(load,35000);}"
    "else{msg(d.changed?'Saved.':'Nothing changed.');load();}"
    "}).catch(function(){msg('Lost connection (expected if you changed WiFi).','err');});}"
    "load();"
    "</script></body></html>";

static esp_err_t send_html(httpd_req_t *req, const char *html) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t index_handler(httpd_req_t *req) { return send_html(req, INDEX_HTML); }
static esp_err_t settings_page_handler(httpd_req_t *req) { return send_html(req, SETTINGS_HTML); }

static esp_err_t status_handler(httpd_req_t *req) {
    settings_t cfg;
    settings_copy(&cfg);
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(wifi_ip()));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddStringToObject(o, "wifi", cfg.wifi_ssid);   /* escaped by cJSON */
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t power_handler(httpd_req_t *req) {
    shelly_snapshot_t snap;
    shelly_get_snapshot(&snap);
    settings_t cfg;
    settings_copy(&cfg);

    long long fresh_ms = cfg.shelly_poll_s * 2500LL, aging_ms = cfg.shelly_poll_s * 7500LL;
    if (fresh_ms < 5000) fresh_ms = 5000;
    if (aging_ms < 15000) aging_ms = 15000;
    int64_t now_us = esp_timer_get_time();
    long long age_ms = (snap.last_ok_us > 0) ? (long long)((now_us - snap.last_ok_us) / 1000) : -1;
    const char *state = (age_ms < 0) ? "none" : (age_ms < fresh_ms) ? "fresh"
                      : (age_ms < aging_ms) ? "aging" : "stale";
    char diag_url[96];
    int diag_status = shelly_diag(diag_url, sizeof(diag_url));

    int len = snprintf(s_power_json, sizeof(s_power_json),
        "{\"id\":\"%s\",\"ok\":%s,\"state\":\"%s\",\"age_ms\":%lld,\"uptime_s\":%lld,"
        "\"total\":%.2f,\"err\":%d,\"url\":\"%s\","
        "\"phases\":["
        "{\"power\":%.2f,\"current\":%.2f,\"voltage\":%.2f},"
        "{\"power\":%.2f,\"current\":%.2f,\"voltage\":%.2f},"
        "{\"power\":%.2f,\"current\":%.2f,\"voltage\":%.2f}"
        "]}",
        cfg.shelly_id,
        (strcmp(state, "fresh") == 0 || strcmp(state, "aging") == 0) ? "true" : "false",
        state, age_ms, (long long)(now_us / 1000000),
        snap.total_power, diag_status, diag_url,
        snap.phase[0].power, snap.phase[0].current, snap.phase[0].voltage,
        snap.phase[1].power, snap.phase[1].current, snap.phase[1].voltage,
        snap.phase[2].power, snap.phase[2].current, snap.phase[2].voltage);
    if (len < 0 || len >= (int)sizeof(s_power_json)) len = (int)sizeof(s_power_json) - 1;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, s_power_json, len);
    return ESP_OK;
}

static esp_err_t settings_get_handler(httpd_req_t *req) {
    char *json = settings_to_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t settings_post_handler(httpd_req_t *req) {
    /* Requiring a JSON content type forces a CORS preflight, so a foreign web
     * page in the user's browser cannot post settings (CSRF). */
    char ctype[48] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));
    if (strncmp(ctype, "application/json", 16) != 0)
        return reply(req, "415 Unsupported Media Type", "{\"ok\":false,\"error\":\"Content-Type must be application/json\"}");
    if (req->content_len == 0 || req->content_len > 1024)
        return reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"Bad request size\"}");
    if (wifi_apply_busy())
        return reply(req, "409 Conflict", "{\"ok\":false,\"error\":\"A WiFi change is still being applied, try again in a moment\"}");

    char *body = malloc(req->content_len + 1);
    if (!body) return reply(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"Out of memory\"}");
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) { free(body); return ESP_FAIL; }
        got += n;
    }
    body[got] = '\0';
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
    }

    settings_t old;
    uint32_t changed = 0, wifi_mask = 0;
    bool shelly_changed = false;
    char err[160];
    if (!settings_apply(root, &old, &changed, &wifi_mask, &shelly_changed, err, sizeof(err))) {
        cJSON_Delete(root);
        char out[220];
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", err);
        return reply(req, "400 Bad Request", out);
    }
    cJSON_Delete(root);

    /* WiFi fields are persisted only after the new network really works. */
    esp_err_t pe = settings_persist(changed & ~wifi_mask);
    if (shelly_changed) shelly_reresolve();

    bool wifi_pending = false;
    if (wifi_mask) { wifi_apply(&old, wifi_mask); wifi_pending = true; }

    char out[96];
    snprintf(out, sizeof(out), "{\"ok\":%s,\"changed\":%d,\"wifi_pending\":%s}",
             pe == ESP_OK ? "true" : "false", __builtin_popcount(changed), wifi_pending ? "true" : "false");
    return reply(req, "200 OK", out);
}

void web_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) return;

    httpd_uri_t index = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_register_uri_handler(server, &index);
    httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_register_uri_handler(server, &status);
    httpd_uri_t power = { .uri = "/api/power", .method = HTTP_GET, .handler = power_handler };
    httpd_register_uri_handler(server, &power);
    httpd_uri_t set_page = { .uri = "/settings", .method = HTTP_GET, .handler = settings_page_handler };
    httpd_register_uri_handler(server, &set_page);
    httpd_uri_t set_get = { .uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler };
    httpd_register_uri_handler(server, &set_get);
    httpd_uri_t set_post = { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler };
    httpd_register_uri_handler(server, &set_post);

    ESP_LOGI(TAG, "web server started on http://" IPSTR, IP2STR(wifi_ip()));
}
