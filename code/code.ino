#include "globals.h"
#include "wifi_config.h"
#include "config.h"
#include "web_handlers.h"
#include "modem.h"
#include "push.h"
#include "sms_process.h"
#include "auth.h"
#include "api_handlers.h"
#include "sms_store.h"
#include "esim_manager.h"
#include "operator_manager.h"
#include "sim_manager.h"

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  // 缩短初始化延时，WiFi连接会处理自己的超时
  delay(200);
  Serial1.setRxBufferSize(MODEM_RX_BUFFER_SIZE);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();
  initConcatBuffer();
  loadConfig();
  configValid = isConfigValid();
  if (!smsStoreBegin()) {
    logCaptureLn(String("⚠️ 短信持久化存储初始化失败"));
  }

  // ---- WiFi 连接 ----
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                    // 保持射频常开,后台响应零延迟
  WiFi.setAutoReconnect(true);             // 断线后自动重连
  WiFi.setScanMethod(WIFI_FAST_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  // 优先使用网页配置的WiFi,失败则回退编译期凭据,保证设备不会失联
  const bool hasRuntimeWifi = config.wifiSsid.length() > 0;
  const char* primarySsid = hasRuntimeWifi ? config.wifiSsid.c_str() : WIFI_SSID;
  const char* primaryPass = hasRuntimeWifi ? config.wifiPass.c_str() : WIFI_PASS;
  WiFi.begin(primarySsid, primaryPass);
  logCaptureLn(String("连接wifi: ") + primarySsid);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    blink_short(200);
  }

  if (WiFi.status() != WL_CONNECTED && hasRuntimeWifi) {
    logCaptureLn(String("⚠️ 网页配置的WiFi连接失败，回退编译期WiFi: ") + WIFI_SSID);
    WiFi.disconnect(true);
    delay(500);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
      blink_short(200);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    logCaptureLn(String("wifi已连接"));
    logCapture(String("IP地址: "));
    logCaptureLn(WiFi.localIP().toString());
    logCapture(String("信号强度(RSSI): "));
    logCaptureLn(String(WiFi.RSSI()) + " dBm");
  } else {
    logCaptureLn(String("⚠️ WiFi连接超时，即将重启重试..."));
    delay(1000);
    ESP.restart();
  }

  authBegin();
  server.on("/", HTTP_GET, handleRoot);
  server.on("/tools", HTTP_GET, handleRoot);
  server.on("/sms", HTTP_GET, handleRoot);
  server.on("/query", HTTP_GET, handleQuery);
  server.on("/at", HTTP_POST, handleATCommand);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/modem", HTTP_POST, handleModem);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/ping", HTTP_POST, handlePing);
  server.on("/flight", HTTP_POST, handleFlightMode);
  server.on("/data", HTTP_POST, handleDataToggle);
  registerApiRoutes();
  server.begin();
  logCaptureLn(String("HTTP服务器已启动"));

  // ---- NTP 时间同步 ----
  logCaptureLn(String("正在同步NTP时间..."));
  configTime(0, 0, "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
  unsigned long ntpStartedAt = millis();
  while (time(nullptr) < 100000 && millis() - ntpStartedAt < 5000) {
    delay(25);
    server.handleClient();
  }
  if (time(nullptr) >= 100000) {
    timeSynced = true;
    logCaptureLn(String("NTP时间同步成功"));
    time_t now = time(nullptr);
    logCapture(String("当前UTC时间戳: "));
    logCaptureLn(String(now));
  } else {
    logCaptureLn(String("NTP时间同步失败，将使用设备时间"));
  }

  ssl_client.setInsecure();
  digitalWrite(LED_BUILTIN, LOW);

  // ---- 启动通知（网页已可用，发邮件不会影响用户访问） ----
  if (configValid) {
    logCaptureLn(String("配置有效，发送启动通知..."));
    String subject = "短信转发器已启动";
    String body = "设备已启动\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }

  // ---- 模组初始化（较慢，但网页已可访问） ----
  modemInit();
  esimManagerBegin();
  simManagerBegin();
  operatorManagerBegin();
}

void loop() {
  // Drain long COPS responses before serving HTTP so the UART ring cannot
  // overflow while a network scan is returning several kilobytes at once.
  operatorManagerLoop();
  // Only drain an already active SIM transaction before HTTP. New periodic
  // probes are started after serving HTTP so the 3-second status poll cannot
  // repeatedly observe a short CPIN transaction as a permanently busy modem.
  if (simManagerIsBusy()) simManagerLoop();
  if (smsStoredMessageIsBusy()) smsStoredMessageLoop();
  server.handleClient();
  static bool configWarningPrinted = false;
  if (!configValid && !configWarningPrinted) {
    configWarningPrinted = true;
    logCaptureLn(String("⚠️ 请访问 " + getDeviceUrl() + " 配置系统参数"));
  }
  checkConcatTimeout();
  checkSmsReceiveTimeout();
  operatorManagerLoop();
  simManagerLoop();
  smsStoredMessageLoop();
  if (!operatorManagerIsBusy() && !simManagerIsBusy() && !smsStoredMessageIsBusy()) {
    esimManagerLoop();
  }
  if (!esimIsBusy() && !operatorManagerIsBusy() && !simManagerIsBusy() &&
      !smsStoredMessageIsBusy()) {
    if (Serial.available()) Serial1.write(Serial.read());
    checkSerial1URC();
  }
  checkKeepalive();
  delay(1);
}

// 自动保号:临时激活数据连接并 Ping,产生真实流量后关闭
static void runKeepalivePing() {
  logCaptureLn(String("自动保号 Ping 开始"));
  sendATCommand("AT+CGACT=1,1", 10000);
  delay(300);
  Serial1.println("AT+MPING=\"8.8.8.8\",10,1");
  unsigned long start = millis();
  while (millis() - start < 16000) {
    while (Serial1.available()) {
      dispatchSerial1Byte(Serial1.read(), false);
    }
    server.handleClient();
    delay(2);
  }
  sendATCommand("AT+CGACT=0,1", 8000);
  logCaptureLn(String("自动保号 Ping 结束"));
}

static void checkKeepalive() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 60000) return;
  lastCheck = millis();
  if (config.keepaliveDays == 0) return;
  time_t now = time(nullptr);
  if (now < 100000) return;  // NTP 尚未同步
  if (!modemReady || esimIsBusy() || operatorManagerIsBusy() ||
      simManagerIsBusy() || smsStoredMessageIsBusy() || modemIsBusy()) {
    return;
  }
  uint32_t day = (uint32_t)(now / 86400);
  uint32_t last = configLastKeepaliveDay();
  if (last != 0 && day - last < config.keepaliveDays) return;
  runKeepalivePing();
  configRecordKeepalive(day);
}
