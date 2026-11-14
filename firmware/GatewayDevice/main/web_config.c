#include "web_config.h"
#include "NVS.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static const char *TAG = "web_config";
static httpd_handle_t s_server = NULL;

// Shared style block -- red-only palette, white background, red-outlined
// centered box. Sent as its own chunk (see send_page below) so its size
// never has to be accounted for in any handler's snprintf buffer.
static const char *STYLE_BLOCK =
    "<style>"
    "body{background:#ffffff;color:#000000;font-family:sans-serif;"
    "display:flex;justify-content:center;align-items:flex-start;"
    "min-height:100vh;margin:0;padding-top:40px;}"
    ".box{border:2px solid #d40000;border-radius:4px;padding:24px 32px;"
    "width:340px;text-align:center;}"
    "h2,h3,p,a{color:#000000;}"
    "input[type=text],input:not([type]){border:1px solid #d40000;color:#000000;"
    "padding:4px;margin:4px 0;width:90%;}"
    "input[type=submit]{background:#ffffff;border:2px solid #d40000;color:#000000;"
    "padding:6px 16px;margin-top:10px;cursor:pointer;}"
    "input[type=submit]:hover{background:#d40000;color:#ffffff;}"
    "a{text-decoration:none;font-weight:bold;}"
    ".err{color:#d40000;font-weight:bold;}"
    "</style>";

/* -------------------------------------------------------------------
 * Small helper: pull one field out of a application/x-www-form-urlencoded
 * POST body into caller's buffer. Returns true if the field was present
 * AND non-empty (this is what "blank = keep existing" hinges on).
 * ------------------------------------------------------------------- */
static bool form_get_field(const char *body, const char *key, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }

    out[0] = '\0';

    if (body == NULL || key == NULL) {
        return false;
    }

    if (httpd_query_key_value(body, key, out, out_size) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

static bool parse_uint32(const char *field, uint32_t max_value, uint32_t *value)
{
    if (field == NULL || field[0] == '\0' || value == NULL) {
        return false;
    }

    for (const char *digit = field; *digit != '\0'; digit++) {
        if (*digit < '0' || *digit > '9') {
            return false;
        }
    }

    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(field, &end, 10);
    if (errno == ERANGE || end == field || *end != '\0' || parsed > max_value) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool form_get_uint32(const char *body, const char *key, uint32_t max_value,
                            uint32_t *value, bool *invalid_input)
{
    char field[32];
    esp_err_t err = httpd_query_key_value(body, key, field, sizeof(field));
    if (err == ESP_ERR_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        *invalid_input = true;
        return false;
    }
    if (field[0] == '\0') {
        return false;
    }
    if (!parse_uint32(field, max_value, value)) {
        *invalid_input = true;
        return false;
    }
    return true;
}
/*
static bool copy_string_field(char *destination, size_t destination_size, const char *source)
{
    if (destination == NULL || destination_size == 0) {
        return false;
    }

    destination[0] = '\0';

    if (source == NULL) {
        return false;
    }

    size_t source_length = strnlen(source, destination_size);

    bool fits = (source_length < destination_size);
    size_t copy_size = fits ? source_length : (destination_size - 1);

    memcpy(destination, source, copy_size);
    destination[copy_size] = '\0';

    return fits;
}
*/
static bool copy_string_field(char *destination, size_t destination_size, const char *source)
{
    // Reject unusable destination (no buffer, or zero capacity)
    if (destination == NULL || destination_size == 0) {
        return false;
    }

    // Start with an empty string so the buffer is valid even on early return
    destination[0] = '\0';

    // Nothing to copy from
    if (source == NULL) {
        return false;
    }

    // strnlen never reads more than destination_size bytes of source,
    // so it is safe even if source is not NUL-terminated.
    size_t source_length = strnlen(source, destination_size);

    // length < size means the string plus its NUL fits in the buffer
    bool fits = (source_length < destination_size);

    // Copy everything if it fits, otherwise leave 1 byte for the NUL
    size_t copy_size = fits ? source_length : (destination_size - 1);

    // Manual bounded copy: i never reaches destination_size - 1 or beyond,
    // because copy_size <= destination_size - 1
    for (size_t i = 0; i < copy_size; i++) {
        destination[i] = source[i];
    }

    // Always terminate manually
    destination[copy_size] = '\0';

    return fits;
}


/* NEW -- checkbox helper. A checked box sends name=on in the POST body;
 * an UNCHECKED box sends nothing at all -- there is no "blank value" to
 * detect. So this is presence-only: does the key exist in the body at
 * all, regardless of what it's set to. Used only for the three new
 * enable/disable flags below -- do NOT use this for text fields, it
 * would break "blank = keep existing" for those. */
static bool form_has_field(const char *body, const char *key)
{
    char tmp[8];
    return httpd_query_key_value(body, key, tmp, sizeof(tmp)) == ESP_OK;
}

/* Reads the full POST body into a heap buffer. Caller must free(). */
static char *read_post_body(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 1024) {
        ESP_LOGW(TAG, "Rejecting POST body of size %d", total_len);
        return NULL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) return NULL;

    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, buf + received, total_len - received);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        received += r;
    }
    buf[total_len] = '\0';
    return buf;
}

/* NEW -- reads the "error" key out of the request's URL query string,
 * e.g. GET /modbus?error=tcp_needs_eth2. Returns true and fills out[]
 * if present, false otherwise. Used so a rejected POST can redirect
 * back to the GET page with a reason the operator can actually see. */
static bool get_url_error_param(httpd_req_t *req, char *out, size_t out_size)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, "error", out, out_size) == ESP_OK;
}

/* NEW -- maps our internal error codes to an operator-facing sentence.
 * Kept as one place so wording changes don't need touching handlers. */
static const char *error_code_to_message(const char *code)
{
    if (strcmp(code, "tcp_needs_eth2") == 0) {
        return "Modbus TCP requires Ethernet Module 2 to be enabled first. TCP was NOT enabled.";
    }
    return "Invalid input -- change was not saved.";
}

/* NEW -- sends <html><head>+STYLE_BLOCK+</head><body><div class="box">
 * as separate chunks, then the caller's page-specific body_html, then
 * closes the tags. Because STYLE_BLOCK is sent as its own chunk (not
 * snprintf'd into a fixed buffer alongside page content), no handler's
 * buffer size ever has to account for the style block's length -- this
 * is what fixes the format-truncation errors from before. */
static void send_page(httpd_req_t *req, const char *body_html)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, "<html><head>");
    httpd_resp_sendstr_chunk(req, STYLE_BLOCK);
    httpd_resp_sendstr_chunk(req, "</head><body><div class=\"box\">");
    httpd_resp_sendstr_chunk(req, body_html);
    httpd_resp_sendstr_chunk(req, "</div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);   // NULL chunk terminates the response
}

/* =====================================================================
 * LANDING PAGE  (GET /)
 * ===================================================================== */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    // No dynamic fields on this page -- plain literal, no snprintf needed.
    const char *body =
        "<h2>Gateway Device - Config Mode</h2>"
        "<p><a href=\"/mode\">Device Mode (CNC / Press)</a></p>"
        "<p><a href=\"/eth\">Ethernet (Module 1 + 2)</a></p>"
        "<p><a href=\"/modbus\">Modbus (RTU + TCP)</a></p>"
        "<p><a href=\"/mqtt\">MQTT Broker + Device Name</a></p>"
        "<hr>"
        "<p><a href=\"/reboot\">Done configuring - Reboot now</a></p>";

    send_page(req, body);
    return ESP_OK;
}

/* =====================================================================
 * NEW PAGE: DEVICE MODE  (GET/POST /mode)
 * ===================================================================== */
static esp_err_t mode_get_handler(httpd_req_t *req)
{
    device_mode_t mode = nvs_storage_get_device_mode();
    const char *sel_cnc   = (mode == DEVICE_MODE_CNC)   ? "selected" : "";
    const char *sel_press = (mode == DEVICE_MODE_PRESS) ? "selected" : "";

    char body[500];
    snprintf(body, sizeof(body),
        "<h2>Device Mode</h2>"
        "<form method=\"POST\" action=\"/mode\">"
        "Mode: <select name=\"mode\">"
        "<option value=\"CNC\" %s>CNC</option>"
        "<option value=\"PRESS\" %s>Press</option>"
        "</select><br><br>"
        "<input type=\"submit\" value=\"Save\">"
        "</form>"
        "<p>Takes effect after reboot.</p>"
        "<p><a href=\"/\">Back</a></p>",
        sel_cnc, sel_press);

    send_page(req, body);
    return ESP_OK;
}

static esp_err_t mode_post_handler(httpd_req_t *req)
{
    char *body = read_post_body(req);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char field[16];
    if (form_get_field(body, "mode", field, sizeof(field))) {
        device_mode_t mode = (strcmp(field, "PRESS") == 0) ? DEVICE_MODE_PRESS : DEVICE_MODE_CNC;
        nvs_storage_save_device_mode(mode);
    }

    free(body);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/mode");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* =====================================================================
 * PAGE 1: ETHERNET MODULE 1 + MODULE 2   (GET/POST /eth)
 * ===================================================================== */
static esp_err_t eth_get_handler(httpd_req_t *req)
{
    const eth_config_t  *e1 = nvs_storage_get_eth_config();
    const eth2_config_t *e2 = nvs_storage_get_eth2_config();
    const char *e2_checked = nvs_storage_get_eth2_enabled() ? "checked" : "";

    char body[950];   // +~50 bytes for the new checkbox row
    snprintf(body, sizeof(body),
        "<h2>Ethernet Config</h2>"
        "<form method=\"POST\" action=\"/eth\">"
        "<h3>Module 1</h3>"
        "IP: <input name=\"e1_ip\" value=\"%s\"><br>"
        "Gateway: <input name=\"e1_gw\" value=\"%s\"><br>"
        "Netmask: <input name=\"e1_mask\" value=\"%s\"><br>"
        "<h3>Module 2</h3>"
        "Enable Module 2: <input type=\"checkbox\" name=\"e2_en\" %s><br>"
        "IP: <input name=\"e2_ip\" value=\"%s\"><br>"
        "Gateway: <input name=\"e2_gw\" value=\"%s\"><br>"
        "Netmask: <input name=\"e2_mask\" value=\"%s\"><br><br>"
        "<input type=\"submit\" value=\"Save\">"
        "</form>"
        "<p>Leave a text field blank to keep its current value.</p>"
        "<p><a href=\"/\">Back</a></p>",
        e1->ip, e1->gateway, e1->netmask,
        e2_checked, e2->ip, e2->gateway, e2->netmask);

    send_page(req, body);
    return ESP_OK;
}

static esp_err_t eth_post_handler(httpd_req_t *req)
{
    char *body = read_post_body(req);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    eth_config_t  e1 = *nvs_storage_get_eth_config();
    eth2_config_t e2 = *nvs_storage_get_eth2_config();

    char field[32];
    if (form_get_field(body, "e1_ip",   field, sizeof(field))) copy_string_field(e1.ip, sizeof(e1.ip), field);
    if (form_get_field(body, "e1_gw",   field, sizeof(field))) copy_string_field(e1.gateway, sizeof(e1.gateway), field);
    if (form_get_field(body, "e1_mask", field, sizeof(field))) copy_string_field(e1.netmask, sizeof(e1.netmask), field);
    if (form_get_field(body, "e2_ip",   field, sizeof(field))) copy_string_field(e2.ip, sizeof(e2.ip), field);
    if (form_get_field(body, "e2_gw",   field, sizeof(field))) copy_string_field(e2.gateway, sizeof(e2.gateway), field);
    if (form_get_field(body, "e2_mask", field, sizeof(field))) copy_string_field(e2.netmask, sizeof(e2.netmask), field);

    // NEW -- checkbox, presence-only (see form_has_field comment above)
    bool e2_enabled = form_has_field(body, "e2_en");

    free(body);

    nvs_storage_save_eth_config(&e1);
    nvs_storage_save_eth2_config(&e2);
    nvs_storage_save_eth2_enabled(e2_enabled);

    // NEW -- if operator just turned eth2 OFF while modbus TCP was ON,
    // TCP would be left in an invalid state after reboot (main.c would
    // skip it and only log). Force TCP off here too so the web UI
    // reflects reality instead of showing a checkbox that silently does
    // nothing on next boot.
    if (!e2_enabled && nvs_storage_get_modbus_tcp_enabled()) {
        nvs_storage_save_modbus_tcp_enabled(false);
        ESP_LOGW(TAG, "Eth2 disabled -- Modbus TCP was also force-disabled");
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/eth");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* =====================================================================
 * PAGE 2: MODBUS RTU + MODBUS TCP   (GET/POST /modbus)
 * ===================================================================== */
static esp_err_t modbus_get_handler(httpd_req_t *req)
{
    const modbus_rtu_config_t *rtu = nvs_storage_get_rtu_config();
    const modbus_tcp_config_t *tcp = nvs_storage_get_tcp_config();

    const char *sel_none = (rtu->parity == UART_PARITY_DISABLE) ? "selected" : "";
    const char *sel_even = (rtu->parity == UART_PARITY_EVEN)    ? "selected" : "";
    const char *sel_odd  = (rtu->parity == UART_PARITY_ODD)     ? "selected" : "";
    const char *rtu_checked = nvs_storage_get_modbus_rtu_enabled() ? "checked" : "";
    const char *tcp_checked = nvs_storage_get_modbus_tcp_enabled() ? "checked" : "";

    // NEW -- surface a rejected-save reason, if the redirect carried one
    char err_code[32];
    char err_html[160];
    err_html[0] = '\0';
    if (get_url_error_param(req, err_code, sizeof(err_code))) {
        snprintf(err_html, sizeof(err_html), "<p class=\"err\">%s</p>", error_code_to_message(err_code));
    }

    char body[1350];   // +~250 bytes for two checkboxes + error line
    snprintf(body, sizeof(body),
        "<h2>Modbus Config</h2>"
        "%s"
        "<form method=\"POST\" action=\"/modbus\">"
        "<h3>RTU</h3>"
        "Enable RTU: <input type=\"checkbox\" name=\"rtu_en\" %s><br>"
        "Slave Addr: <input name=\"rtu_addr\" value=\"%u\"><br>"
        "Poll (ms): <input name=\"rtu_poll\" value=\"%lu\"><br>"
        "Baud Rate: <input name=\"rtu_baud\" value=\"%lu\"><br>"
        "Parity: <select name=\"rtu_parity\">"
        "<option value=\"None\" %s>None</option>"
        "<option value=\"Even\" %s>Even</option>"
        "<option value=\"Odd\" %s>Odd</option>"
        "</select><br>"
        "<h3>TCP (requires Ethernet Module 2 enabled)</h3>"
        "Enable TCP: <input type=\"checkbox\" name=\"tcp_en\" %s><br>"
        "IP: <input name=\"tcp_ip\" value=\"%s\"><br>"
        "Port: <input name=\"tcp_port\" value=\"%u\"><br>"
        "Slave Addr: <input name=\"tcp_addr\" value=\"%u\"><br>"
        "Poll (ms): <input name=\"tcp_poll\" value=\"%lu\"><br><br>"
        "<input type=\"submit\" value=\"Save\">"
        "</form>"
        "<p>Leave a text field blank to keep its current value.</p>"
        "<p><a href=\"/\">Back</a></p>",
        err_html,
        rtu_checked, rtu->slave_addr, (unsigned long)rtu->poll_every,
        (unsigned long)rtu->baud_rate,
        sel_none, sel_even, sel_odd,
        tcp_checked, tcp->ip, tcp->port, tcp->slave_addr, (unsigned long)tcp->poll_every);

    send_page(req, body);
    return ESP_OK;
}

static esp_err_t modbus_post_handler(httpd_req_t *req)
{
    char *body = read_post_body(req);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    modbus_rtu_config_t rtu = *nvs_storage_get_rtu_config();
    modbus_tcp_config_t tcp = *nvs_storage_get_tcp_config();

    char field[32];
    uint32_t value;
    bool invalid_input = false;
    if (form_get_uint32(body, "rtu_addr", UINT8_MAX, &value, &invalid_input)) rtu.slave_addr = (uint8_t)value;
    if (form_get_uint32(body, "rtu_poll", UINT32_MAX, &value, &invalid_input)) rtu.poll_every = value;
    if (form_get_uint32(body, "rtu_baud", UINT32_MAX, &value, &invalid_input)) rtu.baud_rate = value;

    if (form_get_field(body, "rtu_parity", field, sizeof(field))) {
        if (strcmp(field, "Even") == 0)      rtu.parity = UART_PARITY_EVEN;
        else if (strcmp(field, "Odd") == 0)  rtu.parity = UART_PARITY_ODD;
        else                                  rtu.parity = UART_PARITY_DISABLE;  // "None" or anything unrecognized
    }

    if (form_get_field(body, "tcp_ip", field, sizeof(field))) copy_string_field(tcp.ip, sizeof(tcp.ip), field);
    if (form_get_uint32(body, "tcp_port", UINT16_MAX, &value, &invalid_input)) tcp.port = (uint16_t)value;
    if (form_get_uint32(body, "tcp_addr", UINT8_MAX, &value, &invalid_input)) tcp.slave_addr = (uint8_t)value;
    if (form_get_uint32(body, "tcp_poll", UINT32_MAX, &value, &invalid_input)) tcp.poll_every = value;

    if (invalid_input) {
        free(body);
        ESP_LOGW(TAG, "Rejecting Modbus configuration with invalid numeric input");
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/modbus?error=invalid_input");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // NEW -- checkboxes, presence-only
    bool rtu_enabled = form_has_field(body, "rtu_en");
    bool tcp_enabled = form_has_field(body, "tcp_en");

    free(body);

    nvs_storage_save_rtu_config(&rtu);
    nvs_storage_save_tcp_config(&tcp);
    nvs_storage_save_modbus_rtu_enabled(rtu_enabled);

    // NEW -- validation: TCP rides eth2, refuse to enable TCP without it.
    // RTU has no such dependency so it always saves as requested.
    bool reject_tcp = tcp_enabled && !nvs_storage_get_eth2_enabled();
    nvs_storage_save_modbus_tcp_enabled(reject_tcp ? false : tcp_enabled);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", reject_tcp ? "/modbus?error=tcp_needs_eth2" : "/modbus");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* =====================================================================
 * PAGE 3: MQTT BROKER + DEVICE NAME   (GET/POST /mqtt)
 * ===================================================================== */
static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    const mqtt_config_t   *mq = nvs_storage_get_mqtt_config();
    const device_config_t *dev = nvs_storage_get_device_config();

    char err_code[32];
    char err_html[160];
    err_html[0] = '\0';
    if (get_url_error_param(req, err_code, sizeof(err_code))) {
        snprintf(err_html, sizeof(err_html), "<p class=\"err\">%s</p>", error_code_to_message(err_code));
    }

    char body[860];
    snprintf(body, sizeof(body),
        "<h2>MQTT / Device Config</h2>"
        "%s"
        "<form method=\"POST\" action=\"/mqtt\">"
        "<h3>MQTT Broker</h3>"
        "IP: <input name=\"mqtt_ip\" value=\"%s\"><br>"
        "Port: <input name=\"mqtt_port\" value=\"%u\"><br>"
        "<h3>Device</h3>"
        "Machine Name: <input name=\"dev_name\" value=\"%s\"><br><br>"
        "<input type=\"submit\" value=\"Save\">"
        "</form>"
        "<p>Leave a field blank to keep its current value.</p>"
        "<p><a href=\"/\">Back</a></p>",
        err_html, mq->broker_ip, mq->broker_port, dev->name);

    send_page(req, body);
    return ESP_OK;
}

static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
    char *body = read_post_body(req);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    mqtt_config_t   mq  = *nvs_storage_get_mqtt_config();
    device_config_t dev = *nvs_storage_get_device_config();

    char field[32];
    bool invalid_input = false;
    if (form_get_field(body, "mqtt_ip", field, sizeof(field))) copy_string_field(mq.broker_ip, sizeof(mq.broker_ip), field);
    uint32_t value;
    if (form_get_uint32(body, "mqtt_port", UINT16_MAX, &value, &invalid_input)) mq.broker_port = (uint16_t)value;
    if (form_get_field(body, "dev_name", field, sizeof(field))) copy_string_field(dev.name, sizeof(dev.name), field);

    free(body);

    if (invalid_input) {
        ESP_LOGW(TAG, "Rejecting MQTT configuration with invalid numeric input");
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/mqtt?error=invalid_input");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    nvs_storage_save_mqtt_config(&mq);
    nvs_storage_save_device_config(&dev);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/mqtt");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* =====================================================================
 * SHARED REBOOT ENDPOINT  (GET /reboot)
 * ===================================================================== */
static esp_err_t reboot_get_handler(httpd_req_t *req)
{
    // No dynamic fields -- plain literal, no snprintf needed.
    const char *body =
        "<h2>Rebooting now...</h2>"
        "<p>Device will restart with the saved configuration.</p>";

    send_page(req, body);

    ESP_LOGI(TAG, "Operator requested reboot from config page");
    vTaskDelay(pdMS_TO_TICKS(500)); // let the response flush before we drop the link
    esp_restart();
    return ESP_OK; // unreachable
}

/* =====================================================================
 * START / STOP
 * ===================================================================== */
esp_err_t web_config_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    // Default is 8 -- we're at 10 routes now (root, mode x2, eth x2,
    // modbus x2, mqtt x2, reboot). Past-8 registrations were failing
    // silently, which is what caused /reboot's 404. Headroom of 4
    // added for whatever page comes next.
    config.max_uri_handlers = 14;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",        .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/mode",    .method = HTTP_GET,  .handler = mode_get_handler },
        { .uri = "/mode",    .method = HTTP_POST, .handler = mode_post_handler },
        { .uri = "/eth",     .method = HTTP_GET,  .handler = eth_get_handler },
        { .uri = "/eth",     .method = HTTP_POST, .handler = eth_post_handler },
        { .uri = "/modbus",  .method = HTTP_GET,  .handler = modbus_get_handler },
        { .uri = "/modbus",  .method = HTTP_POST, .handler = modbus_post_handler },
        { .uri = "/mqtt",    .method = HTTP_GET,  .handler = mqtt_get_handler },
        { .uri = "/mqtt",    .method = HTTP_POST, .handler = mqtt_post_handler },
        { .uri = "/reboot",  .method = HTTP_GET,  .handler = reboot_get_handler },
    };

    // CHANGED: now checks the return value. Before, a full handler table
    // failed silently -- routes just didn't work, with only a runtime
    // 404 as the symptom, no build-time or boot-time indication of why.
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t reg_err = httpd_register_uri_handler(s_server, &routes[i]);
        if (reg_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register route %s (method %d): %s",
                     routes[i].uri, routes[i].method, esp_err_to_name(reg_err));
        }
    }

    ESP_LOGI(TAG, "Config web server started");
    return ESP_OK;
}

void web_config_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
