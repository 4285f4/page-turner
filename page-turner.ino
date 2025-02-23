#include "M5StickCPlus2.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <EEPROM.h>
#include <WebServer.h>
#include <ESPmDNS.h>

//================ 默认配置 ================//
#define DEFAULT_SSID "M5StickC-Plus2"
#define DEFAULT_PASS "12345678"
#define DEFAULT_IP "192.168.2.99"
#define DEFAULT_PORT 8080
//=========================================//

//================ 用户配置 ================//
const bool ENABLE_BUTTON_BEEP = true;                            // 启用按键提示音
const bool ENABLE_COUNTDOWN_BEEP = false;                        // 启用倒计时蜂鸣
const bool ENABLE_SCREEN_PROMPT = true;                          // 启用屏幕倒计时提示
const unsigned long SHUTDOWN_TIMEOUT = 600000;                   // 10分钟无操作关机
const unsigned long COUNTDOWN_START = SHUTDOWN_TIMEOUT - 10000;  // 关机前10秒提示
const int BEEP_FREQ = 4000;                                      // 警告蜂鸣频率
const int BEEP_DURATION = 100;                                   // 警告蜂鸣时长(ms)
const int BUTTON_FREQ = 8000;                                    // 按键提示音频率
const int BUTTON_DURATION = 20;                                  // 按键提示音时长(ms)
const int SCREEN_TIMEOUT = 3000;                                 // 启动提示显示时长(ms)
//=========================================//

// 硬件定义
#define BUTTON_C_PIN 35
#define EEPROM_SIZE 128
#define CONFIG_MAGIC 0xAA55

// EEPROM存储结构
struct Config {
  uint16_t magic;
  char ssid[32];
  char password[64];
  char targetIP[16];
  uint16_t targetPort;
};

// 全局变量
WebServer server(80);
Config deviceConfig;
bool screenEnabled = false;  // 是否启用屏幕显示提示信息，可通过电源键切换显示
bool isAPMode = false;
unsigned long lastActivityTime = 0;
unsigned long lastBeepTime = 0;
unsigned long lastScreenUpdate = 0;
int lastRemainingSeconds = -1;
String lastSSID;
String lastIP;
String lastTarget;

// 自定义ButtonC检测
bool isButtonCPressed() {
  static unsigned long lastPress = 0;
  if (digitalRead(BUTTON_C_PIN) == LOW) {
    if (millis() - lastPress > 500) {
      lastPress = millis();
      return true;
    }
  }
  return false;
}

void initDisplay() {
  StickCP2.Display.setBrightness(15);
  StickCP2.Display.setRotation(1);
  StickCP2.Display.setTextColor(GREEN);
  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setTextFont(&fonts::efontCN_16);
  StickCP2.Display.setTextSize(1);
}

void drawStatusScreen(bool forceUpdate = false) {
  // 获取当前状态
  String currentSSID = WiFi.status() == WL_CONNECTED ? String(deviceConfig.ssid) : "";
  String currentIP = WiFi.localIP().toString();
  String currentTarget = String(deviceConfig.targetIP) + ":" + String(deviceConfig.targetPort);

  // 检查是否需要更新
  if (!forceUpdate && currentSSID == lastSSID && currentIP == lastIP && currentTarget == lastTarget) {
    return;
  }

  // 更新状态缓存
  lastSSID = currentSSID;
  lastIP = currentIP;
  lastTarget = currentTarget;

  // 开始绘制
  StickCP2.Display.clear();

  if (WiFi.status() == WL_CONNECTED) {
    StickCP2.Display.setTextColor(GREEN);
    StickCP2.Display.drawString("已连接 WIFI: " + String(deviceConfig.ssid), 0, 10);
    StickCP2.Display.drawString("设备 IP: " + WiFi.localIP().toString(), 0, 30);
    StickCP2.Display.drawString("koreader: " + String(deviceConfig.targetIP) + ":" + String(deviceConfig.targetPort), 0, 50);
  } else {
    StickCP2.Display.setTextColor(RED);
    StickCP2.Display.drawString("连接指定 WIFI 失败", 0, 10);
    StickCP2.Display.drawString("请连接以下热点重新配网", 0, 30);
    StickCP2.Display.setTextColor(GREEN);
    StickCP2.Display.drawString("名称: " DEFAULT_SSID, 0, 50);
    StickCP2.Display.drawString("密码: " DEFAULT_PASS, 0, 70);
  }
}

void toggleScreen() {
  screenEnabled = !screenEnabled;
  if (screenEnabled) {
    StickCP2.Display.wakeup();
    drawStatusScreen(true);
  } else {
    StickCP2.Display.clear();
    StickCP2.Display.sleep();
  }
}

void saveConfig() {
  deviceConfig.magic = CONFIG_MAGIC;
  EEPROM.put(0, deviceConfig);
  EEPROM.commit();
}

void loadConfig() {
  EEPROM.get(0, deviceConfig);
  if (deviceConfig.magic != CONFIG_MAGIC) {
    // 初始化默认配置
    memset(&deviceConfig, 0, sizeof(deviceConfig));
    strncpy(deviceConfig.ssid, DEFAULT_SSID, sizeof(deviceConfig.ssid));
    strncpy(deviceConfig.password, DEFAULT_PASS, sizeof(deviceConfig.password));
    strncpy(deviceConfig.targetIP, DEFAULT_IP, sizeof(deviceConfig.targetIP));
    deviceConfig.targetPort = DEFAULT_PORT;
    saveConfig();
  }
}

void startAPMode() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(DEFAULT_SSID, DEFAULT_PASS);
  isAPMode = true;

  // 显示AP信息
  Serial.println("AP Mode Activated");
  Serial.print("SSID: ");
  Serial.println(DEFAULT_SSID);
  Serial.print("Password: ");
  Serial.println(DEFAULT_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  // 配置AP模式下的Web服务器
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html",
                "<form action='/config' method='POST'>"
                "WiFi 名称: <input type='text' name='ssid' value='"
                  + String(deviceConfig.ssid) + "'><br>"
                                                "WiFi 密码: <input type='password' name='pass' value='"
                  + String(deviceConfig.password) + "'><br>"
                                                    "koreader IP: <input type='text' name='ip' value='"
                  + String(deviceConfig.targetIP) + "'><br>"
                                                    "koreader 端口: <input type='number' name='port' value='"
                  + String(deviceConfig.targetPort) + "'><br>"
                                                      "<input type='submit' value='保存'>"
                                                      "</form>");
  });

  server.on("/config", HTTP_POST, []() {
    strncpy(deviceConfig.ssid, server.arg("ssid").c_str(), sizeof(deviceConfig.ssid));
    strncpy(deviceConfig.password, server.arg("pass").c_str(), sizeof(deviceConfig.password));
    strncpy(deviceConfig.targetIP, server.arg("ip").c_str(), sizeof(deviceConfig.targetIP));
    deviceConfig.targetPort = server.arg("port").toInt();
    saveConfig();
    server.send(200, "text/plain", "配置已保存，正在重启...");
    delay(1000);
    ESP.restart();
  });

  server.begin();  // 确保服务器启动
}

void connectWiFi() {
  WiFi.begin(deviceConfig.ssid, deviceConfig.password);
  unsigned long start = millis();

  // 显示连接状态
  StickCP2.Display.clear();
  StickCP2.Display.setTextDatum(middle_center);
  StickCP2.Display.drawString("正在连接 WiFi...", StickCP2.Display.width() / 2, StickCP2.Display.height() / 2);
  StickCP2.Display.setTextDatum(top_left);

  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
  }

  if (WiFi.status() != WL_CONNECTED) {
    startAPMode();  // 连接失败时启动AP模式
    // 更新屏幕显示
    StickCP2.Display.clear();
    StickCP2.Display.setTextColor(RED);
    StickCP2.Display.drawString("连接指定 WIFI 失败", 0, 10);
    StickCP2.Display.drawString("请连接以下热点重新配网", 0, 30);
    StickCP2.Display.setTextColor(GREEN);
    StickCP2.Display.drawString("名称: " DEFAULT_SSID, 0, 50);
    StickCP2.Display.drawString("密码: " DEFAULT_PASS, 0, 70);
    return;
  }

  // 连接成功后的处理
  if (!MDNS.begin("m5stickc-plus2")) {
    Serial.println("Error setting up MDNS responder!");
  }

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html",
                "<h1>设备控制面板</h1>"
                "<p><a href='/config'>配置WiFi</a></p>");
  });

  server.on("/config", HTTP_GET, []() {
    server.send(200, "text/html",
                "<form action='/update' method='POST'>"
                "新 WIFI 名称: <input type='text' name='ssid' value='"
                  + String(deviceConfig.ssid) + "'><br>"
                                                "新 WIFI 密码: <input type='password' name='pass' value='"
                  + String(deviceConfig.password) + "'><br>"
                                                    "目标IP: <input type='text' name='ip' value='"
                  + String(deviceConfig.targetIP) + "'><br>"
                                                    "目标端口: <input type='number' name='port' value='"
                  + String(deviceConfig.targetPort) + "'><br>"
                                                      "<input type='submit' value='更新'>"
                                                      "</form>");
  });

  server.on("/update", HTTP_POST, []() {
    strncpy(deviceConfig.ssid, server.arg("ssid").c_str(), sizeof(deviceConfig.ssid));
    strncpy(deviceConfig.password, server.arg("pass").c_str(), sizeof(deviceConfig.password));
    strncpy(deviceConfig.targetIP, server.arg("ip").c_str(), sizeof(deviceConfig.targetIP));
    deviceConfig.targetPort = server.arg("port").toInt();
    saveConfig();
    server.send(200, "text/plain", "配置已更新，重启中...");
    delay(1000);
    ESP.restart();
  });

  server.begin();
}


void handleShutdownCountdown(unsigned long currentTime, unsigned long inactiveTime) {
  if (ENABLE_SCREEN_PROMPT) {
    StickCP2.Display.wakeup();
    int remainingSeconds = 10 - ((inactiveTime - COUNTDOWN_START) / 1000);

    if (remainingSeconds != lastRemainingSeconds) {
      StickCP2.Display.clear();
        StickCP2.Display.setTextDatum(middle_center);
      StickCP2.Display.drawString(String(remainingSeconds) + " 秒后关机",
                                  StickCP2.Display.width() / 2,
                                  StickCP2.Display.height() / 2);
      lastRemainingSeconds = remainingSeconds;
      StickCP2.Display.setTextDatum(top_left);
    }
  }

  if (ENABLE_COUNTDOWN_BEEP && (currentTime - lastBeepTime >= 1000)) {
    StickCP2.Speaker.tone(BEEP_FREQ, BEEP_DURATION);
    lastBeepTime = currentTime;
  }
}

void sendTurnPageRequest(int turnPageSize) {
  HTTPClient http;
  String url = "http://" + String(deviceConfig.targetIP) + ":" + String(deviceConfig.targetPort) + "/koreader/event/GotoViewRel/" + String(turnPageSize);
  Serial.printf("send GET request to: %s\n", url);
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode > 0) {
    Serial.printf("GET request sent to %s\n", url.c_str());
  } else {
    Serial.printf("Error sending GET request: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

void setup() {
  auto cfg = M5.config();
  StickCP2.begin(cfg);
  pinMode(BUTTON_C_PIN, INPUT_PULLUP);

  EEPROM.begin(EEPROM_SIZE);
  loadConfig();

  initDisplay();
  delay(1000);

  connectWiFi();
  drawStatusScreen(true);
  delay(SCREEN_TIMEOUT);
  if (!screenEnabled) {
    StickCP2.Display.clear();
    StickCP2.Display.sleep();
  }
  lastActivityTime = millis();
}

void loop() {
  StickCP2.update();
  unsigned long currentTime = millis();
  unsigned long inactiveTime = currentTime - lastActivityTime;

  // 关机检测
  if (inactiveTime >= SHUTDOWN_TIMEOUT) {
    StickCP2.Power.powerOff();
  }
  // 倒计时处理
  else if (inactiveTime >= COUNTDOWN_START) {
    handleShutdownCountdown(currentTime, inactiveTime);
  }

  // 按键处理
  if (StickCP2.BtnA.wasPressed() || StickCP2.BtnB.wasPressed()) {
    lastActivityTime = currentTime;
    lastRemainingSeconds = -1;

    if (ENABLE_BUTTON_BEEP) {
      StickCP2.Speaker.stop();
      StickCP2.Speaker.tone(BUTTON_FREQ, BUTTON_DURATION);
    }

    if (StickCP2.BtnA.wasPressed()) sendTurnPageRequest(1);
    if (StickCP2.BtnB.wasPressed()) sendTurnPageRequest(-1);

    if (!screenEnabled) {
      StickCP2.Display.clear();
      StickCP2.Display.sleep();
    }
  }

  // 处理ButtonC
  if (isButtonCPressed()) {
    toggleScreen();
    lastActivityTime = currentTime;
  }

  // 更新屏幕信息
  if (screenEnabled && (currentTime - lastScreenUpdate >= 1000)) {
    drawStatusScreen();
    lastScreenUpdate = currentTime;
  }

  // 处理Web请求
  server.handleClient();
}
