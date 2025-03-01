/**
 * @file page-turner.ino
 * @author onehero (onehero0927@163.com)
 * @brief A page turner for KOReader made with M5StickC Plus 2 and Arduino.
 * @version 1.0
 * @date 2025-02-23
 */

#include <M5StickCPlus2.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

/**===============宏定义===============*/
#define EEPROM_SIZE sizeof(Config)
#define BEEP_SHUTDOWN_FREQ 4000
#define BEEP_SHUTDOWN_DURATION 100
#define BEEP_PRESSED_FREQ 8000
#define BEEP_PRESSED_DURATION 20
#define DEVICE_SSID "M5StickC-Plus2"
#define DEVICE_PASSWORD "12345678"
#define BTN_PWR_PIN 35

/**===============配置结构体===============*/
struct Config {
  char ssid[32] = "defaultSSID";          // 默认 WiFi 名称
  char password[32] = "defaultPass";      // 默认 WiFi 密码
  char koReaderIP[16] = "192.168.1.100";  // 默认 KOReader IP
  int koReaderPort = 8080;                // 默认 KOReader 端口
  int aBtnClickAction = 1;                // 按键 A 单击（1:下一页）
  int aBtnHoldAction = 2;                 // 按键 A 长按（2:完全刷新）
  int bBtnClickAction = 0;                // 按键 B 单击（0:上一页）
  int bBtnHoldAction = 3;                 // 按键 B 长按（3:切换灯光）
  bool keepScreenOn = true;               // 是否保持屏幕开启
  bool beepOnPress = true;                // 按键蜂鸣开关
  int autoShutdownTime = 600;             // 自动关机时间（秒），默认 10 分钟
  bool shutdownCountdownScreen = true;    // 倒计时屏幕提示开关，默认开
  bool shutdownCountdownBeep = false;     // 倒计时蜂鸣提示开关，默认关
  int countdownTime = 10;                 // 倒计时提示时间（秒），默认 10 秒
};

/**===============全局变量===============*/
Config config;
WebServer server(80);
HTTPClient http;
String lastDisplayedText = "";                // 上次显示文本
unsigned long lastActiveTime = 0;             // 上次活跃时间
unsigned long lastScreenUpdateTime = 0;       // 上次屏幕更新时间
unsigned long pwrBtnpressStartTime = 0;       // 电源键按下的时间
uint16_t textColor = GREEN;                   // 动态字体颜色
const unsigned long longPressDuration = 500;  // 电源键长按的时间阈值，单位为毫秒
const int batteryThreshold = 5;               // 电量变动阈值
int lastRemainingSeconds = -1;                // 倒计时剩余秒数
int lastBatteryLevel = -1;                    // 上一次的电量
bool turnOnScreen = true;                     // 是否要开启屏幕
bool delayToTurnOffScreen;                    // 延迟关闭屏幕
bool onCountdown = false;                     // 是否处于倒计时中
bool screenStateBeforeCountdown = true;       // 进入倒计时模式时前的屏幕开关状态

/**===============函数===============*/

// 加载配置
void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);
  EEPROM.end();
  // 检查是否首次运行
  if (strcmp(config.ssid, "defaultSSID") == 0) {
    saveConfig();  // 首次运行时保存默认配置
  }
  delayToTurnOffScreen = !config.keepScreenOn;
}

// 保存配置
void saveConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();
}

// 屏幕显示优化（避免闪烁）
void displayText(String text) {
  if (text != lastDisplayedText) {
    StickCP2.Display.fillScreen(BLACK);
    StickCP2.Display.setCursor(0, 0);
    StickCP2.Display.setTextSize(1);
    StickCP2.Display.setTextColor(textColor);
    StickCP2.Display.println(text);
    lastDisplayedText = text;
  }
}

// 绘制当前设备信息
void drawInfoScreen() {
  String info;
  if (WiFi.status() == WL_CONNECTED) {
    info = "已连接 WiFi: " + String(WiFi.SSID()) + "\n设备 IP: " + WiFi.localIP().toString() + "\nKOReader: " + config.koReaderIP + ":" + config.koReaderPort;
    textColor = GREEN;
  } else {
    info = "热点名称: " + String(DEVICE_SSID) + "\n热点密码: " + String(DEVICE_PASSWORD) + "\nIP: " + WiFi.softAPIP().toString();
    textColor = RED;
  }

  // 获取当前电池电量
  int currentBatteryLevel = StickCP2.Power.getBatteryLevel();
  // 只有当电量变化超过阈值或者第一次获取时才更新显示
  if (lastBatteryLevel < 0 || abs(currentBatteryLevel - lastBatteryLevel) >= batteryThreshold) {
    lastBatteryLevel = currentBatteryLevel;
  }
  info += "\n电量: " + String(lastBatteryLevel) + "%";
  displayText(info);
}

// WiFi 设置
void setupWiFi() {
  textColor = RED;
  displayText("正在连接 WiFi...");
  WiFi.begin(config.ssid, config.password);
  // 在 10s 内，每隔 500ms 检查一次 WIFI 连接状态
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
  }

  // WiFi 连接失败时开启热点
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.softAP(DEVICE_SSID, DEVICE_PASSWORD);
  }

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.begin();
}

// 发送 HTTP 请求
void sendRequest(int action) {
  if (WiFi.status() != WL_CONNECTED) return;
  String url;
  switch (action) {
    case 0: url = "http://" + String(config.koReaderIP) + ":" + String(config.koReaderPort) + "/koreader/event/GotoViewRel/-1"; break;    // 上一页
    case 1: url = "http://" + String(config.koReaderIP) + ":" + String(config.koReaderPort) + "/koreader/event/GotoViewRel/1"; break;     // 下一页
    case 2: url = "http://" + String(config.koReaderIP) + ":" + String(config.koReaderPort) + "/koreader/event/FullRefresh"; break;       // 完全刷新
    case 3: url = "http://" + String(config.koReaderIP) + ":" + String(config.koReaderPort) + "/koreader/event/ToggleFrontlight"; break;  // 切换灯光
    default: return;
  }
  http.begin(url);
  http.GET();
  http.end();
}

// WebServer 主页
void handleRoot() {
  String html = R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>M5StickC-Plus2 配置</title>
    <style>
        body { font-family: Arial, sans-serif; text-align: center; }
        form { display: inline-block; text-align: left; }
        label { display: block; margin: 5px 0; }
        input[type="submit"] { width: 30%; padding: 10px; display: block; margin: 10px auto; }
    </style>
</head>
<body>
    <h1>设备配置</h1>
    <form action="/save" method="post">
        <label>WiFi 名称: <input type="text" name="ssid" value=")"
                + String(config.ssid) + R"(" maxlength="31"></label>
        <label>WiFi 密码: <input type="text" name="password" value=")"
                + String(config.password) + R"(" maxlength="31"></label>
        <label>KOReader IP: <input type="text" name="koReaderIP" value=")"
                + String(config.koReaderIP) + R"(" maxlength="15"></label>
        <label>KOReader 端口: <input type="number" name="koReaderPort" value=")"
                + String(config.koReaderPort) + R"(" min="1" max="65535"></label>
        <label>按键 A 单击: <select name="aClick">
            <option value="0" )"
                + (config.aBtnClickAction == 0 ? "selected" : "") + R"(>上一页</option>
            <option value="1" )"
                + (config.aBtnClickAction == 1 ? "selected" : "") + R"(>下一页</option>
            <option value="2" )"
                + (config.aBtnClickAction == 2 ? "selected" : "") + R"(>完全刷新</option>
            <option value="3" )"
                + (config.aBtnClickAction == 3 ? "selected" : "") + R"(>切换灯光</option>
        </select></label>
        <label>按键 A 长按: <select name="aLong">
            <option value="0" )"
                + (config.aBtnHoldAction == 0 ? "selected" : "") + R"(>上一页</option>
            <option value="1" )"
                + (config.aBtnHoldAction == 1 ? "selected" : "") + R"(>下一页</option>
            <option value="2" )"
                + (config.aBtnHoldAction == 2 ? "selected" : "") + R"(>完全刷新</option>
            <option value="3" )"
                + (config.aBtnHoldAction == 3 ? "selected" : "") + R"(>切换灯光</option>
        </select></label>
        <label>按键 B 单击: <select name="bClick">
            <option value="0" )"
                + (config.bBtnClickAction == 0 ? "selected" : "") + R"(>上一页</option>
            <option value="1" )"
                + (config.bBtnClickAction == 1 ? "selected" : "") + R"(>下一页</option>
            <option value="2" )"
                + (config.bBtnClickAction == 2 ? "selected" : "") + R"(>完全刷新</option>
            <option value="3" )"
                + (config.bBtnClickAction == 3 ? "selected" : "") + R"(>切换灯光</option>
        </select></label>
        <label>按键 B 长按: <select name="bLong">
            <option value="0" )"
                + (config.bBtnHoldAction == 0 ? "selected" : "") + R"(>上一页</option>
            <option value="1" )"
                + (config.bBtnHoldAction == 1 ? "selected" : "") + R"(>下一页</option>
            <option value="2" )"
                + (config.bBtnHoldAction == 2 ? "selected" : "") + R"(>完全刷新</option>
            <option value="3" )"
                + (config.bBtnHoldAction == 3 ? "selected" : "") + R"(>切换灯光</option>
        </select></label>
        <label>保持屏幕开启: <input type="checkbox" name="keepScreenOn" )"
                + (config.keepScreenOn ? "checked" : "") + R"(></label>
        <label>按键蜂鸣: <input type="checkbox" name="beepOnPress" )"
                + (config.beepOnPress ? "checked" : "") + R"(></label>
        <label>自动关机时间(秒): <input type="number" name="autoShutdownTime" value=")"
                + String(config.autoShutdownTime) + R"(" min="0"></label>
        <label>倒计时屏幕提示: <input type="checkbox" name="shutdownCountdownScreen" )"
                + (config.shutdownCountdownScreen ? "checked" : "") + R"(></label>
        <label>倒计时蜂鸣提示: <input type="checkbox" name="shutdownCountdownBeep" )"
                + (config.shutdownCountdownBeep ? "checked" : "") + R"(></label>
        <label>倒计时提示时间(秒): <input type="number" name="countdownTime" value=")"
                + String(config.countdownTime) + R"(" min="1"></label>
        <input type="submit" value="保存">
    </form>
</body>
</html>
    )";
  server.send(200, "text/html; charset=UTF-8", html);
}

// 保存配置
void handleSave() {
  strncpy(config.ssid, server.arg("ssid").c_str(), sizeof(config.ssid));
  strncpy(config.password, server.arg("password").c_str(), sizeof(config.password));
  strncpy(config.koReaderIP, server.arg("koReaderIP").c_str(), sizeof(config.koReaderIP));
  config.koReaderPort = server.arg("koReaderPort").toInt();
  config.aBtnClickAction = server.arg("aClick").toInt();
  config.aBtnHoldAction = server.arg("aLong").toInt();
  config.bBtnClickAction = server.arg("bClick").toInt();
  config.bBtnHoldAction = server.arg("bLong").toInt();
  config.keepScreenOn = server.hasArg("keepScreenOn");
  config.beepOnPress = server.hasArg("beepOnPress");
  config.autoShutdownTime = server.arg("autoShutdownTime").toInt();
  config.shutdownCountdownScreen = server.hasArg("shutdownCountdownScreen");
  config.shutdownCountdownBeep = server.hasArg("shutdownCountdownBeep");
  config.countdownTime = server.arg("countdownTime").toInt();
  saveConfig();
  String html = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"></head><body style=\"text-align: center;\"><h1>配置已保存，正在重启...</h1><a href=\"/\">返回</a></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
  delay(1000);
  ESP.restart();
}

// A、B 按钮事件处理方法
void handleNormalBtnEvent(m5::Button_Class& btn, int clickAction, int holdAction) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (btn.wasClicked() || btn.wasHold()) {
    if (config.beepOnPress) {
      StickCP2.Speaker.stop();
      StickCP2.Speaker.tone(BEEP_PRESSED_FREQ, BEEP_PRESSED_DURATION);
    }
    lastActiveTime = millis();
    lastRemainingSeconds = -1;
    if (btn.wasClicked()) {
      sendRequest(clickAction);
    } else if (btn.wasHold()) {
      sendRequest(holdAction);
    }
  }
}

// 自定义电源键单击检测方法
bool isPWRBtnClicked() {
  int btnState = digitalRead(BTN_PWR_PIN);

  if (btnState == LOW) {                // 按键被按下
    if (pwrBtnpressStartTime == 0) {    // 如果是第一次按下
      pwrBtnpressStartTime = millis();  // 记录按下时间
    }
  } else {                                                            // 按键被松开
    if (pwrBtnpressStartTime != 0) {                                  // 如果之前有按下记录
      unsigned long pressDuration = millis() - pwrBtnpressStartTime;  // 计算按下持续时间

      pwrBtnpressStartTime = 0;                 // 重置按下时间
      if (pressDuration < longPressDuration) {  // 如果按下时间小于长按阈值
        return true;
      }
    }
  }
  return false;
}

// 电源按钮事件处理方法
void handlePWRBtnEvent() {
  // 使用自定义的电源键单击检测方法而不是 M5.BtnPWR.wasClicked()
  // 是因为后者在断开 USB 连接时长按电源键开机会被识别成单击，导致屏幕被立刻关闭（原因未知）
  if (isPWRBtnClicked()) {
    lastActiveTime = millis();
    lastRemainingSeconds = -1;
    if (config.beepOnPress) {
      StickCP2.Speaker.stop();
      StickCP2.Speaker.tone(BEEP_PRESSED_FREQ, BEEP_PRESSED_DURATION);
    }
    turnOnScreen = !turnOnScreen;
    // 禁用延时关闭屏幕功能
    delayToTurnOffScreen = false;
  }
}

// 处理倒计时事件
void handleShutdownCountdown(unsigned long currentTime, unsigned long inactiveTime) {
  if (config.shutdownCountdownScreen) {
    if (!onCountdown) {
      screenStateBeforeCountdown = turnOnScreen;  // 保存当前屏幕状态
    }
    turnOnScreen = true;
    onCountdown = true;
    int remainingSeconds = config.autoShutdownTime - inactiveTime / 1000;
    if (remainingSeconds != lastRemainingSeconds) {
      if (config.shutdownCountdownBeep) {
        StickCP2.Speaker.tone(BEEP_SHUTDOWN_FREQ, BEEP_SHUTDOWN_DURATION);
      }
      textColor = RED;
      displayText("关机倒计时: " + String(remainingSeconds) + " 秒");
      lastRemainingSeconds = remainingSeconds;
    }
  }
}

// 长时间无操作自动关机处理方法
void handleAutoShutdown() {
  // 长时间无操作自动关机逻辑
  unsigned long currentTime = millis();
  unsigned long inactiveTime = currentTime - lastActiveTime;
  if (inactiveTime >= config.autoShutdownTime * 1000) {
    StickCP2.Power.powerOff();
  } else if (inactiveTime >= (config.autoShutdownTime - config.countdownTime) * 1000) {
    // 倒计时处理
    handleShutdownCountdown(currentTime, inactiveTime);
  } else {
    if (onCountdown) {
      turnOnScreen = screenStateBeforeCountdown;  // 恢复屏幕状态
      onCountdown = false;
    }
  }
}

// 屏幕开关处理
void handleScreenSwitch() {
  // 保持屏幕常亮开关=开，WIFI 未连接，屏幕常开，电源键可切换屏幕开关
  // 保持屏幕常亮开关=开，WIFI 已连接，屏幕常开，电源键可切换屏幕开关
  // 保持屏幕常亮开关=关，WIFI 已连接，屏幕等待五秒后关闭，电源键可切换屏幕开关
  // 保持屏幕常亮开关=关，WIFI 未连接，屏幕等待五秒后关闭，电源键可切换屏幕开关
  if (delayToTurnOffScreen && !config.keepScreenOn && millis() - lastActiveTime > 5000) {
    turnOnScreen = false;
    delayToTurnOffScreen = false;
  }

  if (turnOnScreen) {
    StickCP2.Display.wakeup();
    if (!onCountdown) drawInfoScreen();
  } else {
    StickCP2.Display.sleep();
  }
}

// 初始化屏幕
void initDisplay() {
  StickCP2.Display.setRotation(1);  // 调整屏幕方向
  StickCP2.Display.setBrightness(15);
  StickCP2.Display.setTextColor(textColor);
  StickCP2.Display.setFont(&efontCN_16);
}

// 初始化
void setup() {
  pinMode(BTN_PWR_PIN, INPUT);
  auto cfg = M5.config();
  StickCP2.begin(cfg);
  loadConfig();
  initDisplay();
  setupWiFi();
  // 设置连接超时，避免请求不通时长时间阻塞
  http.setConnectTimeout(500);
  // 设置读取响应内容超时（不关心这个，故设置为 1 ms）
  http.setTimeout(1);
  lastActiveTime = millis();
}

// 主循环
void loop() {
  StickCP2.update();

  // 屏幕开关处理
  handleScreenSwitch();

  // 按键处理
  handleNormalBtnEvent(StickCP2.BtnA, config.aBtnClickAction, config.aBtnHoldAction);
  handleNormalBtnEvent(StickCP2.BtnB, config.bBtnClickAction, config.bBtnHoldAction);
  handlePWRBtnEvent();

  // 自动关机处理
  handleAutoShutdown();

  server.handleClient();
}