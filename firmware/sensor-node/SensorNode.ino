#include <WiFi.h>
#include <WebSocketsClient.h>

#define NODE_ID 1   // CHANGE PER BOARD (1–4)

#if NODE_ID < 1 || NODE_ID > 4
#error "NODE_ID must be between 1 and 4"
#endif

// ================= SOIL SENSOR =================
#define SOIL_DRY 3200
#define SOIL_WET 1500
#define SOIL_PIN 34

// ================= WIFI =================
const char* ssid = "TerraNet_820n";
const char* password = "12345678";

// Core IP (DHCP reserved)
IPAddress coreIP(192,168,0,10);
const uint16_t corePort = 81;

WebSocketsClient webSocket;

// 🔧 Dynamic send interval (per node, per boot)
unsigned long SEND_INTERVAL = 5000;
unsigned long lastSend = 0;

// ================= SOIL READ =================
int readSoil() {
  int raw = analogRead(SOIL_PIN);
  int soil = map(raw, SOIL_DRY, SOIL_WET, 0, 100);
  return constrain(soil, 0, 100);
}

// ================= WEBSOCKET EVENTS =================
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {

  switch (type) {

    case WStype_CONNECTED:
      Serial.println("[WS] Connected to Core");

      // Send initial packet immediately
      lastSend = millis() - SEND_INTERVAL;

      webSocket.sendTXT("{\"hello\":true}");
      break;

    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected from Core");
      break;

    case WStype_ERROR:
      Serial.println("[WS] Error");
      break;

    case WStype_PING:
    case WStype_PONG:
      break;

    default:
      break;
  }
}

// ================= SETUP =================
void setup() {

  Serial.begin(115200);
  delay(1000);

  pinMode(SOIL_PIN, INPUT);

  Serial.printf("=== TerraOS Node %d ===\n", NODE_ID);

  // 🔧 Seed randomness properly
  randomSeed(esp_random());

  // 🔧 Random startup delay (prevents WiFi spike)
  delay(random(500,3000));

  // 🔧 Randomized send interval (prevents sync flooding)
  SEND_INTERVAL = 5000 + random(0,2000);

  // ---- WIFI ----
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("Node IP: ");
  Serial.println(WiFi.localIP());

  // ---- WEBSOCKET ----
  webSocket.begin(coreIP, corePort, "/");
  webSocket.onEvent(webSocketEvent);

  // 🔧 Keep connection alive
  webSocket.enableHeartbeat(20000, 3000, 2);

  // 🔧 Prevent reconnect spam
  webSocket.setReconnectInterval(5000);
}

// ================= LOOP =================
void loop() {

  webSocket.loop();

  // 🔧 Send only when connected + interval reached
  if (webSocket.isConnected() && millis() - lastSend >= SEND_INTERVAL) {

    lastSend = millis();

    int soil = readSoil();

    String msg = "{";
    msg += "\"node\":" + String(NODE_ID) + ",";
    msg += "\"soil\":" + String(soil);
    msg += "}";

    webSocket.sendTXT(msg);

    Serial.printf("[TX] Node %d → %d%%\n", NODE_ID, soil);
  }
}
