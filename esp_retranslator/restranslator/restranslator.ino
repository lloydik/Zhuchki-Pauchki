// Ретранслятор на ESP32 и двух HC-12.
// Один модуль слушает канал, второй сразу отдаёт те же байты на другом.
// Своего протокола нет: что пришло с UART, то и ушло в эфир.
//
// Сеть для настройки сама не поднимается. Нажали BOOT на плате —
// появляется точка HC12-Retranslator. Минуту никто не открывал
// страницу — Wi-Fi гаснет, ретранслятор дальше работает без него.
//
// Провода (земля и 3.3 В у обоих модулей общие с платой):
//   приём:    TXD -> GPIO16, RXD -> GPIO17, SET -> GPIO18
//   передача: TXD -> GPIO4,  RXD -> GPIO2,  SET -> GPIO19
// Светодиод DevKit — это тоже GPIO2. Мигать им нельзя: пин отбирается
// у UART и второй модуль перестаёт передавать. Смотреть, идёт ли
// ретрансляция, можно по счётчику на странице и в мониторе порта.
//
// Каналы как у радиопередатчика: 1 это 433.4 МГц, шаг 0.4, всего 100.

#include <HardwareSerial.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

const char *AP_SSID = "HC12-Retranslator";
const char *AP_PASS = "12345678";

// минута без заходов на страницу
#define WIFI_TIMEOUT 60000

#define DEFAULT_RX_CH 1
#define DEFAULT_TX_CH 50
#define DEFAULT_MODE  3
#define HC12_BAUD     9600

#define HC12_RX_RX  16
#define HC12_RX_TX  17
#define HC12_RX_SET 18

#define HC12_TX_RX  4
#define HC12_TX_TX  2
#define HC12_TX_SET 19

#define BOOT_BTN 0

#define BUF_SIZE 512
#define UART_BUF 1024

HardwareSerial hc12Rx(1);
HardwareSerial hc12Tx(2);
WebServer server(80);
Preferences prefs;

int rxChannel = DEFAULT_RX_CH;
int txChannel = DEFAULT_TX_CH;
int fuMode = DEFAULT_MODE;

uint32_t totalBytes = 0;
uint32_t totalPackets = 0;
unsigned long startTime = 0;

bool wifiOn = false;
unsigned long lastVisit = 0;
bool needReconfigure = false;

float channelToFreq(int ch) {
  return 433.4f + (ch - 1) * 0.4f;
}

// AT+C ждёт три цифры: 1 -> 001
String formatChannel(int ch) {
  String s = String(ch);
  while (s.length() < 3) s = "0" + s;
  return s;
}

String fuModeStr(int mode) {
  return "FU" + String(mode);
}

// SET к земле — модуль читает команду, а не передаёт её в эфир.
// В конце обязателен перевод строки, без него HC-12 просто молчит
// (в скетче радиопередатчика команды такие же).
String sendAT(HardwareSerial &port, int setPin, const char *cmd) {
  digitalWrite(setPin, LOW);
  delay(100);
  while (port.available()) port.read();

  port.print(cmd);
  port.print("\r\n");
  port.flush();

  String answer = "";
  unsigned long t0 = millis();
  while (millis() - t0 < 500) {
    while (port.available()) {
      answer += (char)port.read();
    }
  }

  digitalWrite(setPin, HIGH);
  delay(100);
  answer.trim();
  return answer;
}

// После прошлых опытов модуль мог остаться на другой скорости.
// Перебираем, пока не ответит OK на AT.
bool findBaud(HardwareSerial &port, int setPin) {
  const long bauds[] = {9600, 1200, 2400, 4800, 19200, 38400, 57600, 115200};

  for (int i = 0; i < 8; i++) {
    port.updateBaudRate(bauds[i]);
    delay(50);
    String answer = sendAT(port, setPin, "AT");
    if (answer.indexOf("OK") >= 0) {
      Serial.printf("  нашёлся на %ld\n", bauds[i]);
      return true;
    }
  }

  // не оставляем UART на последней (115200), иначе в эфир уйдёт каша
  port.updateBaudRate(HC12_BAUD);
  return false;
}

void configureHC12(HardwareSerial &port, int setPin, int channel, int mode,
                   const char *name, bool searchBaud) {
  Serial.printf("[%s] канал %d (%.1f МГц), FU%d\n",
                name, channel, channelToFreq(channel), mode);

  if (searchBaud) {
    if (!findBaud(port, setPin)) {
      Serial.printf("[%s] молчит, проверь питание, TX/RX и SET\n", name);
      return;
    }
    sendAT(port, setPin, "AT+B9600");
    port.updateBaudRate(HC12_BAUD);
    delay(50);
  }

  String chCmd = "AT+C" + formatChannel(channel);
  String chAns = sendAT(port, setPin, chCmd.c_str());
  Serial.printf("  %s -> %s\n", chCmd.c_str(), chAns.c_str());

  String modeCmd = "AT+" + fuModeStr(mode);
  String modeAns = sendAT(port, setPin, modeCmd.c_str());
  Serial.printf("  %s -> %s\n", modeCmd.c_str(), modeAns.c_str());
}

void startWiFi() {
  if (wifiOn) return;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.begin();

  wifiOn = true;
  lastVisit = millis();

  Serial.printf("[WiFi] AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void stopWiFi() {
  if (!wifiOn) return;

  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiOn = false;
  Serial.println("[WiFi] выключен");
}

String uptimeStr() {
  unsigned long sec = (millis() - startTime) / 1000UL;
  int h = sec / 3600;
  int m = (sec % 3600) / 60;
  int s = sec % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  return String(buf);
}

void handleRoot() {
  lastVisit = millis();

  String html;
  // без запаса страница иногда выходит пустой: Wi-Fi уже занял кучу
  html.reserve(4096);
  html += "<!DOCTYPE html><html><head>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Ретранслятор</title>";
  html += "<style>";
  html += "body{font-family:sans-serif;max-width:460px;margin:32px auto;padding:0 16px;background:#f0f2f5}";
  html += "h1{margin-bottom:4px}";
  html += ".card{background:#fff;border-radius:8px;padding:16px;margin-top:14px;box-shadow:0 1px 4px #0002}";
  html += ".row{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #eee}";
  html += ".row:last-child{border:none}";
  html += ".muted{color:#777;font-size:.9em}";
  html += ".freq{color:#1a73e8;font-weight:bold}";
  html += "label{display:block;margin-top:12px;font-size:.9em;color:#555}";
  html += "input,select{width:100%;padding:9px;font-size:1em;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;margin-top:4px}";
  html += "button{margin-top:16px;width:100%;padding:11px;font-size:1em;background:#1a73e8;color:#fff;border:none;border-radius:4px;cursor:pointer}";
  html += ".ok{background:#e6f4ea;color:#137333;padding:8px 12px;border-radius:6px;margin-top:12px}";
  html += ".cols{display:flex;gap:12px}.cols>div{flex:1}";
  html += "</style></head><body>";
  html += "<h1>Ретранслятор</h1>";
  html += "<div class='muted'>Точка погаснет через минуту без обновления страницы. Снова включить — кнопка BOOT.</div>";

  if (server.hasArg("ok")) {
    html += "<div class='ok'>Настройки записал.</div>";
  }

  html += "<div class='card'>";
  html += "<div class='row'><span>Аптайм</span><span>" + uptimeStr() + "</span></div>";
  html += "<div class='row'><span>Байт переслано</span><span>" + String(totalBytes) + "</span></div>";
  html += "<div class='row'><span>Пакетов</span><span>" + String(totalPackets) + "</span></div>";
  html += "<div class='row'><span>Приём</span><span class='freq'>" + String(channelToFreq(rxChannel), 1);
  html += " МГц</span></div>";
  html += "<div class='row'><span>Передача</span><span class='freq'>" + String(channelToFreq(txChannel), 1);
  html += " МГц</span></div>";
  html += "<div class='row'><span>Режим</span><span>" + fuModeStr(fuMode) + "</span></div>";
  html += "</div>";

  html += "<div class='card'><form method='POST' action='/save'>";
  html += "<div class='cols'><div>";
  html += "<label>Канал приёма (1–100)";
  html += "<input type='number' name='rx' min='1' max='100' value='" + String(rxChannel) + "'>";
  html += "</label></div><div>";
  html += "<label>Канал передачи (1–100)";
  html += "<input type='number' name='tx' min='1' max='100' value='" + String(txChannel) + "'>";
  html += "</label></div></div>";

  html += "<label>Режим HC-12<select name='mode'>";
  html += "<option value='1'";
  if (fuMode == 1) html += " selected";
  html += ">FU1 — короткая задержка, около 100 м</option>";
  html += "<option value='2'";
  if (fuMode == 2) html += " selected";
  html += ">FU2 — середина, около 300 м</option>";
  html += "<option value='3'";
  if (fuMode == 3) html += " selected";
  html += ">FU3 — дальность, около 1000 м</option>";
  html += "</select></label>";
  html += "<button type='submit'>Применить</button>";
  html += "</form></div>";
  html += "</body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
  lastVisit = millis();

  if (server.hasArg("rx") && server.hasArg("tx") && server.hasArg("mode")) {
    int newRx = server.arg("rx").toInt();
    int newTx = server.arg("tx").toInt();
    int newMode = server.arg("mode").toInt();

    if (newRx >= 1 && newRx <= 100) rxChannel = newRx;
    if (newTx >= 1 && newTx <= 100) txChannel = newTx;
    if (newMode >= 1 && newMode <= 3) fuMode = newMode;

    // на ESP32 это flash, не EEPROM, как было на 8266 в радио
    prefs.begin("hc12", false);
    prefs.putInt("rxCh", rxChannel);
    prefs.putInt("txCh", txChannel);
    prefs.putInt("fuMode", fuMode);
    prefs.end();

    // сами модули трогаем уже в loop: отсюда UART лучше не занимать
    needReconfigure = true;
    Serial.printf("сохранил: приём %d, передача %d, %s\n",
                  rxChannel, txChannel, fuModeStr(fuMode).c_str());
  }

  server.sendHeader("Location", "/?ok=1");
  server.send(302, "text/plain", "");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BOOT_BTN, INPUT_PULLUP);

  prefs.begin("hc12", true);
  rxChannel = prefs.getInt("rxCh", DEFAULT_RX_CH);
  txChannel = prefs.getInt("txCh", DEFAULT_TX_CH);
  fuMode = prefs.getInt("fuMode", DEFAULT_MODE);
  prefs.end();

  if (rxChannel < 1 || rxChannel > 100) rxChannel = DEFAULT_RX_CH;
  if (txChannel < 1 || txChannel > 100) txChannel = DEFAULT_TX_CH;
  if (fuMode < 1 || fuMode > 3) fuMode = DEFAULT_MODE;

  Serial.println();
  Serial.println("=== HC-12 ретранслятор ===");
  Serial.printf("Приём:    канал %s (%.1f МГц)\n",
                formatChannel(rxChannel).c_str(), channelToFreq(rxChannel));
  Serial.printf("Передача: канал %s (%.1f МГц)\n",
                formatChannel(txChannel).c_str(), channelToFreq(txChannel));
  Serial.printf("Режим: %s\n", fuModeStr(fuMode).c_str());

  WiFi.mode(WIFI_OFF);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  Serial.println("Wi-Fi выключен. BOOT — открыть настройку.");

  pinMode(HC12_RX_SET, OUTPUT);
  pinMode(HC12_TX_SET, OUTPUT);
  digitalWrite(HC12_RX_SET, HIGH);
  digitalWrite(HC12_TX_SET, HIGH);

  // размер буфера ставится до begin, иначе ESP32 его не применит
  hc12Rx.setRxBufferSize(UART_BUF);
  hc12Tx.setRxBufferSize(UART_BUF);
  hc12Rx.begin(HC12_BAUD, SERIAL_8N1, HC12_RX_RX, HC12_RX_TX);
  hc12Tx.begin(HC12_BAUD, SERIAL_8N1, HC12_TX_RX, HC12_TX_TX);
  delay(200);

  configureHC12(hc12Rx, HC12_RX_SET, rxChannel, fuMode, "RX", true);
  configureHC12(hc12Tx, HC12_TX_SET, txChannel, fuMode, "TX", true);

  startTime = millis();
  Serial.println("Можно слушать. Байты с приёма сразу уходят на передачу.");
}

void loop() {
  // BOOT на плате замыкает GPIO0 на землю
  if (digitalRead(BOOT_BTN) == LOW) {
    delay(50);
    if (digitalRead(BOOT_BTN) == LOW) {
      startWiFi();
      while (digitalRead(BOOT_BTN) == LOW) delay(10);
    }
  }

  if (wifiOn) {
    server.handleClient();
    if (millis() - lastVisit > WIFI_TIMEOUT) {
      stopWiFi();
    }
  }

  if (needReconfigure) {
    needReconfigure = false;
    configureHC12(hc12Rx, HC12_RX_SET, rxChannel, fuMode, "RX", false);
    configureHC12(hc12Tx, HC12_TX_SET, txChannel, fuMode, "TX", false);
    totalBytes = 0;
    totalPackets = 0;
    Serial.println("каналы переключил");
  }

  // ничего не разбираем: сколько байт пришло, столько отдали второму модулю
  static uint8_t buf[BUF_SIZE];
  int waiting = hc12Rx.available();
  if (waiting > 0) {
    if (waiting > BUF_SIZE) waiting = BUF_SIZE;
    int got = hc12Rx.readBytes(buf, waiting);
    if (got > 0) {
      hc12Tx.write(buf, got);
      totalBytes += got;
      totalPackets++;
    }
  }
}
