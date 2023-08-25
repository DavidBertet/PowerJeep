#include "server.h"

#include "freertos/task.h"
#include <sys/param.h>
#include <sys/unistd.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include "esp_netif.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_ota_ops.h"

#include <esp_http_server.h>

static const char *TAG = "server";

#define SCRATCH_BUFSIZE  8192
// Max length a file path can have on storage
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)
// Max size of an individual file. Make sure this
// value is same as that set in upload_script.html */
#define MAX_FILE_SIZE   (200*1024) // 200 KB
#define MAX_FILE_SIZE_STR "200KB"

#define MAX_CLIENTS 4
static httpd_handle_t server = NULL;
static int clients_fd[MAX_CLIENTS];

static esp_ota_handle_t ota_handle;

#define MAX_CALLBACKS 4
static wsserver_receive_callback receive_callbacks[MAX_CALLBACKS];

struct file_server_data {
  // Base path of file storage
  char base_path[ESP_VFS_PATH_MAX + 1];

  // Buffer for temporary storage during file transfer
  char buffer[SCRATCH_BUFSIZE];
};

#define IS_FILE_EXT(filename, ext) \
  (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

// Handler to redirect to /
static esp_err_t index_html_get_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "307 Temporary Redirect");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

// Set HTTP content type from file extension
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename) {
  if (IS_FILE_EXT(filename, ".pdf")) {
    return httpd_resp_set_type(req, "application/pdf");
  } else if (IS_FILE_EXT(filename, ".html")) {
    return httpd_resp_set_type(req, "text/html");
  } else if (IS_FILE_EXT(filename, ".jpeg")) {
    return httpd_resp_set_type(req, "image/jpeg");
  } else if (IS_FILE_EXT(filename, ".ico")) {
    return httpd_resp_set_type(req, "image/x-icon");
  }
  // For any other type always set as plain text
  return httpd_resp_set_type(req, "text/plain");
}

// Copies the full path into destination buffer and returns
// pointer to path (skipping the preceding base path)
static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize) {
  const size_t base_pathlen = strlen(base_path);
  size_t pathlen = strlen(uri);

  const char *quest = strchr(uri, '?');
  if (quest) {
    pathlen = MIN(pathlen, quest - uri);
  }
  const char *hash = strchr(uri, '#');
  if (hash) {
    pathlen = MIN(pathlen, hash - uri);
  }

  if (base_pathlen + pathlen + 1 > destsize) {
    // Full path string won't fit into destination buffer
    return NULL;
  }

  // Construct full path (base + path)
  strcpy(dest, base_path);
  strlcpy(dest + base_pathlen, uri, pathlen + 1);

  // Return pointer to path, skipping the base
  return dest + base_pathlen;
}

// Delayed restarted by 1s
static esp_err_t restart_task(void *pvParameter) {
  vTaskDelay(1000 / portTICK_PERIOD_MS);

  esp_restart();
}

// Handler to download a file kept on the server
static esp_err_t download_get_handler(httpd_req_t *req) {
  ESP_LOGE(TAG, "Request received for %s", ((struct file_server_data *)req->user_ctx)->base_path);

  char filepath[FILE_PATH_MAX];
  FILE *fd = NULL;
  struct stat file_stat;

  const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                            req->uri, sizeof(filepath));
  if (!filename) {
    ESP_LOGE(TAG, "Filename is too long");
    // Respond with 500 Internal Server Error
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
    return ESP_FAIL;
  }

  // If name has trailing '/', respond with directory contents
  // if (filename[strlen(filename) - 1] == '/') {
  //     return http_resp_dir_html(req, filepath);
  // }

  if (strcmp(filename, "/") == 0) {
    strcpy(filepath, "/spiffs/index.html");
    filename = "/index.html";
  }

  if (stat(filepath, &file_stat) == -1) {
      // If file not present on SPIFFS check if URI corresponds to one of the hardcoded paths
      if (strcmp(filename, "/index.html") == 0) {
          return index_html_get_handler(req);
      }
      //  else if (strcmp(filename, "/favicon.ico") == 0) {
      //     return favicon_get_handler(req);
      // }
      ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
      // Respond with 404 Not Found
      // httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
      // return ESP_FAIL;
      strcpy(filepath, "/spiffs/index.html");
      filename = "/index.html";
  }

  fd = fopen(filepath, "r");
  if (!fd) {
    ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
    // Respond with 500 Internal Server Error
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Sending file : %s (%ld bytes)...", filename, file_stat.st_size);
  set_content_type_from_file(req, filename);

  // Retrieve the pointer to scratch buffer for temporary storage
  char *buffer = ((struct file_server_data *)req->user_ctx)->buffer;
  size_t chunksize;
  do {
    // Read file in chunks into the scratch buffer
    chunksize = fread(buffer, 1, SCRATCH_BUFSIZE, fd);

    if (chunksize > 0) {
      // Send the buffer contents as HTTP response chunk
      if (httpd_resp_send_chunk(req, buffer, chunksize) != ESP_OK) {
        fclose(fd);
        ESP_LOGE(TAG, "File sending failed!");
        // Abort sending file
        httpd_resp_sendstr_chunk(req, NULL);
        // Respond with 500 Internal Server Error
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
        return ESP_FAIL;
      }
    }

    // Keep looping till the whole file is sent
  } while (chunksize != 0);

  // Close file after sending complete
  fclose(fd);
  ESP_LOGI(TAG, "File sending complete");

  // Respond with an empty chunk to signal HTTP response completion
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

void broadcast_progress(int loaded, int total) {
  char *message;
  char *format = "{\"loaded\":\"%d\",\"total\":\"%d\"}";
  asprintf(&message, format, loaded, total);
  ESP_LOGI(TAG, "%s", message);
  broadcast_message(message);
}

static esp_err_t upload_ota_handler(httpd_req_t *req) {
  // Retrieve the pointer to scratch buffer for temporary storage
  char *buffer = ((struct file_server_data *)req->user_ctx)->buffer;
  int received;

  esp_err_t ret = esp_ota_begin(esp_ota_get_next_update_partition(NULL), OTA_SIZE_UNKNOWN, &ota_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to begin OTAs");
    // Respond with 500 Internal Server Error
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to begin OTA");
    return ESP_FAIL;
  }

  // Content length of the request gives the size of the file being uploaded
  int remaining = req->content_len;

  while (remaining > 0) {
    broadcast_progress(req->content_len - remaining, req->content_len);

    // Receive the file part by part into a buffer
    if ((received = httpd_req_recv(req, buffer, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) {
        // Retry if timeout occurred
        continue;
      }

      // In case of unrecoverable error, close and delete the unfinished file
      esp_ota_end(ota_handle);

      ESP_LOGE(TAG, "File reception failed!");
      // Respond with 500 Internal Server Error
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
      return ESP_FAIL;
    }

    // Write buffer content to file on storage
    if (received && esp_ota_write(ota_handle, buffer, received) != ESP_OK) {
      // Couldn't write everything to file!
      esp_ota_end(ota_handle);

      ESP_LOGE(TAG, "OTA write failed!");
      // Respond with 500 Internal Server Error
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to OTA");
      return ESP_FAIL;
    }

    // Keep track of remaining size of the file left to be uploaded
    remaining -= received;
  }

  broadcast_progress(req->content_len, req->content_len);

  // End OTA upon upload completion
  esp_ota_end(ota_handle);
  // Set new boot partition
  ret = esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Set new boot partition failed!");
    // Respond with 500 Internal Server Error
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set new boot partition");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "File reception complete");

  // Redirect onto root
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_sendstr(req, "File uploaded successfully");

  xTaskCreate(&restart_task, "restart_task", 2048, NULL, 10, NULL);  
  return ESP_OK;
}

static esp_err_t upload_file_handler(httpd_req_t *req, const char *filepath, const char *filename) {
  FILE *fd = NULL;

  // File cannot be larger than a limit
  if (req->content_len > MAX_FILE_SIZE) {
    ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
    // Respond with 400 Bad Request
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File size must be less than " MAX_FILE_SIZE_STR "!");
    return ESP_FAIL;
  }

  fd = fopen(filepath, "w");
  if (!fd) {
    ESP_LOGE(TAG, "Failed to create file : %s", filepath);
    // Respond with 500 Internal Server Error
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Receiving file : %s...", filename);

  // Retrieve the pointer to scratch buffer for temporary storage
  char *buffer = ((struct file_server_data *)req->user_ctx)->buffer;
  int received;

  // Content length of the request gives the size of the file being uploaded
  int remaining = req->content_len;

  while (remaining > 0) {
    broadcast_progress(req->content_len - remaining, req->content_len);

    // Receive the file part by part into a buffer
    if ((received = httpd_req_recv(req, buffer, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) {
        // Retry if timeout occurred
        continue;
      }

      // In case of unrecoverable error, close and delete the unfinished file
      fclose(fd);
      unlink(filepath);

      ESP_LOGE(TAG, "File reception failed!");
      // Respond with 500 Internal Server Error
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
      return ESP_FAIL;
    }

    // Write buffer content to file on storage
    if (received && (received != fwrite(buffer, 1, received, fd))) {
      // Couldn't write everything to file!
      // Storage may be full?
      fclose(fd);
      unlink(filepath);

      ESP_LOGE(TAG, "File write failed!");
      // Respond with 500 Internal Server Error
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage");
      return ESP_FAIL;
    }

    // Keep track of remaining size of the file left to be uploaded
    remaining -= received;
  }

  broadcast_progress(req->content_len, req->content_len);

  // Close file upon upload completion
  fclose(fd);

  ESP_LOGI(TAG, "File reception complete");

  // Redirect onto root
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_sendstr(req, "File uploaded successfully");
  return ESP_OK;
}

// Handler to upload a file onto the server
static esp_err_t upload_post_handler(httpd_req_t *req) {
  char filepath[FILE_PATH_MAX];

  // Skip leading "/upload" from URI to get filename
  // Note sizeof() counts NULL termination hence the -1
  const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                            req->uri + sizeof("/upload") - 1, sizeof(filepath));
  if (!filename) {
    // Respond with 500 Internal Server Error
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
    return ESP_FAIL;
  }

  // Filename cannot have a trailing '/'
  if (filename[strlen(filename) - 1] == '/') {
    ESP_LOGE(TAG, "Invalid filename : %s", filename);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
    return ESP_FAIL;
  }

  if (IS_FILE_EXT(filename, ".bin")) {
    return upload_ota_handler(req);
  } else {
    return upload_file_handler(req, filepath, filename);
  }
}

// *************************
// Websocket
// *************************

struct async_resp_arg {
  httpd_handle_t hd;
  int fd;
};

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

static void on_client_disconnected(httpd_handle_t hd, int sockfd) {
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
      on_client_disconnected(server, clients_fd[i]);
    }
  }

  return ESP_OK;
}

/*
 * This handler echos back the received ws data
 * and triggers an async send if certain message received
 */
esp_err_t echo_handler(httpd_req_t *req) {
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

// *************************
// Server
// *************************

static httpd_handle_t start_webserver(void) {
  static struct file_server_data *server_data = NULL;

  // Init clients
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    clients_fd[i] = -1;
  }

  server_data = calloc(1, sizeof(struct file_server_data));
  if (!server_data) {
    ESP_LOGE(TAG, "Failed to allocate memory for server data");
    return ESP_ERR_NO_MEM;
  }
  strlcpy(server_data->base_path, "/spiffs", sizeof(server_data->base_path));

  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  //config.open_fn = on_client_connected;
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

  // URI handler for websockets to server
  static const httpd_uri_t ws = {
    .uri        = "/ws",
    .method     = HTTP_GET,
    .handler    = echo_handler,
    .user_ctx   = NULL,
    .is_websocket = true
  };
  httpd_register_uri_handler(server, &ws);

  // URI handler for accessing files from server
  httpd_uri_t file_download = {
    .uri       = "/*",  // Match all URIs of type /path/to/file
    .method    = HTTP_GET,
    .handler   = download_get_handler,
    .user_ctx  = server_data    // Pass server data as context
  };
  httpd_register_uri_handler(server, &file_download);

  // URI handler for uploading files to server
  httpd_uri_t file_upload = {
    .uri       = "/upload/*",   // Match all URIs of type /upload/path/to/file
    .method    = HTTP_POST,
    .handler   = upload_post_handler,
    .user_ctx  = server_data    // Pass server data as context
  };
  httpd_register_uri_handler(server, &file_upload);
  
  return server;
}

static void disconnect_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data) {
  httpd_handle_t* server = (httpd_handle_t*) arg;
  if (*server) {
    ESP_LOGI(TAG, "Stopping webserver");
    httpd_stop(*server);
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

void setup_server(void) {
  // Register to WIFI events
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

  // Init callbacks
  for (int i = 0; i < MAX_CALLBACKS; ++i) {
    receive_callbacks[i] = NULL;
  }

  // Start the server
  server = start_webserver();
}
