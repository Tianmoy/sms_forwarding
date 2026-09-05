#include "globals.h"
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

  // ---- HTTP 服务先行启动,保证任意 WiFi 状态下都可访问管理台 ----
  // WebServer 依赖 WiFi 协议栈的事件队列,必须先初始化 WiFi 再起服务
  // persistent(false): 凭据只由本固件 NVS 管理,防止 esp 自动重连旧网络干扰热点
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
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

  // ---- WiFi:依次尝试主备网络,全部失败则开启配置热点 ----
  if (wifiConnectAll()) {
    logCaptureLn(String("wifi已连接"));
    logCapture(String("IP地址: "));
    logCaptureLn(WiFi.localIP().toString());
    logCapture(String("信号强度(RSSI): "));
    logCaptureLn(String(WiFi.RSSI()) + " dBm");
  } else {
    wifiStartAp();
  }

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
  checkWifiFailover();
  delay(1);
}

// WiFi 断开超过 2 分钟时轮换到下一个已配置网络(仅 STA 模式)
static void checkWifiFailover() {
  static unsigned long wifiDownSince = 0;
  if (WiFi.status() == WL_CONNECTED || WiFi.getMode() != WIFI_STA) {
    wifiDownSince = 0;
    return;
  }
  if (wifiDownSince == 0) {
    wifiDownSince = millis();
    return;
  }
  if (millis() - wifiDownSince < 120000) return;
  logCaptureLn(String("WiFi 持续断开超过 2 分钟，尝试切换到备用网络"));
  wifiDownSince = 0;
  wifiConnectAll();
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
