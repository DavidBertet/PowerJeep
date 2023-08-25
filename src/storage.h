#ifndef STORAGE_H
#define STORAGE_H

void setup_storage(void);

esp_err_t readFloat(char* key, float *value, float defaultValue);
esp_err_t writeFloat(char* key, float value);

#endif