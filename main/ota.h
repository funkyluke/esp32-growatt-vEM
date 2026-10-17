#pragma once

#include "esp_http_server.h"

/* If the running image is still pending verification (first boot after an
 * update), mark it valid once it has run for a while without crashing.
 * A crash or reset before that rolls back to the previous image. */
void ota_init(void);

/* POST handler: the request body is the raw firmware .bin. */
esp_err_t ota_upload_handler(httpd_req_t *req);
