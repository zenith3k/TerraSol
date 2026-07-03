#include <WiFi.h>
#include <WebSocketsServer.h>
#include <SPI.h>
#include <SD.h>
#include <DS1302.h>
#include <U8g2lib.h>

// ================= WIFI =================
const char* routerSSID = "YOUR_WIFI_SSID";
const char* routerPASS = "YOUR_WIFI_PASSWORD";

WiFiServer httpServer(80);
WebSocketsServer webSocket(81);

// ================= SETTINGS =================
#define NODE_TIMEOUT 20000
#define DRY_THRESHOLD 35
#define WET_THRESHOLD 70

// ================= PUMPS =================
#define PUMP1_PIN 4
#define PUMP2_PIN 16
#define PUMP3_PIN 17
#define PUMP4_PIN 21

bool pumpState[5] = { false,false,false,false,false };

// ================= SD =================
#define SD_CS 15
bool sdOK = false;

// ================= RTC =================
#define RTC_CLK 25
#define RTC_DAT 33
#define RTC_RST 32
DS1302 rtc(RTC_RST, RTC_DAT, RTC_CLK);

// ================= OLED =================
#define OLED_CS   5
#define OLED_DC   27
#define OLED_RST  26
#define OLED_SCK  14
#define OLED_MOSI 13

U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI u8g2(
  U8G2_R0, OLED_SCK, OLED_MOSI,
  OLED_CS, OLED_DC, OLED_RST
);

// ================= STATE =================
int soilNode[5] = { -1,-1,-1,-1,-1 };
unsigned long lastSeen[5] = {0,0,0,0,0};

// ================= BOOT =================
void bootAnimation(){
  digitalWrite(SD_CS,HIGH);
  digitalWrite(OLED_CS,LOW);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_logisoso20_tf);
  u8g2.drawStr(10,28,"TerraOS");
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(38,50,"Beta v3.0");
  u8g2.sendBuffer();
  delay(1500);

  digitalWrite(OLED_CS,HIGH);
}

// ================= RTC =================
bool rtcAvailable(){
  Time t = rtc.time();
  return (t.yr >= 2020 && t.yr <= 2100);
}

// ================= CONTENT TYPE =================
String getContentType(const String& path){
  if(path.endsWith(".HTM")) return "text/html";
  if(path.endsWith(".CSS")) return "text/css";
  if(path.endsWith(".JS"))  return "application/javascript";
  if(path.endsWith(".CSV")) return "text/plain";
  return "text/plain";
}

// ================= FILE =================
void sendFile(WiFiClient& client,const String& path){

  digitalWrite(OLED_CS,HIGH);
  digitalWrite(SD_CS,LOW);

  File file = SD.open(path.c_str(),FILE_READ);

  if(!file){
    client.println("HTTP/1.1 404 Not Found");
    client.println();
    digitalWrite(SD_CS,HIGH);
    return;
  }

  client.println("HTTP/1.1 200 OK");
  client.print("Content-Type: ");
  client.println(getContentType(path));
  client.println();

  while(file.available())
    client.write(file.read());

  file.close();
  digitalWrite(SD_CS,HIGH);
}

// ================= LOG =================
String logFile(uint8_t node){
  Time t = rtc.time();
  char name[24];
  sprintf(name,"/NODE%d_%02d%02d%02d.CSV",
          node,t.yr%100,t.mon,t.date);
  return String(name);
}

void logPumpOn(uint8_t node){
  if(!sdOK || !rtcAvailable()) return;

  File file = SD.open(logFile(node).c_str(),FILE_APPEND);
  if(!file) return;

  Time t = rtc.time();
  file.printf("%02d:%02d:%02d,%d,PUMP_ON\n",
              t.hr,t.min,t.sec,soilNode[node]);
  file.close();
}

// ================= PUMP =================
void setPump(uint8_t node, bool state){

  int pin =
    (node==1)?PUMP1_PIN:
    (node==2)?PUMP2_PIN:
    (node==3)?PUMP3_PIN:PUMP4_PIN;

  pumpState[node] = state;
  digitalWrite(pin, state ? LOW : HIGH);
}

void updatePump(uint8_t node){

  if (soilNode[node] < 0) return;

  if (!pumpState[node] && soilNode[node] < DRY_THRESHOLD) {
    setPump(node,true);
    logPumpOn(node);
  }
  else if (pumpState[node] && soilNode[node] > WET_THRESHOLD) {
    setPump(node,false);
  }
}

// ================= TIMEOUT =================
void handleNodeTimeouts(){
  unsigned long now = millis();

  for(int node=1;node<=4;node++){
    if(soilNode[node]>=0 && (now-lastSeen[node]>NODE_TIMEOUT)){
      soilNode[node]=-1;
      setPump(node,false);
    }
  }
}

// ================= OLED =================
void drawOLED(){

  digitalWrite(SD_CS,HIGH);
  digitalWrite(OLED_CS,LOW);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  if(rtcAvailable()){
    Time t = rtc.time();
    int h = t.hr%12; if(h==0) h=12;

    char buf[16];
    sprintf(buf,"%02d:%02d %s",h,t.min,t.hr>=12?"PM":"AM");
    u8g2.drawStr(0,12,buf);
  }

  u8g2.drawStr(86,12,sdOK?"SD:OK":"SD:--");
  u8g2.drawHLine(0,15,128);

  for(int i=1;i<=4;i++){
    char line[22];

    if(soilNode[i]>=0)
      sprintf(line,"N%d  %3d%%  %s",
              i,soilNode[i],pumpState[i]?"ON ":"OFF");
    else
      sprintf(line,"N%d   --   OFF",i);

    u8g2.drawStr(0,15+i*12,line);
  }

  u8g2.sendBuffer();
  digitalWrite(OLED_CS,HIGH);
}

// ================= WEBSOCKET =================
void webSocketEvent(uint8_t clientID,WStype_t type,
                    uint8_t *payload,size_t length){

  if(type != WStype_TEXT) return;

  String msg;
  for(size_t i=0;i<length;i++) msg+=(char)payload[i];

  // ===== UI COMMAND =====
  if(msg.indexOf("\"cmd\"") >= 0){

    if(msg.indexOf("pump") >= 0){

      int node = msg.substring(msg.indexOf("\"node\"")+7).toInt();
      int state = msg.substring(msg.indexOf("\"state\"")+8).toInt();

      if(node>=1 && node<=4){
        setPump(node,state);
      }
    }
    return;
  }

  // ===== NODE DATA =====
  if(msg.indexOf("\"node\"")<0 ||
     msg.indexOf("\"soil\"")<0) return;

  int node = msg.substring(msg.indexOf("\"node\"")+7).toInt();
  int soil = msg.substring(msg.indexOf("\"soil\"")+7).toInt();

  if(node<1 || node>4) return;

  soilNode[node]=soil;
  lastSeen[node]=millis();

  updatePump(node);

  String out = "{";
  out += "\"node\":" + String(node) + ",";
  out += "\"soil\":" + String(soil) + ",";
  out += "\"pump\":" + String(pumpState[node]);
  out += "}";

  webSocket.broadcastTXT(out);
}

// ================= LOG HANDLER =================
void handleLogRequest(WiFiClient& client,int node){

  if(!sdOK){
    client.println("HTTP/1.1 500 Internal Server Error");
    client.println();
    return;
  }

  String filename = logFile(node);

  digitalWrite(OLED_CS,HIGH);
  digitalWrite(SD_CS,LOW);

  File file = SD.open(filename.c_str(), FILE_READ);

  if(!file){
    client.println("HTTP/1.1 404 Not Found");
    client.println("Content-Type: text/plain");
    client.println();
    client.println("LOG FILE NOT FOUND");

    digitalWrite(SD_CS,HIGH);
    return;
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/plain");
  client.println();

  while(file.available())
    client.write(file.read());

  file.close();
  digitalWrite(SD_CS,HIGH);
}

// ================= HTTP =================
void handleHttpClient(){

  WiFiClient client = httpServer.available();
  if(!client) return;

  unsigned long timeout = millis()+1000;
  while(!client.available() && millis()<timeout) delay(1);
  if(!client.available()){ client.stop(); return; }

  String request = client.readStringUntil('\r');
  client.readStringUntil('\n');

  int start = request.indexOf(' ')+1;
  int end   = request.indexOf(' ',start);
  if(start<=0 || end<=start){ client.stop(); return; }

  String path = request.substring(start,end);

  if(path.startsWith("/log")){
    int node = 1;
    int idx = path.indexOf("node=");
    if(idx>=0) node = path.substring(idx+5).toInt();

    handleLogRequest(client,node);
    client.stop();
    return;
  }

  if(path=="/") path="/INDEX.HTM";

  while(client.available()) client.read();
  sendFile(client,path);

  client.stop();
}

// ================= SETUP =================
void setup(){

  Serial.begin(115200);

  pinMode(PUMP1_PIN,OUTPUT);
  pinMode(PUMP2_PIN,OUTPUT);
  pinMode(PUMP3_PIN,OUTPUT);
  pinMode(PUMP4_PIN,OUTPUT);

  setPump(1,false);
  setPump(2,false);
  setPump(3,false);
  setPump(4,false);

  pinMode(SD_CS,OUTPUT);
  pinMode(OLED_CS,OUTPUT);

  digitalWrite(SD_CS,HIGH);
  digitalWrite(OLED_CS,HIGH);

  u8g2.begin();
  bootAnimation();

  rtc.writeProtect(false);
  rtc.halt(false);

  SPI.begin(18,19,23,SD_CS);
  sdOK = SD.begin(SD_CS,SPI);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(routerSSID,routerPASS);

  Serial.print("Connecting to WiFi");

  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("Core IP: ");
  Serial.println(WiFi.localIP());

  httpServer.begin();
  webSocket.begin();
  webSocket.enableHeartbeat(20000,3000,2);
  webSocket.onEvent(webSocketEvent);
}

// ================= LOOP =================
void loop(){

  handleHttpClient();
  webSocket.loop();
  handleNodeTimeouts();

  static unsigned long lastOLED=0;
  if(millis()-lastOLED>=1000){
    lastOLED=millis();
    drawOLED();
  }
}
