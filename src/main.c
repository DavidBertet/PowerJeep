#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_event.h>
#include "esp_netif.h"

#include "storage.h"
#include "captdns.h"
#include "power_wheel.h"
#include "wifi.h"
#include "webserver.h"
#include "spiffs.h"

static const char *TAG = "main";

void app_main() {
  // To disable all logs, use ESP_LOG_NONE
  esp_log_level_set("*", ESP_LOG_DEBUG);

  ESP_LOGI(TAG, "Start hello!!");

  ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

  // Init NVS storage
  setup_storage();

  // Init TCP/IP stack
  ESP_ERROR_CHECK(esp_netif_init());
  // Init event mechanism
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Init file storage
  ESP_ERROR_CHECK(setup_spiffs());

  // Setup captive portal - automatically opens the page when we connect to the wifi
  setup_captive_dns();

  // Setup wifi access point
  setup_softap();

  // Setup HTTP server
  setup_server();

  // Setup driving
  setup_driving();
}
