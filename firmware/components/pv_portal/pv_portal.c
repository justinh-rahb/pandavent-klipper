#include "pv_portal.h"
#include "pv_dns.h"
#include "pv_moonraker.h"
#include "pv_policy.h"
#include "pv_wifi.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pv_portal";

static httpd_handle_t s_httpd = NULL;
static bool           s_ap_mode = false;

// ---------- URL-encoded form parsing ----------

// Extract a value for `key` from a url-encoded form body. NUL-terminates.
// Returns 0 on success, -1 if not found.
static int form_get(const char *body, const char *key, char *out, size_t out_sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < out_sz) {
                char c = *p++;
                if (c == '+') {
                    out[o++] = ' ';
                } else if (c == '%' && p[0] && p[1]) {
                    char hex[3] = { p[0], p[1], 0 };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    p += 2;
                } else {
                    out[o++] = c;
                }
            }
            out[o] = '\0';
            return 0;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return -1;
}

// Copy a value into a buffer with HTML-attribute-safe escaping.
static void html_escape(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 6 < out_sz; ++p) {
        const char *r = NULL;
        switch (*p) {
            case '&':  r = "&amp;";  break;
            case '<':  r = "&lt;";   break;
            case '>':  r = "&gt;";   break;
            case '"':  r = "&quot;"; break;
            case '\'': r = "&#39;";  break;
        }
        if (r) {
            size_t rl = strlen(r);
            memcpy(out + o, r, rl);
            o += rl;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

// Read the request body into `out`.
static int recv_body(httpd_req_t *req, char *out, size_t out_sz)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)out_sz) return -1;
    int off = 0;
    while (off < total) {
        int n = httpd_req_recv(req, out + off, total - off);
        if (n <= 0) return -1;
        off += n;
    }
    out[off] = '\0';
    return off;
}

// Parse a dotted-quad IPv4 string into host-byte-order uint32. Returns 0 on
// failure.
static uint32_t parse_ipv4(const char *s)
{
    if (s == NULL || *s == '\0') return 0;
    struct in_addr a;
    if (inet_aton(s, &a) == 0) return 0;
    return ntohl(a.s_addr);
}

// ---------- status/label helpers ----------

static const char *wifi_label(pv_wifi_state_t s)
{
    switch (s) {
        case PV_WIFI_STATE_INIT:            return "starting";
        case PV_WIFI_STATE_STA_CONNECTING:  return "connecting";
        case PV_WIFI_STATE_STA_CONNECTED:   return "STA connected";
        case PV_WIFI_STATE_AP_PORTAL:       return "AP mode (setup)";
    }
    return "?";
}

static const char *mk_label(pv_moonraker_state_t s)
{
    switch (s) {
        case PV_MK_DISABLED:      return "not configured";
        case PV_MK_DISCONNECTED:  return "disconnected";
        case PV_MK_CONNECTING:    return "connecting…";
        case PV_MK_CONNECTED:     return "handshaking";
        case PV_MK_SUBSCRIBED:    return "connected";
    }
    return "?";
}

static const char *target_label(pv_motor_target_t t)
{
    return t == PV_MOTOR_TARGET_OPEN ? "OPEN"
         : t == PV_MOTOR_TARGET_CLOSED ? "CLOSED" : "STOP";
}

// ---------- HTML rendering (chunked) ----------

#define SEND(req, buf) httpd_resp_send_chunk((req), (buf), HTTPD_RESP_USE_STRLEN)

static esp_err_t send_head(httpd_req_t *req)
{
    static const char *HEAD =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>PandaVent</title>"
"<style>"
"body{font-family:sans-serif;max-width:520px;margin:1em auto;padding:0 1em;color:#222}"
"h1{font-size:1.3em}h2{font-size:1.05em;margin-top:1.6em;border-bottom:1px solid #ddd;padding-bottom:.2em}"
"label{display:block;margin:.7em 0 .2em;font-size:.9em;color:#555}"
"input,button,select{width:100%;padding:.55em;font-size:1em;box-sizing:border-box}"
"button{margin-top:1em;background:#222;color:#fff;border:0;border-radius:4px;cursor:pointer}"
"button.secondary{background:#eee;color:#222;margin-top:.5em}"
".status{background:#f4f4f4;padding:.7em 1em;border-radius:4px;font-size:.9em;line-height:1.4}"
".status b{display:inline-block;min-width:110px}"
".row{display:flex;gap:.5em}.row>*{flex:1}"
".radios label{display:inline-block;margin-right:1em;font-size:1em;color:#222}"
".radios input{width:auto;margin-right:.3em}"
".hint{color:#666;font-size:.85em;margin-top:.2em}"
"</style></head><body>"
"<h1>PandaVent</h1>";
    return SEND(req, HEAD);
}

static esp_err_t send_status(httpd_req_t *req)
{
    pv_moonraker_status_t mk;
    pv_moonraker_get_status(&mk);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "<div class=\"status\">"
        "<div><b>WiFi:</b> %s</div>"
        "<div><b>Moonraker:</b> %s</div>"
        "<div><b>Printer state:</b> %s (bed %.1f\xC2\xB0""C)</div>"
        "<div><b>Vent target:</b> %s</div>"
        "<div><b>Mode:</b> %s</div>"
        "</div>",
        wifi_label(pv_wifi_state()),
        mk_label(mk.state),
        mk.printing ? "printing" : "idle",
        mk.bed_temp,
        target_label(pv_policy_get_target()),
        pv_policy_get_mode() == PV_POLICY_MODE_MANUAL ? "MANUAL" : "AUTO");
    return SEND(req, buf);
}

static esp_err_t send_wifi_section(httpd_req_t *req)
{
    // Header + save form open.
    SEND(req,
        "<h2>WiFi</h2>"
        "<form method=\"POST\" action=\"/wifi\">"
        "<label>Network</label>"
        "<select name=\"ssid\">"
        "<option value=\"\">— pick a network —</option>");

    // Scanned networks. `recs` is static so we don't put ~1.6 KB on the
    // httpd task's 4 KB stack; httpd is single-threaded, so no lock needed.
    static wifi_ap_record_t recs[PV_WIFI_SCAN_MAX];
    int n = pv_wifi_get_scan_results(recs, PV_WIFI_SCAN_MAX);
    for (int i = 0; i < n; ++i) {
        char ssid_esc[80];
        html_escape((const char *)recs[i].ssid, ssid_esc, sizeof(ssid_esc));
        // Two escaped SSIDs + fixed template, worst case ~200 bytes.
        char row[256];
        snprintf(row, sizeof(row),
            "<option value=\"%s\">%s (%d dBm%s)</option>",
            ssid_esc, ssid_esc,
            recs[i].rssi,
            recs[i].authmode == WIFI_AUTH_OPEN ? ", open" : "");
        SEND(req, row);
    }

    // Manual entry + password + save. Scan status hint.
    char tail[512];
    const char *scan_hint = pv_wifi_is_scanning()
        ? "<div class=\"hint\">Scanning…</div>"
        : (n == 0
            ? "<div class=\"hint\">No networks cached — click Scan.</div>"
            : "");
    snprintf(tail, sizeof(tail),
        "</select>"
        "%s"
        "<label>Or enter SSID manually (used if dropdown left blank)</label>"
        "<input name=\"ssid_manual\" maxlength=\"32\" autocomplete=\"off\">"
        "<label>Password</label>"
        "<input name=\"password\" type=\"password\" maxlength=\"64\" autocomplete=\"off\">"
        "<button>Save WiFi &amp; reboot</button>"
        "</form>"
        "<form method=\"POST\" action=\"/scan\">"
        "<button class=\"secondary\">Rescan networks</button>"
        "</form>",
        scan_hint);
    return SEND(req, tail);
}

static esp_err_t send_moonraker_section(httpd_req_t *req)
{
    pv_moonraker_config_t mk_cfg = {0};
    pv_moonraker_get_config(&mk_cfg);
    char host_esc[128];
    html_escape(mk_cfg.host, host_esc, sizeof(host_esc));

    char buf[800];
    snprintf(buf, sizeof(buf),
        "<h2>Moonraker</h2>"
        "<form method=\"POST\" action=\"/moonraker\">"
        "<div class=\"row\">"
          "<div><label>Host / IP</label><input name=\"host\" value=\"%s\" required maxlength=\"63\"></div>"
          "<div style=\"max-width:130px\"><label>Port</label><input name=\"port\" type=\"number\" value=\"%u\" min=\"1\" max=\"65535\"></div>"
        "</div>"
        "<label>API key (leave blank to keep current)</label>"
        "<input name=\"api_key\" maxlength=\"64\" autocomplete=\"off\">"
        "<button>Save Moonraker</button>"
        "</form>",
        host_esc, mk_cfg.port ? mk_cfg.port : 7125);
    return SEND(req, buf);
}

static esp_err_t send_mode_section(httpd_req_t *req)
{
    bool manual = pv_policy_get_mode() == PV_POLICY_MODE_MANUAL;
    pv_motor_target_t target = pv_policy_get_target();
    char buf[800];
    snprintf(buf, sizeof(buf),
        "<h2>Mode</h2>"
        "<form method=\"POST\" action=\"/mode\">"
        "<div class=\"radios\">"
          "<label><input type=\"radio\" name=\"mode\" value=\"auto\"%s>Auto</label>"
          "<label><input type=\"radio\" name=\"mode\" value=\"manual\"%s>Manual</label>"
        "</div>"
        "<label style=\"margin-top:1em\">Manual target (used in Manual mode)</label>"
        "<div class=\"radios\">"
          "<label><input type=\"radio\" name=\"manual_target\" value=\"open\"%s>Open</label>"
          "<label><input type=\"radio\" name=\"manual_target\" value=\"closed\"%s>Closed</label>"
        "</div>"
        "<button>Save Mode</button>"
        "</form>",
        manual ? "" : " checked",
        manual ? " checked" : "",
        target == PV_MOTOR_TARGET_OPEN ? " checked" : "",
        target != PV_MOTOR_TARGET_OPEN ? " checked" : "");
    return SEND(req, buf);
}

static esp_err_t send_ap_section(httpd_req_t *req)
{
    pv_wifi_ap_config_t ap = {0};
    pv_wifi_get_ap_config(&ap);
    char ssid_esc[80], pass_esc[128];
    html_escape(ap.ssid,     ssid_esc, sizeof(ssid_esc));
    html_escape(ap.password, pass_esc, sizeof(pass_esc));

    char buf[900];
    snprintf(buf, sizeof(buf),
        "<h2>AP Hotspot</h2>"
        "<form method=\"POST\" action=\"/ap_config\">"
        "<div class=\"hint\">Used when there's no saved WiFi or the saved network is unreachable. Saving reboots.</div>"
        "<label>SSID</label><input name=\"ap_ssid\" value=\"%s\" maxlength=\"32\">"
        "<label>Password (blank = keep, min 8 chars for WPA2)</label>"
        "<input name=\"ap_password\" value=\"%s\" maxlength=\"63\">"
        "<label>IP address</label>"
        "<input name=\"ap_ip\" value=\"%u.%u.%u.%u\" maxlength=\"15\">"
        "<button>Save AP &amp; reboot</button>"
        "</form>",
        ssid_esc, pass_esc,
        (unsigned)((ap.ip >> 24) & 0xFF),
        (unsigned)((ap.ip >> 16) & 0xFF),
        (unsigned)((ap.ip >>  8) & 0xFF),
        (unsigned)( ap.ip        & 0xFF));
    return SEND(req, buf);
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    send_head(req);
    send_status(req);
    send_wifi_section(req);
    send_moonraker_section(req);
    send_mode_section(req);
    send_ap_section(req);
    SEND(req, "</body></html>");
    httpd_resp_send_chunk(req, NULL, 0);   // terminate chunked stream
    return ESP_OK;
}

// ---------- POST handlers ----------

static esp_err_t handle_wifi_post(httpd_req_t *req)
{
    char body[512];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char ssid[33] = {0}, ssid_manual[33] = {0}, pass[65] = {0};
    form_get(body, "ssid",        ssid,        sizeof(ssid));
    form_get(body, "ssid_manual", ssid_manual, sizeof(ssid_manual));
    form_get(body, "password",    pass,        sizeof(pass));

    // Prefer the dropdown selection; fall back to manual entry for hidden APs.
    const char *chosen = ssid[0] ? ssid : ssid_manual;
    if (chosen[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no SSID chosen");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><body><h1>Saved. Rebooting…</h1></body>");
    pv_wifi_save_creds_and_reboot(chosen, pass);   // does not return
    return ESP_OK;
}

static esp_err_t handle_scan_post(httpd_req_t *req)
{
    esp_err_t err = pv_wifi_scan_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
    }
    // Small delay so the caller who reloads has a better chance of seeing
    // results without an extra manual refresh.
    vTaskDelay(pdMS_TO_TICKS(2500));
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_moonraker_post(httpd_req_t *req)
{
    char body[512];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    pv_moonraker_config_t cfg = {0};
    pv_moonraker_get_config(&cfg);   // start from current (preserves api_key on blank)

    char host[64] = {0}, port_str[8] = {0}, api_key[65] = {0};
    if (form_get(body, "host", host, sizeof(host)) != 0 || host[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing host");
        return ESP_OK;
    }
    strncpy(cfg.host, host, sizeof(cfg.host) - 1);

    if (form_get(body, "port", port_str, sizeof(port_str)) == 0 && port_str[0]) {
        long p = strtol(port_str, NULL, 10);
        if (p > 0 && p < 65536) cfg.port = (uint16_t)p;
    }
    if (form_get(body, "api_key", api_key, sizeof(api_key)) == 0 && api_key[0] != '\0') {
        strncpy(cfg.api_key, api_key, sizeof(cfg.api_key) - 1);
    }
    esp_err_t err = pv_moonraker_set_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "moonraker_set_config: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_mode_post(httpd_req_t *req)
{
    char body[128];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char mode[16] = {0}, manual_target[16] = {0};
    form_get(body, "mode", mode, sizeof(mode));
    form_get(body, "manual_target", manual_target, sizeof(manual_target));

    if (strcmp(mode, "manual") == 0) {
        pv_motor_target_t t = strcmp(manual_target, "open") == 0
                                  ? PV_MOTOR_TARGET_OPEN
                                  : PV_MOTOR_TARGET_CLOSED;
        pv_policy_set_manual_target(t);
        pv_policy_set_mode(PV_POLICY_MODE_MANUAL);
    } else {
        pv_policy_set_mode(PV_POLICY_MODE_AUTO);
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_ap_post(httpd_req_t *req)
{
    char body[512];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    pv_wifi_ap_config_t cfg = {0};
    char ap_ssid[33] = {0}, ap_pass[65] = {0}, ap_ip[32] = {0};
    form_get(body, "ap_ssid",     ap_ssid, sizeof(ap_ssid));
    form_get(body, "ap_password", ap_pass, sizeof(ap_pass));
    form_get(body, "ap_ip",       ap_ip,   sizeof(ap_ip));

    // Empty inputs mean "revert to default" for that field.
    strncpy(cfg.ssid,     ap_ssid, sizeof(cfg.ssid) - 1);
    strncpy(cfg.password, ap_pass, sizeof(cfg.password) - 1);
    cfg.ip = ap_ip[0] ? parse_ipv4(ap_ip) : 0;

    // Weak sanity check: WPA2 needs ≥ 8 chars. Allow open (empty) too.
    if (cfg.password[0] != '\0' && strlen(cfg.password) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "password must be 8+ chars or blank");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><body><h1>AP config saved. Rebooting…</h1></body>");
    pv_wifi_set_ap_config_and_reboot(&cfg);   // does not return
    return ESP_OK;
}

// AP-mode catch-all: 302 to /, so captive-portal detectors trigger.
static esp_err_t handle_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---------- start / stop ----------

static uint32_t get_ap_gateway_ip(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap == NULL) return 0;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(ap, &info) != ESP_OK) return 0;
    return info.gw.addr;
}

esp_err_t pv_portal_start(void)
{
    if (s_httpd != NULL) return ESP_ERR_INVALID_STATE;
    s_ap_mode = (pv_wifi_state() == PV_WIFI_STATE_AP_PORTAL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t root  = { .uri = "/",           .method = HTTP_GET,  .handler = handle_root };
    httpd_uri_t wifi  = { .uri = "/wifi",       .method = HTTP_POST, .handler = handle_wifi_post };
    httpd_uri_t scan  = { .uri = "/scan",       .method = HTTP_POST, .handler = handle_scan_post };
    httpd_uri_t mk    = { .uri = "/moonraker",  .method = HTTP_POST, .handler = handle_moonraker_post };
    httpd_uri_t mode  = { .uri = "/mode",       .method = HTTP_POST, .handler = handle_mode_post };
    httpd_uri_t apcfg = { .uri = "/ap_config",  .method = HTTP_POST, .handler = handle_ap_post };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &wifi);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &mk);
    httpd_register_uri_handler(s_httpd, &mode);
    httpd_register_uri_handler(s_httpd, &apcfg);

    // Kick off an initial background scan so the WiFi dropdown has entries by
    // the time the user loads the page.
    pv_wifi_scan_start();

    if (s_ap_mode) {
        uint32_t ip = get_ap_gateway_ip();
        if (ip != 0) pv_dns_start(ip);
        httpd_uri_t catchall = { .uri = "/*", .method = HTTP_GET, .handler = handle_captive_redirect };
        httpd_register_uri_handler(s_httpd, &catchall);
        ESP_LOGI(TAG, "portal up (AP mode, DNS on)");
    } else {
        ESP_LOGI(TAG, "portal up (STA mode)");
    }
    return ESP_OK;
}

esp_err_t pv_portal_stop(void)
{
    pv_dns_stop();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    return ESP_OK;
}
