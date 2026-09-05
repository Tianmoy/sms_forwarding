#include "auth.h"
#include "config.h"

namespace {

constexpr uint8_t MAX_SESSIONS = 2;
constexpr unsigned long IDLE_TIMEOUT_MS = 30UL * 60UL * 1000UL;
constexpr unsigned long ABS_TIMEOUT_MS = 8UL * 60UL * 60UL * 1000UL;
constexpr unsigned long LOGIN_LOCK_MS = 30UL * 1000UL;

struct WebSession {
  bool active = false;
  String token;
  String csrf;
  IPAddress remoteIp;
  unsigned long createdAt = 0;
  unsigned long lastSeenAt = 0;
};

WebSession sessions[MAX_SESSIONS];
uint8_t failedLogins = 0;
unsigned long lockedUntil = 0;

String randomHex(size_t bytes) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; i += 4) {
    uint32_t value = esp_random();
    for (uint8_t j = 0; j < 4 && i + j < bytes; ++j) {
      uint8_t b = (value >> (j * 8)) & 0xff;
      out += hex[b >> 4];
      out += hex[b & 0x0f];
    }
  }
  return out;
}

bool secureEquals(const String &a, const String &b) {
  size_t maxLen = max(a.length(), b.length());
  uint8_t diff = static_cast<uint8_t>(a.length() ^ b.length());
  for (size_t i = 0; i < maxLen; ++i) {
    char ac = i < a.length() ? a[i] : 0;
    char bc = i < b.length() ? b[i] : 0;
    diff |= static_cast<uint8_t>(ac ^ bc);
  }
  return diff == 0;
}

String jsonEscapeAuth(const String &input) {
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

String cookieToken() {
  if (!server.hasHeader("Cookie")) return "";
  String cookie = server.header("Cookie");
  const String key = "sms_session=";
  int start = cookie.indexOf(key);
  if (start < 0) return "";
  start += key.length();
  int end = cookie.indexOf(';', start);
  if (end < 0) end = cookie.length();
  return cookie.substring(start, end);
}

bool isExpired(const WebSession &session) {
  unsigned long now = millis();
  return static_cast<unsigned long>(now - session.lastSeenAt) > IDLE_TIMEOUT_MS ||
         static_cast<unsigned long>(now - session.createdAt) > ABS_TIMEOUT_MS;
}

WebSession *currentSession(bool touch = true) {
  String token = cookieToken();
  if (token.length() != 32) return nullptr;
  IPAddress remote = server.client().remoteIP();
  for (auto &session : sessions) {
    if (!session.active) continue;
    if (isExpired(session)) {
      session.active = false;
      session.token = "";
      session.csrf = "";
      continue;
    }
    if (session.remoteIp != remote || !secureEquals(session.token, token)) continue;
    if (touch) session.lastSeenAt = millis();
    return &session;
  }
  return nullptr;
}

WebSession *allocateSession() {
  for (auto &session : sessions) {
    if (!session.active || isExpired(session)) return &session;
  }
  WebSession *oldest = &sessions[0];
  for (auto &session : sessions) {
    if (static_cast<int32_t>(session.lastSeenAt - oldest->lastSeenAt) < 0) {
      oldest = &session;
    }
  }
  return oldest;
}

void sendJson(int status, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(status, "application/json", json);
}

}  // namespace

void authBegin() {
  static const char *headers[] = {"Cookie", "X-CSRF-Token", "Origin", "Host"};
  server.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
}

bool authIsAuthenticated() {
  return currentSession() != nullptr;
}

bool authRequire() {
  if (currentSession()) return true;
  sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
  return false;
}

bool authRequireCsrf() {
  WebSession *session = currentSession();
  if (!session) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return false;
  }
  String supplied = server.hasHeader("X-CSRF-Token") ? server.header("X-CSRF-Token") : "";
  if (!secureEquals(session->csrf, supplied)) {
    sendJson(403, "{\"ok\":false,\"error\":\"csrf\"}");
    return false;
  }
  return true;
}

void authInvalidateAll() {
  for (auto &session : sessions) {
    session.active = false;
    session.token = "";
    session.csrf = "";
  }
}

void handleApiSession() {
  WebSession *session = currentSession();
  if (!session) {
    sendJson(200, "{\"authenticated\":false}");
    return;
  }
  String json = "{\"authenticated\":true,\"user\":\"" + jsonEscapeAuth(config.webUser) +
                "\",\"csrf\":\"" + session->csrf + "\",\"mustChangePassword\":" +
                String(config.webPass == DEFAULT_WEB_PASS ? "true" : "false") + "}";
  sendJson(200, json);
}

void handleApiLogin() {
  unsigned long now = millis();
  if (lockedUntil && static_cast<long>(lockedUntil - now) > 0) {
    unsigned long retry = (lockedUntil - now + 999) / 1000;
    sendJson(429, "{\"ok\":false,\"error\":\"locked\",\"retryAfter\":" + String(retry) + "}");
    return;
  }

  String username = server.arg("username");
  String password = server.arg("password");
  if (username.length() > 64 || password.length() > 128 ||
      !secureEquals(username, config.webUser) || !secureEquals(password, config.webPass)) {
    failedLogins++;
    if (failedLogins >= 5) {
      failedLogins = 0;
      lockedUntil = now + LOGIN_LOCK_MS;
    }
    sendJson(401, "{\"ok\":false,\"error\":\"invalid_credentials\"}");
    return;
  }

  failedLogins = 0;
  lockedUntil = 0;
  WebSession *session = allocateSession();
  session->active = true;
  session->token = randomHex(16);
  session->csrf = randomHex(12);
  session->remoteIp = server.client().remoteIP();
  session->createdAt = now;
  session->lastSeenAt = now;

  server.sendHeader("Set-Cookie", "sms_session=" + session->token +
                                      "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800");
  sendJson(200, "{\"ok\":true,\"csrf\":\"" + session->csrf +
                    "\",\"mustChangePassword\":" +
                    String(config.webPass == DEFAULT_WEB_PASS ? "true" : "false") + "}");
}

void handleApiLogout() {
  WebSession *session = currentSession(false);
  if (session) {
    session->active = false;
    session->token = "";
    session->csrf = "";
  }
  server.sendHeader("Set-Cookie", "sms_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  sendJson(200, "{\"ok\":true}");
}
