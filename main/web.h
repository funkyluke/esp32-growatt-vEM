#pragma once

#include <stdbool.h>
#include "esp_http_server.h"
#include "cJSON.h"

void web_start(void);

/* For feature modules that add their own endpoints (call after web_start). */
void web_register(const httpd_uri_t *uri);

/* Basic-auth check; on false a 401 has been sent and the handler returns ESP_OK. */
bool web_check_auth(httpd_req_t *req);

/* Sends `o` as the JSON response and frees it. */
esp_err_t web_send_json(httpd_req_t *req, cJSON *o);
