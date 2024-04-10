#include "websocket.h"

#include "freertos/task.h"
#include <sys/unistd.h>
#include <esp_log.h>
#include <esp_http_server.h>

// Local variables

static const char *TAG = "websocket";

#define MAX_CLIENTS 4
static int clients_fd[MAX_CLIENTS];

#define MAX_CALLBACKS 4
static wsserver_receive_callback receive_callbacks[MAX_CALLBACKS];

static httpd_handle_t server = NULL;

// Implementations

struct async_resp_arg {
  httpd_handle_t hd;
  int fd;
};

// Manage clients

static esp_err_t on_client_connected(httpd_handle_t hd, int sockfd) {
  ESP_LOGI(TAG, "WS Client Connected %i", sockfd);
  int available_index = -1;

  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (clients_fd[i] == sockfd) {
      return ESP_FAIL;
    }
    if (available_index == -1 && clients_fd[i] == -1) {
      available_index = i;
    }
  }
  
  if (available_index == -1) {
    ESP_LOGI(TAG, "No more space available for client %i", sockfd);
    return ESP_FAIL;
  }

  clients_fd[available_index] = sockfd;

  return ESP_OK;
}

void on_ws_client_disconnected(int sockfd) {
  ESP_LOGI(TAG, "WS Client Disconnected %i", sockfd);

  close(sockfd);

  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (clients_fd[i] == sockfd) {
      clients_fd[i] = -1;
      return;
    }
  }

  return;
}

// Manage messages

esp_err_t broadcast_message(char* msg) {
  esp_err_t ret;

  if (server == NULL) {
    ESP_LOGE(TAG, "Tried to broadcast a message while server down");
    return ESP_FAIL;
  }

  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (clients_fd[i] == -1) {
      continue;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)msg;
    ws_pkt.len = strlen(msg);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    ESP_LOGI(TAG, "Send message to %i", clients_fd[i]);
    ret = httpd_ws_send_frame_async(server, clients_fd[i], &ws_pkt);
    if (ret != ESP_OK) {
      on_ws_client_disconnected(clients_fd[i]);
    }
  }

  return ESP_OK;
}

// Handle received messages and forward to listener meaningful messages
esp_err_t receive_message(httpd_req_t *req) {
  // Check for handshake
  if (req->method == HTTP_GET) {
    on_client_connected(server, httpd_req_to_sockfd(req));
    ESP_LOGI(TAG, "Handshake done, the new connection was opened");
    return ESP_OK;
  }

  httpd_ws_frame_t ws_pkt;
  uint8_t *buffer = NULL;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;

  // Set max_len = 0 to get the frame len
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
    return ret;
  }
  ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);

  if (ws_pkt.len) {
    // ws_pkt.len + 1 is for NULL termination as we are expecting a string
    buffer = calloc(1, ws_pkt.len + 1);
    if (buffer == NULL) {
      ESP_LOGE(TAG, "Failed to calloc memory for buffer");
      return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buffer;

    // Set max_len = ws_pkt.len to get the frame payload
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
      free(buffer);
      return ret;
    }
    
    // Broacast received message
    for (int i = 0; i < MAX_CALLBACKS; ++i) {
      if (receive_callbacks[i] != NULL) {
        receive_callbacks[i](&ws_pkt);
      }
    }
  }

  ESP_LOGI(TAG, "Packet type: %d", ws_pkt.type);

  return ret;
}

// Manage consumer callbacks

void register_callback(wsserver_receive_callback callback) {
  int available_index = -1;

  for (int i = 0; i < MAX_CALLBACKS; ++i) {
    if (receive_callbacks[i] == callback) {
      return;
    }
    if (receive_callbacks[i] == NULL) {
      available_index = i;
    }
  }

  if (available_index != -1) {
    ESP_LOGI(TAG, "Register callback in the first available place");
    receive_callbacks[available_index] = callback;
  } else {
    ESP_LOGI(TAG, "Register callback has no available place");
  }
}

void unregister_callback(wsserver_receive_callback callback) {
  for (int i = 0; i < MAX_CALLBACKS; ++i) {
    if (receive_callbacks[i] == callback) {
      receive_callbacks[i] = NULL;
    }
  }
}

// Websocket lifecycle

void start_websocket(httpd_handle_t new_server) {
  ESP_LOGI(TAG, "Start websocket");

  server = new_server;

  // Init clients
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    clients_fd[i] = -1;
  }

  // Init callbacks
  for (int i = 0; i < MAX_CALLBACKS; ++i) {
    receive_callbacks[i] = NULL;
  }

  // URI handler for websockets to server
  static const httpd_uri_t ws = {
    .uri        = "/ws",
    .method     = HTTP_GET,
    .handler    = receive_message,
    .user_ctx   = NULL,
    .is_websocket = true
  };
  httpd_register_uri_handler(server, &ws);
}

void stop_websocket(void) {
  server = NULL;
}