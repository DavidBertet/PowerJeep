#include "webserver.h"

#include "freertos/task.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include "esp_netif.h"
#include <esp_http_server.h>

#include "websocket.h"
#include "webfile.h"

// Local variables

static const char *TAG = "webserver";

static httpd_handle_t server = NULL;

// Implementation

static void on_client_disconnected(httpd_handle_t hd, int sockfd) {
  on_ws_client_disconnected(sockfd);
}

static httpd_handle_t start_webserver(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  config.close_fn = on_client_disconnected;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.lru_purge_enable = true;

  ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
  esp_err_t ret = httpd_start(&server, &config);
  if (ret != ESP_OK) {
    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
  }
  // Set URI handlers
  ESP_LOGI(TAG, "Registering URI handlers");
  
  start_websocket(server);
  start_web_file(server);

  return server;
}

static void disconnect_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data) {
  httpd_handle_t* server = (httpd_handle_t*) arg;
  if (*server) {
    ESP_LOGI(TAG, "Stopping webserver");

    httpd_stop(*server);

    stop_websocket();

    *server = NULL;
  }
}

static void connect_handler(void* arg, esp_event_base_t event_base, 
                            int32_t event_id, void* event_data) {
  httpd_handle_t* server = (httpd_handle_t*) arg;
  if (*server == NULL) {
    ESP_LOGI(TAG, "Starting webserver");

    *server = start_webserver();
  }
}

void setup_server(void) {
  // Register to WIFI events
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

  // Start the server
  server = start_webserver();
}
