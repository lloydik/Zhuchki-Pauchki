// Wi-Fi микрофон на ESP32 + INMP441
// Шлет звук по ESP-NOW. Это не обычный Wi-Fi: нет сети и имени,
// платы говорят напрямую по эфиру, в списке сетей ничего не видно.
//
// Подключение INMP441:
// VDD -> 3V3, GND -> GND, L/R -> GND
// SCK -> 14, WS -> 15, SD -> 32

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ESP_I2S.h>

// канал эфира, должен совпадать с приемником
#define CHANNEL 1

// отсчетов в пакете (120 * 2 байта = 240, больше в ESP-NOW не влезает)
#define SAMPLES 120

// пины микрофона
#define MIC_SCK 14
#define MIC_WS 15
#define MIC_SD 32

uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

I2SClass i2s;

int32_t micData[SAMPLES];  // данные с микрофона
int16_t sound[SAMPLES];    // звук, который отправляем

void setup() {
  Serial.begin(115200);

  // запускаем микрофон: 16 кГц, моно
  i2s.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);

  // включаем ESP-NOW
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_now_init();

  // добавляем приемник
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcast, 6);
  peer.channel = CHANNEL;
  esp_now_add_peer(&peer);
}

void loop() {
  i2s.readBytes((char*)micData, sizeof(micData));

  // микрофон дает 32 бита, переводим в 16 бит
  for (int i = 0; i < SAMPLES; i++) {
    int32_t s = micData[i] >> 14;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    sound[i] = s;
  }

  esp_now_send(broadcast, (uint8_t*)sound, sizeof(sound));
}
