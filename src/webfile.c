#include "webfile.h"

#include "freertos/task.h"
#include <sys/unistd.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"

#include "websocket.h"
#include "utils.h"
#include "spiffs.h"

// Local variables

static const char *TAG = "webfile";

#define SCRATCH_BUFSIZE  8192
// Max length a file path can have on storage
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)
// Max size of an individual file. Make sure this
// value is same as that set in upload_script.html */
#define MAX_FILE_SIZE   (200*1024) // 200 KB
#define MAX_FILE_SIZE_STR "200KB"

static esp_ota_handle_t ota_handle;

// Buffer for temporary storage during file transfer
static char scratch_buffer[SCRATCH_BUFSIZE];

// Implementations

#define IS_FILE_EXTENSION(filename, ext) \
  (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

// Set HTTP content type from file extension
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename) {
  if (IS_FILE_EXTENSION(filename, ".pdf")) {
    return httpd_resp_set_type(req, "application/pdf");
  } else if (IS_FILE_EXTENSION(filename, ".html")) {
    return httpd_resp_set_type(req, "text/html");
  } else if (IS_FILE_EXTENSION(filename, ".jpeg")) {
    return httpd_resp_set_type(req, "image/jpeg");
  } else if (IS_FILE_EXTENSION(filename, ".ico")) {
    return httpd_resp_set_type(req, "image/x-icon");
  } else if (IS_FILE_EXTENSION(filename, ".css")) {
    return httpd_resp_set_type(req, "text/css");
  }
  // For any other type always set as plain text
  return httpd_resp_set_type(req, "text/plain");
}

// Redirect to /
static esp_err_t redirect_root(httpd_req_t *req) {
  httpd_resp_set_status(req, "307 Temporary Redirect");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, NULL, 0);

  return ESP_OK;
}

// Copies the full path into destination buffer and returns
// pointer to path (skipping the preceding base path)
static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize) {
  const size_t base_pathlen = strlen(base_path);
  size_t pathlen = strlen(uri);

  const char *quest = strchr(uri, '?');
  if (quest) {
    pathlen = min(pathlen, quest - uri);
  }
  const char *hash = strchr(uri, '#');
  if (hash) {
    pathlen = min(pathlen, hash - uri);
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

// Delayed restart by 1s
static void restart_task(void *pvParameter) {
  vTaskDelay(1000 / portTICK_PERIOD_MS);

  esp_restart();
}

static void broadcast_upload_progress(int loaded, int total) {
  char *message;
  char *format = "{\"loaded\":\"%d\",\"total\":\"%d\"}";
  asprintf(&message, format, loaded, total);
  ESP_LOGI(TAG, "%s", message);
  broadcast_message(message);
}

// Handler to download a file from the server
static esp_err_t download_get_handler(httpd_req_t *req) {
  ESP_LOGE(TAG, "Request received for %s", req->uri);

  char filepath[FILE_PATH_MAX];
  FILE *fd = NULL;
  struct stat file_stat;

  const char *filename = get_path_from_uri(filepath, SPIFFS_BASE_PATH, req->uri, sizeof(filepath));
  if (!filename) {
    ESP_LOGE(TAG, "Filename is too long");
    // Respond with 500 Internal Server Error
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
    return ESP_FAIL;
  }

  if (strcmp(filename, "/") == 0 || strcmp(filename, "/hotspot-detect.html") == 0) {
    strcpy(filepath, "/spiffs/index.html");
    filename = "/index.html";
  }

  if (stat(filepath, &file_stat) == -1) {
      ESP_LOGE(TAG, "Failed to stat file: %s", filepath);
      // If file not present on SPIFFS, redirect to root
      return redirect_root(req);
  }

  fd = fopen(filepath, "r");
  if (!fd) {
    ESP_LOGE(TAG, "Failed to read existing file: %s", filepath);
    // Respond with 500 Internal Server Error
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Sending file: %s (%ld bytes)...", filename, file_stat.st_size);
  set_content_type_from_file(req, filename);

  size_t chunksize;
  do {
    // Read file in chunks into the scratch buffer
    chunksize = fread(scratch_buffer, 1, SCRATCH_BUFSIZE, fd);

    if (chunksize > 0) {
      // Send the buffer contents as HTTP response chunk
      if (httpd_resp_send_chunk(req, scratch_buffer, chunksize) != ESP_OK) {
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

// Handler to upload a new binary onto the chip
static esp_err_t upload_ota_handler(httpd_req_t *req) {
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
    broadcast_upload_progress(req->content_len - remaining, req->content_len);

    // Receive the file part by part into a buffer
    if ((received = httpd_req_recv(req, scratch_buffer, min(remaining, SCRATCH_BUFSIZE))) <= 0) {
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
    if (received && esp_ota_write(ota_handle, scratch_buffer, received) != ESP_OK) {
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

  broadcast_upload_progress(req->content_len, req->content_len);

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

// Handler to upload a file onto the filesystem
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

  int received;

  // Content length of the request gives the size of the file being uploaded
  int remaining = req->content_len;

  while (remaining > 0) {
    broadcast_upload_progress(req->content_len - remaining, req->content_len);

    // Receive the file part by part into a buffer
    if ((received = httpd_req_recv(req, scratch_buffer, min(remaining, SCRATCH_BUFSIZE))) <= 0) {
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
    if (received && (received != fwrite(scratch_buffer, 1, received, fd))) {
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

  broadcast_upload_progress(req->content_len, req->content_len);

  // Close file upon upload completion
  fclose(fd);

  ESP_LOGI(TAG, "File reception complete");

  // Redirect onto root
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_sendstr(req, "File uploaded successfully");
  return ESP_OK;
}

// Handler to upload a something onto the server
static esp_err_t upload_post_handler(httpd_req_t *req) {
  char filepath[FILE_PATH_MAX];

  // Skip leading "/upload" from URI to get filename
  // Note sizeof() counts NULL termination hence the -1
  const char *filename = get_path_from_uri(filepath, SPIFFS_BASE_PATH, req->uri + sizeof("/upload") - 1, sizeof(filepath));
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

  if (IS_FILE_EXTENSION(filename, ".bin")) {
    return upload_ota_handler(req);
  } else {
    return upload_file_handler(req, filepath, filename);
  }
}

void start_web_file(httpd_handle_t server) {
  ESP_LOGI(TAG, "Start web file");

  // URI handler for accessing files from server
  httpd_uri_t file_download = {
    .uri       = "/*",  // Match all URIs of type /path/to/file
    .method    = HTTP_GET,
    .handler   = download_get_handler,
    .user_ctx  = NULL
  };
  httpd_register_uri_handler(server, &file_download);

  // URI handler for uploading files to server
  httpd_uri_t file_upload = {
    .uri       = "/upload/*",   // Match all URIs of type /upload/path/to/file
    .method    = HTTP_POST,
    .handler   = upload_post_handler,
    .user_ctx  = NULL
  };
  httpd_register_uri_handler(server, &file_upload);
}
