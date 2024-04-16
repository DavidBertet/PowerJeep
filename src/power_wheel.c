#include "power_wheel.h"

#include <sys/param.h>
#include <math.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "websocket.h"
#include "cJSON.h"
#include "storage.h"
#include "utils.h"

static const char *TAG = "drive";

static void drive_task(void *pvParameter);
static void broadcast_speed_task(void *pvParameter);
static void led_task(void *pvParameter);

// ADC throttle capability

#define WITH_ADC_THROTTLE 0

#if WITH_ADC_THROTTLE
#include "driver/adc.h"
#include "esp_adc_cal.h"

// voltage is between MIN_THROTTLE_VALUEv and MAX_THROTTLE_VALUEv
// retrieve those values with setup.html page
#define MIN_THROTTLE_VALUE 1.0f // v  -- add a small margin (like +0.15) so the car doesn't move when it shouldn't!
#define MAX_THROTTLE_VALUE 2.6f // v
#define THROTTLE_RANGE ((MAX_THROTTLE_VALUE - MIN_THROTTLE_VALUE) * 10.0f)

#endif

// PIN

// - Inputs
#if WITH_ADC_THROTTLE
#define GAS_PEDAL_FORWARD_PIN ADC1_CHANNEL_4 // GPIO 32
#define GAS_PEDAL_BACKWARD_PIN ADC1_CHANNEL_5 // GPIO 33
#else
#define GAS_PEDAL_FORWARD_PIN GPIO_NUM_32
#define GAS_PEDAL_BACKWARD_PIN GPIO_NUM_33
#endif
// - Outputs
#define FORWARD_PWM_PIN GPIO_NUM_18
#define BACKWARD_PWM_PIN GPIO_NUM_19
#define STATUS_LED_PIN GPIO_NUM_2

// Constants

#define FORWARD_SHUTOFF_THRESOLD 15 // %
#define BACKWARD_SHUTOFF_THRESOLD 10 // %

// - With a 18v battery, 66% is equivalent to a 12v
#define DEFAULT_FORWARD_MAX_SPEED 60 // %
#define DEFAULT_BACKWARD_MAX_SPEED 35 // %

#define SPEED_INCREMENT 0.5f // % of increment per loop

#define MOTOR_PWM_CHANNEL_FORWARD LEDC_CHANNEL_1
#define MOTOR_PWM_CHANNEL_BACKWARD LEDC_CHANNEL_2
#define MOTOR_PWM_TIMER LEDC_TIMER_1
#define MOTOR_PWM_DUTY_RESOLUTION LEDC_TIMER_10_BIT

// Variables in memory

float current_speed = 0;
float emergency_stop = false;
float max_forward = 0;
float max_backward = 0;
int led_sleep_delay = 20;

#if WITH_ADC_THROTTLE
static esp_adc_cal_characteristics_t adc1_chars;
bool adc_calibration_enabled = false;

uint32_t get_adc_throttle_value(uint8_t gpio) {
  uint32_t adc_average = 0;
  uint32_t adc_current = 0;

  for (int i = 0; i < 5; ++i) {
    esp_adc_cal_get_voltage(gpio, &adc1_chars, &adc_current);
    adc_average += adc_current;
  }

  return adc_average / 5;
}
#endif

// ***************
// **** WEBSOCKETS
// ***************

// Broadcast all values
// {
//   "current_speed": 12,
//   "max_forward": 66,
//   "max_backward": 50,
//   "emergency_stop": false
//}
void broadcast_all_values() {
  char *message;
  char *format = "{\"current_speed\":%f,\"max_forward\":%f,\"max_backward\":%f,\"emergency_stop\":%s}";
  asprintf(&message, format, current_speed, max_forward, max_backward, emergency_stop ? "true" : "false");
  ESP_LOGI(TAG, "Send %s", message);
  broadcast_message(message);
  free(message);
}

// Broadcast only the current speed value - can be negative if going backward
// {
//   "current_speed": 12
// }
void broadcast_current_speed() {
  char *message;
  asprintf(&message, "{\"current_speed\":%f}", current_speed);
  ESP_LOGI(TAG, "Send %s", message);
  broadcast_message(message);
  free(message);
}

void broadcast_current_throttle() {
  #if WITH_ADC_THROTTLE
  float current_throttle = get_adc_throttle_value(GAS_PEDAL_FORWARD_PIN) / 1000.0f;
  #else
  float current_throttle = !gpio_get_level(GAS_PEDAL_FORWARD_PIN) ? 1.0f : 0.0f;
  #endif

  char *message;
  asprintf(&message, "{\"current_throttle\":%f}", current_throttle);
  ESP_LOGI(TAG, "Send %s", message);
  broadcast_message(message);
  free(message);
}

// Manage commands from web sockets
// - Update max values
// { "command": "update_max", "parameters": { "max_forward": double, "max_backward": double } }
// - Read all values
// { "command": "read" }
// - Enable/Disable emergency stop
// { "command": "emergency_stop", "parameters": { "is_enabled": bool } }
static void data_received(httpd_ws_frame_t* ws_pkt) {
  ESP_LOGI(TAG, "Received packet with message: %s", ws_pkt->payload);

  cJSON *root = cJSON_Parse((char*)ws_pkt->payload);
  char* command = cJSON_GetObjectItem(root, "command")->valuestring;
  ESP_LOGI(TAG, "Command: %s", command);
  if (strcmp("update_max", command) == 0) {
    cJSON* parameters = cJSON_GetObjectItem(root, "parameters");
    if (parameters == NULL) {
      goto end;
    }

    cJSON *max_forward_node = cJSON_GetObjectItem(parameters, "max_forward");
    cJSON *max_backward_node = cJSON_GetObjectItem(parameters, "max_backward");
    if (!cJSON_IsNumber(max_forward_node) || !cJSON_IsNumber(max_backward_node)) {
      goto end;
    }
    // Set values in memory for immediate use
    max_forward = max_forward_node->valuedouble;
    max_backward = max_backward_node->valuedouble;

    // Save values in storage to survive restarts
    writeFloat("max_forward", max_forward);
    writeFloat("max_backward", max_backward);

    // Broadcast new values to all listeners
    broadcast_all_values();
  } else if (strcmp("read", command) == 0) {
    broadcast_all_values();
  } else if (strcmp("read_throttle", command) == 0) {
    broadcast_current_throttle();
  } else if (strcmp("emergency_stop", command) == 0) {
    cJSON* parameters = cJSON_GetObjectItem(root, "parameters");
    if (parameters == NULL) {
      goto end;
    }
    cJSON *is_enabled = cJSON_GetObjectItem(parameters, "is_enabled");
    if (!cJSON_IsBool(is_enabled)) {
      return;
    }
    // Set values in memory for immediate use, it doesn't survive restarts
    emergency_stop = cJSON_IsTrue(is_enabled);

    // Broadcast new values to all listeners
    broadcast_all_values();

end:
    cJSON_Delete(root);
  }
}

// **********
// **** SETUP
// **********

#if WITH_ADC_THROTTLE
static bool adc_calibration_init(void) {
  esp_err_t ret;
  bool adc_calibration_enabled = false;

  ret = esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF);
  if (ret == ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG, "Calibration scheme not supported, skip software calibration");
  } else if (ret == ESP_ERR_INVALID_VERSION) {
    ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
  } else if (ret == ESP_OK) {
    adc_calibration_enabled = true;
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_DEFAULT, 0, &adc1_chars);
  } else {
    ESP_LOGE(TAG, "Invalid arg");
  }

  return adc_calibration_enabled;
}
#endif

// Setup pin on the board
void setup_pin() {
  // Throttle
  #if WITH_ADC_THROTTLE
  adc_calibration_enabled = adc_calibration_init();
  ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_DEFAULT));
  ESP_ERROR_CHECK(adc1_config_channel_atten(GAS_PEDAL_FORWARD_PIN, ADC_ATTEN_DB_11));
  ESP_ERROR_CHECK(adc1_config_channel_atten(GAS_PEDAL_BACKWARD_PIN, ADC_ATTEN_DB_11));
  #else
  gpio_reset_pin(GAS_PEDAL_FORWARD_PIN);
  gpio_set_direction(GAS_PEDAL_FORWARD_PIN, GPIO_MODE_INPUT);
  gpio_pullup_en(GAS_PEDAL_FORWARD_PIN);

  gpio_reset_pin(GAS_PEDAL_BACKWARD_PIN);
  gpio_set_direction(GAS_PEDAL_BACKWARD_PIN, GPIO_MODE_INPUT);
  gpio_pullup_en(GAS_PEDAL_BACKWARD_PIN);
  #endif

  // Motor driver
  gpio_reset_pin(FORWARD_PWM_PIN);
  gpio_set_direction(FORWARD_PWM_PIN, GPIO_MODE_OUTPUT);

  gpio_reset_pin(BACKWARD_PWM_PIN);
  gpio_set_direction(BACKWARD_PWM_PIN, GPIO_MODE_OUTPUT);

  gpio_reset_pin(STATUS_LED_PIN);  
  gpio_set_direction(STATUS_LED_PIN, GPIO_MODE_OUTPUT);
}

// Setup LED channel to be used to generate PWM
void setup_pwm() {
  ledc_channel_config_t ledc_channel_forward = {0}, ledc_channel_backward = {0};

  ledc_channel_forward.gpio_num = FORWARD_PWM_PIN;
  ledc_channel_forward.speed_mode = LEDC_HIGH_SPEED_MODE;
  ledc_channel_forward.channel = MOTOR_PWM_CHANNEL_FORWARD;
  ledc_channel_forward.intr_type = LEDC_INTR_DISABLE;
  ledc_channel_forward.timer_sel = MOTOR_PWM_TIMER;
  ledc_channel_forward.duty = 0;

  ledc_channel_backward.gpio_num = BACKWARD_PWM_PIN;
  ledc_channel_backward.speed_mode = LEDC_HIGH_SPEED_MODE;
  ledc_channel_backward.channel = MOTOR_PWM_CHANNEL_BACKWARD;
  ledc_channel_backward.intr_type = LEDC_INTR_DISABLE;
  ledc_channel_backward.timer_sel = MOTOR_PWM_TIMER;
  ledc_channel_backward.duty = 0;

  ledc_timer_config_t ledc_timer = {0};
  ledc_timer.speed_mode = LEDC_HIGH_SPEED_MODE;
  ledc_timer.duty_resolution = MOTOR_PWM_DUTY_RESOLUTION;
  ledc_timer.timer_num = MOTOR_PWM_TIMER;
  ledc_timer.freq_hz = 25000;

  ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_forward));
	ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_backward));
	ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
}

void setup_driving(void) {
  // Retrieve max values from storage
  readFloat("max_forward", &max_forward, DEFAULT_FORWARD_MAX_SPEED);
  readFloat("max_backward", &max_backward, DEFAULT_BACKWARD_MAX_SPEED);

  // Setup pins
  setup_pin();

  // Setup PWM
  setup_pwm();

  // Listen to Websocket events
  register_callback(data_received);

  // Create a task with the higher priority for the driving task
  xTaskCreate(&drive_task, "drive_task", 2048, NULL, 20, NULL);

  // Create a task with a lower priority to broadcast the current speed
  xTaskCreate(&broadcast_speed_task, "broadcast_speed_task", 2048, NULL, 5, NULL);

  // Create a task for the led
  xTaskCreate(&led_task, "led_task", 2048, NULL, 10, NULL);
}

// **********
// **** LOGIC
// **********

// Speed is a percentage between -100 and 100 (backward and forward)
void send_values_to_motor(int speed) {
  float forward_duty_fraction = 0;
  float backward_duty_fraction = 0;

  if (speed > 100 || speed < -100) {
    return;
  }

  if (speed > 0) {
    forward_duty_fraction = speed / 100.0f;
  } else if (speed < 0) {
    backward_duty_fraction = speed / 100.0f;
  }

  uint32_t max_duty = (1 << MOTOR_PWM_DUTY_RESOLUTION) - 1;
  uint32_t forward_duty = lroundf(forward_duty_fraction * (float)max_duty);
  uint32_t backward_duty = -1 * lroundf(backward_duty_fraction * (float)max_duty);

  ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, MOTOR_PWM_CHANNEL_FORWARD, forward_duty));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, MOTOR_PWM_CHANNEL_FORWARD));

  ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, MOTOR_PWM_CHANNEL_BACKWARD, backward_duty));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, MOTOR_PWM_CHANNEL_BACKWARD));
}

// Return the targeted speed based on the pedal status.
// It is a percentage between -100 and 100 (backward and forward)
int get_speed_target(uint8_t forward_position, uint8_t backward_position) {
  if ((!forward_position && !backward_position) ||
      (forward_position && backward_position)) {
    return 0;
  }
  
  if (forward_position) {
    return min(max_forward, max_forward * (forward_position / 100.0f));
  } 

  // Backward is negative values
  return max(-max_backward, -max_backward * (backward_position / 100.0f));
}

uint8_t get_throttle_position(uint8_t gpio) {
  #if WITH_ADC_THROTTLE
  uint32_t adc_average = get_adc_throttle_value(gpio);

  return min(100, max(0, (int8_t)((adc_average - 1000) / THROTTLE_RANGE)));
  #else
  return !gpio_get_level(gpio) ? 100 : 0;
  #endif
}

// Blink the led to indicate an emergency stop
void blink_led_emergency_stop() {
  led_sleep_delay = 200;
}

// Blink the led to indicate the car is running
void blink_led_running(int speed) {
  led_sleep_delay = speed == 0 ? 1000 : (1.0f - fabsf(speed / 100.0f)) * 160 + 20 /*ms mini*/;
}

// Calculate next step for a smooth transition from current speed to targeted speed
float compute_next_speed(float current, float target, float delta) {    
  if (current < target) {
    // Slow down backward or speed up forward

    if (current < 0 && current > -BACKWARD_SHUTOFF_THRESOLD) {
      // Between -BACKWARD_SHUTOFF_THRESOLD < current < 0, we stop the car
      return 0;
    } else if (current > 0 && current < FORWARD_SHUTOFF_THRESOLD) {
      // Between 0 < current < FORWARD_SHUTOFF_THRESOLD, we set the car to FORWARD_SHUTOFF_THRESOLD
      return FORWARD_SHUTOFF_THRESOLD;
    } else if (current < 0) {
      // Slow down more aggressively if the car is moving quicker than 50%
      float slowdown_rate = current > 50 ? 0.08 : 0.04;
      // Safety! Slowing down backward, we must stop the car within a time frame
      return current + delta * slowdown_rate;
    } else {
      // Else we update speed incrementaly
      return current + SPEED_INCREMENT;
    }

    // Check if we went too far
    if (current > target) return target;
  } else if (current > target) {
    // Slow down forward or speed up backward

    if (current > 0 && current < FORWARD_SHUTOFF_THRESOLD) {
      // Between 0 < current < FORWARD_SHUTOFF_THRESOLD, we stop the car
      return 0;
    } else if (current < 0 && current > -BACKWARD_SHUTOFF_THRESOLD) {
      // Between -BACKWARD_SHUTOFF_THRESOLD < current < 0, we set the car to -BACKWARD_SHUTOFF_THRESOLD
      return -BACKWARD_SHUTOFF_THRESOLD;
    } else if (current > 0) {
      // Slow down more aggressively if the car is moving quicker than 50%
      float slowdown_rate = current > 50 ? 0.08 : 0.04;
      // Safety! Slowing down forward, we must stop the car within a time frame
      return current - delta * slowdown_rate;
    } else {
      // Else we update speed incrementaly
      return current - SPEED_INCREMENT;
    }

    // Check if we went too far
    if (current < target) return target;
  }

  return current;
}

// **********
// **** TASKS
// **********

// Task to broadcast new speed value to websocket listeners
static void broadcast_speed_task(void *pvParameter) {
  float previous_speed_broacasted = -1.0f;

  while (true) {
    if (!emergency_stop && current_speed != previous_speed_broacasted) {
      broadcast_current_speed();
    }

    previous_speed_broacasted = current_speed;

    vTaskDelay(250 / portTICK_PERIOD_MS);
  }
}

// Task that drives the car
static void drive_task(void *pvParameter) {
  int64_t last_update = esp_timer_get_time();
  float delta;

  int forward_position = 0;
  int backward_position = 0;

  int target = 0;

  while (true) {
    // Manage emergency stop
    if (emergency_stop) {
      current_speed = 0;

      send_values_to_motor(current_speed);

      last_update = esp_timer_get_time();

      blink_led_emergency_stop();

      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    // Update pedal & direction status
    forward_position = get_throttle_position(GAS_PEDAL_FORWARD_PIN);
    backward_position = get_throttle_position(GAS_PEDAL_BACKWARD_PIN);

    // Update targeted speed accordingly
    target = get_speed_target(forward_position, backward_position);

    // Take into account a loop could take more than expected
    // This is used to slow down within a fixed timeframe, regardless of the loop duration
    delta = (esp_timer_get_time() - last_update) / 1000;

    // Compute next speed based on current speed and targeted speed
    current_speed = compute_next_speed(current_speed, target, delta);

    // Send value to the motor
    send_values_to_motor(current_speed);

    last_update = esp_timer_get_time();

    // Blink embedded led to have some visible status of the speed
    blink_led_running(current_speed);

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// Blink the board led to indicate what the car is doing, or at least should be doing
static void led_task(void *pvParameter) {
  while (true) {
    gpio_set_level(STATUS_LED_PIN, 0);
    vTaskDelay(led_sleep_delay / portTICK_PERIOD_MS);
    gpio_set_level(STATUS_LED_PIN, 1);
    vTaskDelay(led_sleep_delay / portTICK_PERIOD_MS);
  }
}
