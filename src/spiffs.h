#ifndef SPIFFS_H
#define SPIFFS_H

#include "esp_err.h"

esp_err_t setup_spiffs(void);

#define SPIFFS_BASE_PATH "/spiffs"
#define SPIFFS_MAX_FILES 10 // This decides the maximum number of files that can be created on the storage

#endif