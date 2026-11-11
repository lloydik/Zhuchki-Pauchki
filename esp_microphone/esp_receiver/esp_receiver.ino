// Приемник для Wi-Fi микрофона на ESP32 + MAX98357A
// Принимает звук по ESP-NOW (без Wi-Fi сети) и играет его на усилителе.
//
// Подключение MAX98357A:
// VIN -> 5V, GND -> GND
// BCLK -> 26, LRC -> 25, DIN -> 22
// динамик -> + и - на усилителе

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ESP_I2S.h>

// канал эфира, должен совпадать с микрофоном
#define CHANNEL 1

// отсчетов в пакете (столько же, сколько шлет микрофон)
#define SAMPLES 120

// пины усилителя
#define AMP_BCLK 26
#define AMP_LRC 25
#define AMP_DIN 22

// Дебаг плоттера
bool showLevel = true;

I2SClass i2s;

int16_t stereo[SAMPLES * 2];  // звук в оба канала для усилителя

// сюда ESP-NOW отдает каждый пришедший пакет со звуком
void onSound(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != SAMPLES * 2) {
    return;
  }
  int16_t* sound = (int16_t*)data;

  // ищем самый громкий отсчет 
  if (showLevel) {
    int level = 0;
    for (int i = 0; i < SAMPLES; i++) {
      if (abs(sound[i]) > level) level = abs(sound[i]);
    }
    Serial.println(level);
  }

  for (int i = 0; i < SAMPLES; i++) {
    stereo[i * 2] = sound[i];
    stereo[i * 2 + 1] = sound[i];
  }
  i2s.write((uint8_t*)stereo, sizeof(stereo));
}

void setup() {
  Serial.begin(115200);

  // запускаем усилитель: 16 кГц, стерео
  i2s.setPins(AMP_BCLK, AMP_LRC, AMP_DIN);
  i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

  // включаем ESP-NOW
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_now_init();
  esp_now_register_recv_cb(onSound);

  Serial.println("Приемник готов, жду звук");
}

void loop() {
  delay(1000);
}
