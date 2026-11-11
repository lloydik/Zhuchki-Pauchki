#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

// D5=GPIO14  D6=GPIO12  D7=GPIO13
#define HC12_RX_PIN  14
#define HC12_TX_PIN  12
#define HC12_SET_PIN 13
#define LED_PIN       2

const char* MESSAGE = "Hello from ESP8266";
const char* AP_SSID = "ESP-Radio";
const char* AP_PASS = "radiopass";

#define WPM       12
#define DOT_MS    (1200 / WPM)
#define DASH_MS   (3 * DOT_MS)
#define SYM_GAP   DOT_MS
#define LET_GAP   (3 * DOT_MS)
#define WORD_GAP  (7 * DOT_MS)

static const char* MORSE[26] = {
  ".-",    "-...",  "-.-.",  "-..",   ".",     "..-.",
  "--.",   "....",  "..",    ".---",  "-.-",   ".-..",
  "--",    "-.",    "---",   ".--.",  "--.-",  ".-.",
  "...",   "-",     "..-",   "...-",  ".--",   "-..-",
  "-.--",  "--.."
};

SoftwareSerial hc12(HC12_RX_PIN, HC12_TX_PIN);
ESP8266WebServer server(80);

int currentChannel = 1;

float channelToFreq(int ch) {
  return 433.4f + (ch - 1) * 0.4f;
}

int freqToChannel(float freq) {
  int ch = (int)round((freq - 433.4f) / 0.4f) + 1;
  if (ch < 1)   ch = 1;
  if (ch > 100) ch = 100;
  return ch;
}

void saveChannel(int ch) {
  EEPROM.put(0, ch);
  EEPROM.commit();
}

int loadChannel() {
  int ch;
  EEPROM.get(0, ch);
  if (ch < 1 || ch > 100) ch = 1;
  return ch;
}

void applyChannel(int ch) {
  if (ch < 1)   ch = 1;
  if (ch > 100) ch = 100;

  char cmd[16];
  snprintf(cmd, sizeof(cmd), "AT+C%03d\r\n", ch);

  digitalWrite(HC12_SET_PIN, LOW);
  delay(100);
  while (hc12.available()) hc12.read();
  hc12.print(cmd);
  delay(200);
  digitalWrite(HC12_SET_PIN, HIGH);
  delay(300);

  currentChannel = ch;
  saveChannel(ch);
}

void configureHC12() {
  digitalWrite(HC12_SET_PIN, LOW);
  delay(100);
  while (hc12.available()) hc12.read();

  hc12.print("AT+B9600\r\n"); delay(150);
  hc12.print("AT+P1\r\n");    delay(150);
  hc12.print("AT+FU3\r\n");   delay(150);

  char cmd[16];
  snprintf(cmd, sizeof(cmd), "AT+C%03d\r\n", currentChannel);
  hc12.print(cmd);
  delay(150);

  digitalWrite(HC12_SET_PIN, HIGH);
  delay(300);
}

void handleRoot() {
  float freq = channelToFreq(currentChannel);

  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP Radio</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 16px;background:#f0f2f5}"
    "h1{margin-bottom:4px}"
    ".card{background:#fff;border-radius:8px;padding:20px;margin-top:16px;box-shadow:0 1px 4px #0002}"
    ".freq{font-size:2em;font-weight:bold;color:#1a73e8}"
    ".sub{color:#777;font-size:.85em;margin-top:4px}"
    "label{display:block;margin-top:14px;font-size:.9em;color:#555}"
    "input[type=number]{width:100%;padding:9px;font-size:1em;border:1px solid #ccc;"
      "border-radius:4px;box-sizing:border-box;margin-top:4px}"
    "input[type=range]{width:100%;margin-top:8px;accent-color:#1a73e8}"
    "button{margin-top:18px;width:100%;padding:11px;font-size:1em;background:#1a73e8;"
      "color:#fff;border:none;border-radius:4px;cursor:pointer}"
    "button:active{background:#1558b0}"
    "</style></head><body>"
    "<h1>ESP Radio</h1>"
    "<div class='card'>"
      "<div>Текущая частота</div>"
      "<div class='freq'>" + String(freq, 1) + " МГц</div>"
      "<div class='sub'>Канал " + String(currentChannel) + " из 100 &nbsp;|&nbsp; диапазон 433.4–473.0 МГц</div>"
    "</div>"
    "<div class='card'>"
      "<form method='POST' action='/set'>"
        "<label>Частота (МГц)"
          "<input type='number' id='fq' name='freq'"
            " min='433.4' max='473.0' step='0.4' value='" + String(freq, 1) + "'>"
        "</label>"
        "<label>Канал: <span id='chval'>" + String(currentChannel) + "</span>"
          "<input type='range' id='ch' min='1' max='100' value='" + String(currentChannel) + "'"
            " oninput=\"var f=(433.4+(this.value-1)*0.4).toFixed(1);"
              "document.getElementById('fq').value=f;"
              "document.getElementById('chval').textContent=this.value\">"
        "</label>"
        "<button type='submit'>Применить</button>"
      "</form>"
    "</div>"
    "</body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleSet() {
  if (server.hasArg("freq")) {
    float freq = server.arg("freq").toFloat();
    applyChannel(freqToChannel(freq));
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void keyDown(int ms) {
  digitalWrite(LED_PIN, LOW);  // active LOW
  unsigned long end = millis() + (unsigned long)ms;
  while ((long)(end - millis()) > 0) {
    hc12.write(0xEE);
    yield();  // кормим watchdog, иначе ESP8266 ресетится в tight loop
  }
  hc12.flush();
}

void keyUp(int ms) {
  digitalWrite(LED_PIN, HIGH);
  delay(ms);
}

void sendSymbols(const char* code) {
  for (int i = 0; code[i] != '\0'; i++) {
    if (i > 0) keyUp(SYM_GAP);
    if (code[i] == '.') {
      keyDown(DOT_MS);
      Serial.print('.');
    } else if (code[i] == '-') {
      keyDown(DASH_MS);
      Serial.print('-');
    }
  }
}

void sendMessage(const char* msg) {
  bool firstChar = true;
  for (int i = 0; msg[i] != '\0'; i++) {
    char c = (char)toupper((unsigned char)msg[i]);
    if (c == ' ') {
      keyUp(WORD_GAP);
      Serial.print(" | ");
      firstChar = true;
    } else if (c >= 'A' && c <= 'Z') {
      if (!firstChar) keyUp(LET_GAP);
      Serial.print(c); Serial.print(':');
      sendSymbols(MORSE[c - 'A']);
      Serial.print(' ');
      firstChar = false;
    }
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN,      OUTPUT);
  pinMode(HC12_SET_PIN, OUTPUT);
  digitalWrite(LED_PIN,      HIGH);
  digitalWrite(HC12_SET_PIN, HIGH);

  EEPROM.begin(4);
  currentChannel = loadChannel();

  hc12.begin(9600);
  delay(500);
  configureHC12();

  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  server.on("/",    HTTP_GET,  handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.begin();
  Serial.println("Сервер запущен");

  Serial.printf("Сообщение: \"%s\"  %d WPM  %.1f МГц\n", MESSAGE, WPM, channelToFreq(currentChannel));
}

void loop() {
  server.handleClient();

  Serial.println("\n> Передача:");
  sendMessage(MESSAGE);
  Serial.println("Пауза 5 с");

  unsigned long pauseEnd = millis() + 5000;
  while ((long)(pauseEnd - millis()) > 0) {
    server.handleClient();
    delay(5);
  }
}
