#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "storage.h"

nvs_handle_t storage;

void setup_storage(void) {
  // Init NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  
  ret = nvs_open("storage", NVS_READWRITE, &storage);
  if (ret != ESP_OK) {
    printf("Error (%s) opening NVS handle!\n", esp_err_to_name(ret));
  }
}

esp_err_t readFloat(char* key, float *value, float defaultValue) {
  *value = defaultValue;
  size_t required_size = 4;
  return nvs_get_blob(storage, key, value, &required_size);
}

esp_err_t writeFloat(char* key, float value) {
  ESP_LOGI("Storage", "Store value %f for key %s", value, key);
  esp_err_t ret = nvs_set_blob(storage, key, &value, sizeof(float));
  if (ret != ESP_OK) return ret;

  return nvs_commit(storage);
}
