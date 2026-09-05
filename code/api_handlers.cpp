#include "api_handlers.h"

#include "auth.h"
#include "config.h"
#include "esim_manager.h"
#include "modem.h"
#include "operator_manager.h"
#include "push.h"
#include "sim_manager.h"
#include "sms_store.h"
#include "web_handlers.h"

namespace {

String jsonEscapeApi(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    unsigned char c = input[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

void sendJson(int status, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(status, "application/json", json);
}

bool validLength(const String &value, size_t maxLength) {
  return value.length() <= maxLength;
}

void handleApiStatus() {
  if (!authRequire()) return;
  int enabledPush = 0;
  for (int i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    if (config.pushChannels[i].enabled) enabledPush++;
  }
  String activeProfile = esimActiveProfileLabel();
  String networkOperator = operatorCurrentLabel();
  if (!simManagerIsPresent()) {
    activeProfile = "";
    networkOperator = "";
  }
  bool signalKnown = simManagerIsPresent() && simManagerSignalKnown();
  bool rsrqKnown = signalKnown && simManagerSignalRsrqKnown();
  String rsrpJson = signalKnown ? String(simManagerSignalRsrpDbm()) : String("null");
  String rsrpRawJson = signalKnown ? String(simManagerSignalRsrpRaw()) : String("null");
  String rsrqJson = rsrqKnown
                        ? String(simManagerSignalRsrqTenthsDb() / 10.0f, 1)
                        : String("null");
  unsigned long signalUpdatedAt = signalKnown ? simManagerSignalUpdatedAt() : 0;
  unsigned long signalAge = signalKnown ? millis() - signalUpdatedAt : 0;
  String json;
  json.reserve(1080);
  json = "{\"ok\":true,\"uptime\":" + String(millis() / 1000) +
         ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"epoch\":" + String(static_cast<unsigned long>(time(nullptr))) +
         ",\"wifi\":{\"connected\":" + String(WiFi.isConnected() ? "true" : "false") +
         ",\"ssid\":\"" + jsonEscapeApi(WiFi.SSID()) + "\",\"rssi\":" + String(WiFi.RSSI()) +
         ",\"ip\":\"" + WiFi.localIP().toString() + "\"},\"modem\":{\"ready\":" +
         String(modemReady ? "true" : "false") + ",\"model\":\"ML307R\",\"operator\":\"" +
         jsonEscapeApi(networkOperator) + "\",\"busy\":" +
         String(modemIsBusy() ? "true" : "false") +
         ",\"registration\":\"" + String(modemReady ? "已注册" : "未注册") +
         "\",\"rsrp\":" + rsrpJson + ",\"rsrq\":" + rsrqJson +
         ",\"signal\":{\"known\":" + String(signalKnown ? "true" : "false") +
         ",\"metric\":\"RSRP\",\"dbm\":" + rsrpJson + ",\"raw\":" + rsrpRawJson +
         ",\"rsrq\":" + rsrqJson + ",\"updatedAt\":" + String(signalUpdatedAt) +
         ",\"ageMs\":" + String(signalAge) + "}},\"sim\":{\"state\":\"" + simManagerStateName() +
         "\",\"known\":" + String(simManagerIsKnown() ? "true" : "false") +
         ",\"present\":" + String(simManagerIsPresent() ? "true" : "false") +
         ",\"ready\":" + String(simManagerIsReady() ? "true" : "false") +
         ",\"smsReady\":" + String(simManagerSmsReady() ? "true" : "false") +
         ",\"message\":\"" + jsonEscapeApi(simManagerMessage()) +
         "\",\"generation\":" + String(simManagerGeneration()) +
         ",\"changedAt\":" + String(simManagerChangedAt()) + ",\"profile\":\"" +
         jsonEscapeApi(activeProfile) + "\",\"name\":\"" + jsonEscapeApi(activeProfile) +
         "\",\"profileName\":\"" + jsonEscapeApi(activeProfile) + "\"},\"sms\":{\"stored\":" + String(smsStoreCount()) +
         ",\"unread\":" + String(smsStoreUnread()) + ",\"capacity\":50},\"push\":{\"enabled\":" +
         String(enabledPush) + "},\"job\":" + esimJobJson() + "}";
  sendJson(200, json);
}

void handleApiSmsList() {
  if (!authRequire()) return;
  String messages = smsStoreListJson();
  String json = "{\"ok\":true,\"count\":" + String(smsStoreCount()) + ",\"unread\":" +
                String(smsStoreUnread()) + ",\"capacity\":50,\"messages\":" + messages + "}";
  sendJson(200, json);
}

uint32_t requestId() {
  String value = server.arg("id");
  if (!value.length() || value.length() > 10) return 0;
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isDigit(value[i])) return 0;
  }
  return static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
}

void handleApiSmsRead() {
  if (!authRequireCsrf()) return;
  uint32_t id = requestId();
  if (!id || !smsStoreMarkRead(id)) {
    sendJson(404, "{\"ok\":false,\"error\":\"not_found\"}");
    return;
  }
  sendJson(200, "{\"ok\":true}");
}

void handleApiSmsDelete() {
  if (!authRequireCsrf()) return;
  uint32_t id = requestId();
  if (!id || !smsStoreDelete(id)) {
    sendJson(404, "{\"ok\":false,\"error\":\"not_found\"}");
    return;
  }
  sendJson(200, "{\"ok\":true}");
}

void handleApiSmsClear() {
  if (!authRequireCsrf()) return;
  if (!smsStoreClear()) {
    sendJson(500, "{\"ok\":false,\"error\":\"storage\"}");
    return;
  }
  sendJson(200, "{\"ok\":true}");
}

void handleApiEsimProfiles() {
  if (!authRequire()) return;
  if (!simManagerIsReady()) {
    sendJson(200, esimProfilesJson());
    return;
  }
  if (modemIsBusy()) {
    if (esimProfilesLoaded()) {
      sendJson(200, esimProfilesJson());
      return;
    }
    sendJson(503, "{\"ok\":false,\"error\":\"modem_busy\",\"message\":\"模组正在初始化，请稍后重试\"}");
    return;
  }
  if (!esimProfilesLoaded() && !esimIsBusy()) {
    String error;
    if (!esimRefreshProfiles(error)) {
      sendJson(503, "{\"ok\":false,\"error\":\"esim\",\"message\":\"" + jsonEscapeApi(error) + "\"}");
      return;
    }
  }
  sendJson(200, esimProfilesJson());
}

void handleApiEsimRefresh() {
  if (!authRequireCsrf()) return;
  if (!simManagerIsReady()) {
    sendJson(409, "{\"ok\":false,\"error\":\"sim_not_ready\",\"message\":\"SIM 尚未就绪\"}");
    return;
  }
  if (modemIsBusy()) {
    sendJson(409, "{\"ok\":false,\"error\":\"modem_busy\",\"message\":\"模组正在处理其他通信，请稍后重试\"}");
    return;
  }
  String error;
  if (!esimRefreshProfiles(error)) {
    sendJson(503, "{\"ok\":false,\"error\":\"esim\",\"message\":\"" + jsonEscapeApi(error) + "\"}");
    return;
  }
  sendJson(200, esimProfilesJson());
}

void handleApiEsimSwitch() {
  if (!authRequireCsrf()) return;
  if (!simManagerIsReady()) {
    sendJson(409, "{\"ok\":false,\"error\":\"sim_not_ready\",\"message\":\"SIM 尚未就绪\"}");
    return;
  }
  if (modemIsBusy()) {
    sendJson(409, "{\"ok\":false,\"error\":\"modem_busy\",\"message\":\"模组正在处理其他通信，请稍后重试\"}");
    return;
  }
  String id = server.arg("id");
  if (id.length() != 32) {
    sendJson(400, "{\"ok\":false,\"error\":\"invalid_profile\"}");
    return;
  }
  String message;
  if (!esimStartSwitch(id, message)) {
    sendJson(esimIsBusy() ? 409 : 400,
             "{\"ok\":false,\"message\":\"" + jsonEscapeApi(message) + "\"}");
    return;
  }
  sendJson(202, "{\"ok\":true,\"message\":\"" + jsonEscapeApi(message) + "\",\"job\":" + esimJobJson() + "}");
}

void handleApiEsimEid() {
  if (!authRequire()) return;
  if (!simManagerIsReady()) {
    sendJson(200, "{\"ok\":true,\"eid\":\"\"}");
    return;
  }
  if (modemIsBusy()) {
    sendJson(409, "{\"ok\":false,\"error\":\"modem_busy\",\"message\":\"模组正在处理其他通信，请稍后重试\"}");
    return;
  }
  String error;
  String eid = esimGetEid(error);
  if (!eid.length()) {
    sendJson(503, "{\"ok\":false,\"error\":\"esim\",\"message\":\"" + jsonEscapeApi(error) + "\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"eid\":\"" + jsonEscapeApi(eid) + "\"}");
}

void handleApiEsimDelete() {
  if (!authRequireCsrf()) return;
  if (!simManagerIsReady()) {
    sendJson(409, "{\"ok\":false,\"error\":\"sim_not_ready\",\"message\":\"SIM 尚未就绪\"}");
    return;
  }
  if (modemIsBusy()) {
    sendJson(409, "{\"ok\":false,\"error\":\"modem_busy\",\"message\":\"模组正在处理其他通信，请稍后重试\"}");
    return;
  }
  String id = server.arg("id");
  if (id.length() != 32) {
    sendJson(400, "{\"ok\":false,\"error\":\"invalid_profile\"}");
    return;
  }
  String message;
  if (!esimDeleteProfile(id, message)) {
    sendJson(400, "{\"ok\":false,\"message\":\"" + jsonEscapeApi(message) + "\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"message\":\"Profile 已删除\"}");
}

void handleApiOperatorGet() {
  if (!authRequire()) return;
  sendJson(200, operatorManagerJson());
}

void handleApiOperatorScan() {
  if (!authRequireCsrf()) return;
  String message;
  if (!operatorManagerStartScan(message)) {
    sendJson(409, "{\"ok\":false,\"message\":\"" + jsonEscapeApi(message) + "\"}");
    return;
  }
  sendJson(202, operatorManagerJson());
}

void handleApiOperatorSelect() {
  if (!authRequireCsrf()) return;
  String numeric = server.arg("numeric");
  numeric.trim();
  String actText = server.arg("act");
  if (!actText.length() || actText.length() > 2) {
    sendJson(400, "{\"ok\":false,\"message\":\"网络制式参数无效\"}");
    return;
  }
  for (size_t i = 0; i < actText.length(); ++i) {
    if (!isDigit(actText[i])) {
      sendJson(400, "{\"ok\":false,\"message\":\"网络制式参数无效\"}");
      return;
    }
  }
  String message;
  if (!operatorManagerStartSelect(numeric, actText.toInt(), message)) {
    sendJson(409, "{\"ok\":false,\"message\":\"" + jsonEscapeApi(message) + "\"}");
    return;
  }
  sendJson(202, operatorManagerJson());
}

void handleApiOperatorAuto() {
  if (!authRequireCsrf()) return;
  String message;
  if (!operatorManagerStartAuto(message)) {
    sendJson(409, "{\"ok\":false,\"message\":\"" + jsonEscapeApi(message) + "\"}");
    return;
  }
  sendJson(202, operatorManagerJson());
}

void handleApiWifiPost() {
  if (!authRequireCsrf()) return;
  if (server.hasArg("clearAll")) {
    for (int i = 0; i < WIFI_NETS_MAX; ++i) {
      config.wifiNets[i].ssid = "";
      config.wifiNets[i].pass = "";
    }
    saveConfig();
    sendJson(200, "{\"ok\":true,\"message\":\"已清空全部WiFi，重启后设备开启配置热点 sms-forwarder\"}");
    delay(800);
    ESP.restart();
    return;
  }
  bool anySsid = false;
  WifiNet updated[WIFI_NETS_MAX];
  for (int i = 0; i < WIFI_NETS_MAX; ++i) {
    String ssid = server.arg("wifiSsid" + String(i));
    String pass = server.arg("wifiPass" + String(i));
    ssid.trim();
    if (ssid.length() > 32 || pass.length() > 64) {
      sendJson(400, "{\"ok\":false,\"message\":\"WiFi 名称或密码长度无效\"}");
      return;
    }
    if (pass.length() == 0 && ssid.length() > 0) {
      for (int k = 0; k < WIFI_NETS_MAX; ++k) {
        if (config.wifiNets[k].ssid == ssid) {
          pass = config.wifiNets[k].pass;  // 同名网络未填密码时保留旧密码
          break;
        }
      }
    }
    if (ssid.length() > 0) anySsid = true;
    updated[i].ssid = ssid;
    updated[i].pass = pass;
  }
  if (!anySsid) {
    sendJson(400, "{\"ok\":false,\"message\":\"至少需要填写一个 WiFi 名称\"}");
    return;
  }
  for (int i = 0; i < WIFI_NETS_MAX; ++i) config.wifiNets[i] = updated[i];
  saveConfig();
  sendJson(200, "{\"ok\":true,\"message\":\"WiFi 已保存，设备即将重启并按主备顺序连接\"}");
  delay(800);
  ESP.restart();
}

void handleApiReboot() {
  if (!authRequireCsrf()) return;
  sendJson(200, "{\"ok\":true,\"message\":\"设备正在重启，约 40 秒后恢复\"}");
  delay(800);
  ESP.restart();
}

void handleApiPushTest() {
  if (!authRequireCsrf()) return;
  if (WiFi.status() != WL_CONNECTED) {
    sendJson(409, "{\"ok\":false,\"message\":\"WiFi 未连接\"}");
    return;
  }
  String timestamp = String(time(nullptr));
  int sent = 0;
  for (int i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    if (isPushChannelValid(config.pushChannels[i])) {
      sendToChannel(config.pushChannels[i], "测试", "推送通道测试消息", timestamp.c_str());
      ++sent;
    }
  }
  bool emailOk = config.smtpServer.length() > 0 && config.smtpUser.length() > 0 &&
                 config.smtpPass.length() > 0 && config.smtpSendTo.length() > 0;
  if (emailOk) {
    sendEmailNotification("推送测试", "这是一条来自设备的推送通道测试邮件");
    ++sent;
  }
  if (sent == 0) {
    sendJson(400, "{\"ok\":false,\"message\":\"没有已启用的推送通道或邮件配置\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"message\":\"已发送测试消息（" + String(sent) + " 个通道），请查收\"}");
}

void handleApiFactoryReset() {
  if (!authRequireCsrf()) return;
  sendJson(200, "{\"ok\":true,\"message\":\"正在恢复出厂设置，设备即将重启并开启配置热点\"}");
  delay(800);
  preferences.begin("sms_config", false);
  preferences.clear();
  preferences.end();
  smsStoreClear();
  ESP.restart();
}

// 公开的品牌信息(仅展示用途,不含敏感数据)
void handleApiBrand() {
  sendJson(200, "{\"ok\":true,\"title\":\"" + jsonEscapeApi(config.brandTitle) +
                    "\",\"sub\":\"" + jsonEscapeApi(config.brandSub) + "\"}");
}

void handleApiConfigGet() {
  if (!authRequire()) return;
  String json;
  json.reserve(1800);
  json = "{\"ok\":true,\"webUser\":\"" + jsonEscapeApi(config.webUser) +
         "\",\"mustChangePassword\":" + String(config.webPass == DEFAULT_WEB_PASS ? "true" : "false") +
         ",\"adminPhone\":\"" + jsonEscapeApi(config.adminPhone) + "\",\"numberBlackList\":\"" +
         jsonEscapeApi(config.numberBlackList) + "\",\"smtp\":{\"server\":\"" +
         jsonEscapeApi(config.smtpServer) + "\",\"port\":" + String(config.smtpPort) +
         ",\"user\":\"" + jsonEscapeApi(config.smtpUser) + "\",\"recipient\":\"" +
         jsonEscapeApi(config.smtpSendTo) + "\",\"passwordSet\":" +
         String(config.smtpPass.length() ? "true" : "false") + "},\"push\":[";
  for (int i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    if (i) json += ',';
    const PushChannel &ch = config.pushChannels[i];
    json += "{\"enabled\":" + String(ch.enabled ? "true" : "false") + ",\"type\":" +
            String(static_cast<int>(ch.type)) + ",\"name\":\"" + jsonEscapeApi(ch.name) +
            "\",\"urlSet\":" + String(ch.url.length() ? "true" : "false") +
            ",\"key1Set\":" + String(ch.key1.length() ? "true" : "false") +
            ",\"key2Set\":" + String(ch.key2.length() ? "true" : "false") +
            ",\"urlConfigured\":" + String(ch.url.length() ? "true" : "false") +
            ",\"key1Configured\":" + String(ch.key1.length() ? "true" : "false") +
             ",\"key2Configured\":" + String(ch.key2.length() ? "true" : "false") +
             ",\"customBody\":\"" + jsonEscapeApi(ch.customBody) + "\"}";
   }
  json += "],\"wifi\":{\"nets\":[";
  for (int i = 0; i < WIFI_NETS_MAX; ++i) {
    if (i) json += ',';
    json += "{\"ssid\":\"" + jsonEscapeApi(config.wifiNets[i].ssid) +
            "\",\"passSet\":" + String(config.wifiNets[i].pass.length() ? "true" : "false") + "}";
  }
  json += "],\"apMode\":" + String(WiFi.getMode() == WIFI_AP ? "true" : "false") +
          "},\"keepaliveDays\":" + String(config.keepaliveDays) +
          ",\"brand\":{\"title\":\"" + jsonEscapeApi(config.brandTitle) +
          "\",\"sub\":\"" + jsonEscapeApi(config.brandSub) + "\"}}";
  sendJson(200, json);
}

void handleApiConfigPost() {
  if (!authRequireCsrf()) return;
  String webUser = server.arg("webUser");
  webUser.trim();
  String webPass = server.arg("webPass");
  String adminPhone = server.arg("adminPhone");
  String blacklist = server.arg("numberBlackList");
  if (!validLength(webUser, 64) || !validLength(webPass, 128) || !validLength(adminPhone, 32) ||
      !validLength(blacklist, 512)) {
    sendJson(400, "{\"ok\":false,\"error\":\"input_too_long\"}");
    return;
  }

  bool credentialsChanged = false;
  if (webUser.length() && webUser != config.webUser) {
    config.webUser = webUser;
    credentialsChanged = true;
  }
  if (webPass.length() && webPass != config.webPass) {
    if (webPass.length() < 8) {
      sendJson(400, "{\"ok\":false,\"error\":\"password_too_short\"}");
      return;
    }
    config.webPass = webPass;
    credentialsChanged = true;
  }
  if (server.hasArg("adminPhone")) config.adminPhone = adminPhone;
  if (server.hasArg("numberBlackList")) config.numberBlackList = blacklist;
  if (server.hasArg("keepaliveDays")) {
    config.keepaliveDays = (uint8_t)constrain(server.arg("keepaliveDays").toInt(), 0, 90);
  }
  if (server.hasArg("brandTitle")) {
    String t = server.arg("brandTitle");
    t.trim();
    if (t.length() > 24) t = t.substring(0, 24);
    config.brandTitle = t;
  }
  if (server.hasArg("brandSub")) {
    String s = server.arg("brandSub");
    s.trim();
    if (s.length() > 40) s = s.substring(0, 40);
    config.brandSub = s;
  }

  if (server.hasArg("smtpServer")) config.smtpServer = server.arg("smtpServer").substring(0, 128);
  if (server.hasArg("smtpPort")) config.smtpPort = constrain(server.arg("smtpPort").toInt(), 1, 65535);
  if (server.hasArg("smtpUser")) config.smtpUser = server.arg("smtpUser").substring(0, 128);
  if (server.hasArg("smtpPass") && server.arg("smtpPass").length()) config.smtpPass = server.arg("smtpPass").substring(0, 192);
  if (server.hasArg("smtpSendTo")) config.smtpSendTo = server.arg("smtpSendTo").substring(0, 128);

  // pushCount 存在时,超出部分视为已删除的通道并清空
  int pushCount = -1;
  if (server.hasArg("pushCount")) {
    pushCount = constrain(server.arg("pushCount").toInt(), 0, MAX_PUSH_CHANNELS);
  }
  for (int i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    String p = "push" + String(i);
    if (pushCount >= 0 && i >= pushCount) {
      config.pushChannels[i] = PushChannel();
      continue;
    }
    bool channelPresent = server.hasArg(p + "en") || server.hasArg(p + "type") ||
                          server.hasArg(p + "name") || server.hasArg(p + "url") ||
                          server.hasArg(p + "k1") || server.hasArg(p + "k2") ||
                          server.hasArg(p + "body");
    if (!channelPresent) continue;
    config.pushChannels[i].enabled = server.hasArg(p + "en");
    if (server.hasArg(p + "type")) config.pushChannels[i].type = static_cast<PushType>(constrain(server.arg(p + "type").toInt(), 0, 10));
    if (server.hasArg(p + "name")) config.pushChannels[i].name = server.arg(p + "name").substring(0, 48);
    if (server.hasArg(p + "url") && server.arg(p + "url").length()) config.pushChannels[i].url = server.arg(p + "url").substring(0, 512);
    if (server.hasArg(p + "k1") && server.arg(p + "k1").length()) config.pushChannels[i].key1 = server.arg(p + "k1").substring(0, 256);
    if (server.hasArg(p + "k2") && server.arg(p + "k2").length()) config.pushChannels[i].key2 = server.arg(p + "k2").substring(0, 256);
    if (server.hasArg(p + "body")) config.pushChannels[i].customBody = server.arg(p + "body").substring(0, 768);
  }

  saveConfig();
  configValid = isConfigValid();
  sendJson(200, "{\"ok\":true,\"reauth\":" + String(credentialsChanged ? "true" : "false") + "}");
  if (credentialsChanged) authInvalidateAll();
}

}  // namespace

void registerApiRoutes() {
  server.on("/api/session", HTTP_GET, handleApiSession);
  server.on("/api/login", HTTP_POST, handleApiLogin);
  server.on("/api/logout", HTTP_POST, handleApiLogout);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/sms", HTTP_GET, handleApiSmsList);
  server.on("/api/sms/read", HTTP_POST, handleApiSmsRead);
  server.on("/api/sms/delete", HTTP_POST, handleApiSmsDelete);
  server.on("/api/sms/clear", HTTP_POST, handleApiSmsClear);
  server.on("/api/esim/profiles", HTTP_GET, handleApiEsimProfiles);
  server.on("/api/esim/refresh", HTTP_POST, handleApiEsimRefresh);
  server.on("/api/esim/switch", HTTP_POST, handleApiEsimSwitch);
  server.on("/api/esim/eid", HTTP_GET, handleApiEsimEid);
  server.on("/api/esim/delete", HTTP_POST, handleApiEsimDelete);
  server.on("/api/operator", HTTP_GET, handleApiOperatorGet);
  server.on("/api/operator/scan", HTTP_POST, handleApiOperatorScan);
  server.on("/api/operator/select", HTTP_POST, handleApiOperatorSelect);
  server.on("/api/operator/auto", HTTP_POST, handleApiOperatorAuto);
  server.on("/api/config", HTTP_GET, handleApiConfigGet);
  server.on("/api/config", HTTP_POST, handleApiConfigPost);
  server.on("/api/wifi", HTTP_POST, handleApiWifiPost);
  server.on("/api/reboot", HTTP_POST, handleApiReboot);
  server.on("/api/push/test", HTTP_POST, handleApiPushTest);
  server.on("/api/factory/reset", HTTP_POST, handleApiFactoryReset);
  server.on("/api/brand", HTTP_GET, handleApiBrand);
}
