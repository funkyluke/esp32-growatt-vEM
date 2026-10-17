#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "ota.h"

static const char *TAG = "ota";

#define CONFIRM_AFTER_MS  60000

static void confirm_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(CONFIRM_AFTER_MS));
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        ESP_LOGI(TAG, "new firmware confirmed");
    vTaskDelete(NULL);
}

void ota_init(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    ESP_LOGI(TAG, "running %s from %s", esp_app_get_description()->version, run->label);
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "first boot of a new image, confirming in %d s", CONFIRM_AFTER_MS / 1000);
        xTaskCreate(confirm_task, "ota_confirm", 2048, NULL, 1, NULL);
    }
}

static void reboot_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg) {
    ESP_LOGE(TAG, "%s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

esp_err_t ota_upload_handler(httpd_req_t *req) {
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return fail(req, "500 Internal Server Error", "No OTA partition (flash once over USB with the OTA partition table)");
    if (req->content_len == 0 || req->content_len > part->size)
        return fail(req, "400 Bad Request", "Firmware size does not fit the OTA partition");

    esp_ota_handle_t h;
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) return fail(req, "500 Internal Server Error", esp_err_to_name(e));
    ESP_LOGI(TAG, "receiving %d bytes into %s", (int)req->content_len, part->label);

    char *buf = malloc(4096);
    if (!buf) { esp_ota_abort(h); return fail(req, "500 Internal Server Error", "Out of memory"); }
    size_t left = req->content_len;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { free(buf); esp_ota_abort(h); ESP_LOGE(TAG, "upload aborted"); return ESP_FAIL; }
        if ((e = esp_ota_write(h, buf, n)) != ESP_OK) {
            free(buf); esp_ota_abort(h);
            return fail(req, "400 Bad Request", esp_err_to_name(e));
        }
        left -= n;
    }
    free(buf);

    /* esp_ota_end validates the image (magic, checksum, chip). */
    if ((e = esp_ota_end(h)) != ESP_OK)
        return fail(req, "400 Bad Request", e == ESP_ERR_OTA_VALIDATE_FAILED ? "Not a valid firmware image" : esp_err_to_name(e));
    if ((e = esp_ota_set_boot_partition(part)) != ESP_OK)
        return fail(req, "500 Internal Server Error", esp_err_to_name(e));

    ESP_LOGW(TAG, "update written to %s, rebooting", part->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK, rebooting");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}
