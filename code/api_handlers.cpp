#include "api_handlers.h"

#include "auth.h"
#include "config.h"
#include "esim_manager.h"
#include "modem.h"
#include "operator_manager.h"
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
  json += "]}";
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

  if (server.hasArg("smtpServer")) config.smtpServer = server.arg("smtpServer").substring(0, 128);
  if (server.hasArg("smtpPort")) config.smtpPort = constrain(server.arg("smtpPort").toInt(), 1, 65535);
  if (server.hasArg("smtpUser")) config.smtpUser = server.arg("smtpUser").substring(0, 128);
  if (server.hasArg("smtpPass") && server.arg("smtpPass").length()) config.smtpPass = server.arg("smtpPass").substring(0, 192);
  if (server.hasArg("smtpSendTo")) config.smtpSendTo = server.arg("smtpSendTo").substring(0, 128);

  for (int i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    String p = "push" + String(i);
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
}
