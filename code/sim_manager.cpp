#include "sim_manager.h"

#include "esim_manager.h"
#include "globals.h"
#include "modem.h"
#include "operator_manager.h"
#include "sms_process.h"
#include "web_handlers.h"

namespace {

constexpr unsigned long DETECT_INTERVAL_READY_MS = 2000UL;
constexpr unsigned long DETECT_INTERVAL_RETRY_MS = 1000UL;
constexpr unsigned long DETECT_CONFIRM_MS = 350UL;
constexpr unsigned long COMMAND_TIMEOUT_MS = 2500UL;
constexpr unsigned long CGACT_TIMEOUT_MS = 5000UL;
constexpr unsigned long SIGNAL_INTERVAL_MS = 10000UL;
constexpr unsigned long SIGNAL_STALE_MS = 45000UL;
constexpr size_t RESPONSE_CAPACITY = 640;
constexpr uint8_t CONFIRMATION_COUNT = 2;
constexpr uint8_t MAX_CONFIG_ATTEMPTS = 3;

enum SimState {
  SIM_UNKNOWN,
  SIM_DETECTING,
  SIM_ABSENT,
  SIM_PIN_REQUIRED,
  SIM_PUK_REQUIRED,
  SIM_READY,
  SIM_ERROR
};

enum Observation {
  OBS_UNKNOWN,
  OBS_DETECTING,
  OBS_ABSENT,
  OBS_PIN_REQUIRED,
  OBS_PUK_REQUIRED,
  OBS_READY,
  OBS_ERROR
};

enum WireStage {
  WIRE_IDLE,
  WIRE_CPIN,
  WIRE_CMEE,
  WIRE_CGACT,
  WIRE_ICCID,
  WIRE_CNMI,
  WIRE_CMGF,
  WIRE_CEREG_ENABLE,
  WIRE_CEREG_QUERY,
  WIRE_CEREG_PROBE,
  WIRE_CESQ
};

SimState state = SIM_UNKNOWN;
Observation candidate = OBS_UNKNOWN;
uint8_t candidateCount = 0;
WireStage wireStage = WIRE_IDLE;
bool known = false;
bool present = false;
bool smsReady = false;
bool needsConfigure = false;
bool ownsExclusive = false;
bool probeRegistrationNext = false;
uint8_t configAttempts = 0;
uint8_t transportFailures = 0;
uint32_t generation = 0;
unsigned long changedAt = 0;
unsigned long nextActionAt = 0;
unsigned long commandStartedAt = 0;
unsigned long commandTimeout = COMMAND_TIMEOUT_MS;
char responseBuffer[RESPONSE_CAPACITY];
size_t responseLength = 0;
String activeIccidTail;
bool signalKnown = false;
bool signalRsrqKnown = false;
int signalRsrpDbm = 0;
int signalRsrqTenthsDb = 0;
int signalRsrpRaw = -1;
int signalRsrqRaw = -1;
unsigned long signalUpdatedAt = 0;
unsigned long signalNextAt = 0;

bool elapsed(unsigned long now, unsigned long deadline) {
  return static_cast<long>(now - deadline) >= 0;
}

const char* stateName(SimState value) {
  switch (value) {
    case SIM_DETECTING: return "detecting";
    case SIM_ABSENT: return "absent";
    case SIM_PIN_REQUIRED: return "pin_required";
    case SIM_PUK_REQUIRED: return "puk_required";
    case SIM_READY: return "ready";
    case SIM_ERROR: return "error";
    default: return "unknown";
  }
}

const char* stateMessage(SimState value) {
  switch (value) {
    case SIM_DETECTING:
      return present ? "SIM 已识别，正在恢复短信服务" : "正在检测 SIM 卡";
    case SIM_ABSENT: return "未检测到 SIM 卡";
    case SIM_PIN_REQUIRED: return "SIM 卡需要 PIN 解锁";
    case SIM_PUK_REQUIRED: return "SIM 卡需要 PUK 解锁";
    case SIM_READY: return smsReady ? "SIM 已就绪" : "SIM 已识别，短信服务尚未就绪";
    case SIM_ERROR: return "SIM 卡状态异常";
    default: return "SIM 状态尚未确认";
  }
}

void publishState(SimState next, bool nextKnown, bool nextPresent) {
  bool changed = state != next || known != nextKnown || present != nextPresent;
  state = next;
  known = nextKnown;
  present = nextPresent;
  if (!changed) return;
  ++generation;
  changedAt = millis();
  logCaptureLn(String("SIM 状态更新：") + stateMessage(state));
}

void releaseWire() {
  if (ownsExclusive) modemReleaseExclusive();
  ownsExclusive = false;
  wireStage = WIRE_IDLE;
  responseLength = 0;
  memset(responseBuffer, 0, sizeof(responseBuffer));
}

bool lineEquals(const char* expected) {
  size_t start = 0;
  while (start < responseLength) {
    size_t end = start;
    while (end < responseLength && responseBuffer[end] != '\n') ++end;
    while (start < end && (responseBuffer[start] == '\r' || responseBuffer[start] == ' ')) ++start;
    while (end > start && (responseBuffer[end - 1] == '\r' || responseBuffer[end - 1] == ' ')) --end;
    size_t expectedLength = strlen(expected);
    if (end - start == expectedLength &&
        memcmp(responseBuffer + start, expected, expectedLength) == 0) return true;
    start = end + 1;
  }
  return false;
}

bool responseHas(const char* value) {
  return strstr(responseBuffer, value) != nullptr;
}

bool responseError() {
  return lineEquals("ERROR") || responseHas("+CME ERROR:") || responseHas("+CMS ERROR:");
}

int cmeErrorCode() {
  const char* marker = strstr(responseBuffer, "+CME ERROR:");
  if (!marker) return -1;
  marker += strlen("+CME ERROR:");
  while (*marker == ' ') ++marker;
  if (*marker < '0' || *marker > '9') return -1;
  return atoi(marker);
}

Observation parseCpinObservation() {
  if (responseHas("+CPIN: READY")) return OBS_READY;
  if (responseHas("+CPIN: SIM PIN") || responseHas("SIM PIN required")) return OBS_PIN_REQUIRED;
  if (responseHas("+CPIN: SIM PUK") || responseHas("SIM PUK required")) return OBS_PUK_REQUIRED;
  int cme = cmeErrorCode();
  if (cme == 10 || responseHas("SIM not inserted")) return OBS_ABSENT;
  if (cme == 11) return OBS_PIN_REQUIRED;
  if (cme == 12) return OBS_PUK_REQUIRED;
  if (cme == 14 || cme == 512 || responseHas("SIM busy") || responseHas("SIM not ready")) {
    return OBS_DETECTING;
  }
  if (cme == 13 || cme == 15 || responseHas("SIM failure") || responseHas("SIM wrong")) {
    return OBS_ERROR;
  }
  // SIM busy/not ready and a missing terminal response are transient. They must
  // never be promoted to a false card-removal event.
  return OBS_UNKNOWN;
}

int parseCeregStatus() {
  int queryStatus = -1;
  int urcStatus = -1;
  const char* search = responseBuffer;
  while (const char* marker = strstr(search, "+CEREG:")) {
    marker += strlen("+CEREG:");
    while (*marker == ' ') ++marker;
    char* end = nullptr;
    long first = strtol(marker, &end, 10);
    if (end != marker && first >= 0 && first <= 5) {
      while (*end == ' ') ++end;
      if (*end != ',') {
        urcStatus = static_cast<int>(first);
      } else {
        const char* secondStart = end + 1;
        while (*secondStart == ' ') ++secondStart;
        // Query responses are <n>,<stat>; extended unsolicited reports are
        // <stat>,"<tac>" and therefore have a quote after the first comma.
        if (*secondStart == '"') {
          urcStatus = static_cast<int>(first);
        } else {
          char* secondEnd = nullptr;
          long second = strtol(secondStart, &secondEnd, 10);
          if (secondEnd != secondStart && second >= 0 && second <= 5) {
            queryStatus = static_cast<int>(second);
          }
        }
      }
    }
    const char* lineEnd = strchr(marker, '\n');
    if (!lineEnd) break;
    search = lineEnd + 1;
  }
  return queryStatus >= 0 ? queryStatus : urcStatus;
}

String parseIccidTail(const char* response) {
  if (!response) return "";
  const char* marker = strstr(response, "+ICCID:");
  if (!marker) return "";
  marker += strlen("+ICCID:");
  const char* lineEnd = marker;
  while (*lineEnd && *lineEnd != '\r' && *lineEnd != '\n') ++lineEnd;
  while (marker < lineEnd && (*marker == ' ' || *marker == '\t')) ++marker;
  while (lineEnd > marker &&
         (lineEnd[-1] == ' ' || lineEnd[-1] == '\t')) --lineEnd;
  if (marker < lineEnd && *marker == '"') ++marker;
  if (lineEnd > marker && lineEnd[-1] == '"') --lineEnd;
  // ML307R may append the ISO/IEC 7816 filler nibble used for an odd-length
  // ICCID (for example 19 decimal digits followed by F).
  if (lineEnd > marker && (lineEnd[-1] == 'F' || lineEnd[-1] == 'f')) --lineEnd;
  const size_t length = static_cast<size_t>(lineEnd - marker);
  if (length < 18 || length > 22) return "";
  for (const char* p = marker; p < lineEnd; ++p) {
    if (!isDigit(*p)) return "";
  }
  String tail;
  tail.reserve(4);
  for (const char* p = lineEnd - 4; p < lineEnd; ++p) tail += *p;
  return tail;
}

void clearSignalCache() {
  signalKnown = false;
  signalRsrqKnown = false;
  signalRsrpDbm = 0;
  signalRsrqTenthsDb = 0;
  signalRsrpRaw = -1;
  signalRsrqRaw = -1;
  signalUpdatedAt = 0;
  signalNextAt = millis();
}

bool updateSignalFromCesq() {
  const char* marker = strstr(responseBuffer, "+CESQ:");
  if (!marker) return false;
  int rxlev = 0;
  int ber = 0;
  int rscp = 0;
  int ecno = 0;
  int rsrq = 0;
  int rsrp = 0;
  if (sscanf(marker, "+CESQ: %d,%d,%d,%d,%d,%d",
             &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp) != 6) {
    return false;
  }

  signalRsrpRaw = rsrp;
  signalRsrqRaw = rsrq;
  signalKnown = rsrp >= 0 && rsrp <= 97;
  signalRsrqKnown = rsrq >= 0 && rsrq <= 34;
  if (signalKnown) signalRsrpDbm = -140 + rsrp;
  if (signalRsrqKnown) signalRsrqTenthsDb = -195 + rsrq * 5;
  signalUpdatedAt = millis();
  return true;
}

void invalidateCardCaches() {
  modemReady = false;
  smsReady = false;
  probeRegistrationNext = false;
  activeIccidTail = "";
  clearSignalCache();
  esimManagerInvalidateProfiles();
  operatorManagerInvalidate();
  smsResetForSimChange();
}

void commitObservation(Observation observation) {
  candidate = OBS_UNKNOWN;
  candidateCount = 0;
  transportFailures = 0;

  switch (observation) {
    case OBS_DETECTING:
      needsConfigure = false;
      configAttempts = 0;
      modemReady = false;
      smsReady = false;
      publishState(SIM_DETECTING, true, true);
      nextActionAt = millis() + DETECT_INTERVAL_RETRY_MS;
      break;
    case OBS_ABSENT:
      needsConfigure = false;
      configAttempts = 0;
      if (state != SIM_ABSENT || present) invalidateCardCaches();
      publishState(SIM_ABSENT, true, false);
      nextActionAt = millis() + DETECT_INTERVAL_RETRY_MS;
      break;
    case OBS_PIN_REQUIRED:
      needsConfigure = false;
      configAttempts = 0;
      if (state != SIM_PIN_REQUIRED || !present) invalidateCardCaches();
      publishState(SIM_PIN_REQUIRED, true, true);
      nextActionAt = millis() + DETECT_INTERVAL_RETRY_MS;
      break;
    case OBS_PUK_REQUIRED:
      needsConfigure = false;
      configAttempts = 0;
      if (state != SIM_PUK_REQUIRED || !present) invalidateCardCaches();
      publishState(SIM_PUK_REQUIRED, true, true);
      nextActionAt = millis() + DETECT_INTERVAL_RETRY_MS;
      break;
    case OBS_ERROR:
      needsConfigure = false;
      configAttempts = 0;
      if (state != SIM_ERROR || !present) invalidateCardCaches();
      publishState(SIM_ERROR, true, true);
      nextActionAt = millis() + DETECT_INTERVAL_RETRY_MS;
      break;
    case OBS_READY: {
      bool newlyReady = state != SIM_READY || !present || !smsReady;
      if (newlyReady) {
        modemReady = false;
        smsReady = false;
        esimManagerInvalidateProfiles();
        operatorManagerInvalidate();
        needsConfigure = true;
        configAttempts = 0;
        publishState(SIM_DETECTING, true, true);
        nextActionAt = millis();
      } else {
        nextActionAt = millis() + DETECT_INTERVAL_READY_MS;
      }
      break;
    }
    default:
      nextActionAt = millis() + DETECT_INTERVAL_RETRY_MS;
      break;
  }
}

void observe(Observation observation) {
  if (observation == OBS_UNKNOWN) {
    candidate = OBS_UNKNOWN;
    candidateCount = 0;
    if (++transportFailures >= 3 && state != SIM_ABSENT) {
      modemReady = false;
      smsReady = false;
      probeRegistrationNext = false;
      publishState(SIM_UNKNOWN, false, false);
    }
    nextActionAt = millis() + DETECT_INTERVAL_RETRY_MS;
    return;
  }

  bool matchesPublished =
      (observation == OBS_DETECTING && state == SIM_DETECTING && !needsConfigure) ||
      (observation == OBS_ABSENT && state == SIM_ABSENT) ||
      (observation == OBS_PIN_REQUIRED && state == SIM_PIN_REQUIRED) ||
      (observation == OBS_PUK_REQUIRED && state == SIM_PUK_REQUIRED) ||
      (observation == OBS_READY && state == SIM_READY && smsReady) ||
      (observation == OBS_ERROR && state == SIM_ERROR && !needsConfigure);
  if (matchesPublished) {
    candidate = OBS_UNKNOWN;
    candidateCount = 0;
    transportFailures = 0;
    probeRegistrationNext = observation == OBS_READY && !modemReady;
    nextActionAt = millis() + (probeRegistrationNext ? DETECT_INTERVAL_RETRY_MS
                                                      : (observation == OBS_READY
                                                             ? DETECT_INTERVAL_READY_MS
                                                             : DETECT_INTERVAL_RETRY_MS));
    return;
  }

  if (candidate != observation) {
    candidate = observation;
    candidateCount = 1;
    // A first definitive change immediately withdraws READY and invalidates
    // card-derived caches. The second sample still decides the final state,
    // but a quick remove/reinsert can no longer leave stale Profile data live.
    if (state != SIM_UNKNOWN && state != SIM_DETECTING) {
      invalidateCardCaches();
      needsConfigure = false;
      configAttempts = 0;
      publishState(SIM_DETECTING, false, observation != OBS_ABSENT);
    }
  } else if (candidateCount < 255) {
    ++candidateCount;
  }
  if (candidateCount >= CONFIRMATION_COUNT) commitObservation(observation);
  else nextActionAt = millis() + DETECT_CONFIRM_MS;
}

bool startWire(const char* command, WireStage stage, unsigned long timeout = COMMAND_TIMEOUT_MS) {
  if (wireStage != WIRE_IDLE || esimIsBusy() || operatorManagerIsBusy() ||
      smsReceiverAwaitingPdu()) return false;
  if (!modemAcquireExclusive()) return false;
  ownsExclusive = true;
  // Drain complete URCs before starting a transaction. If a +CMT header was
  // observed without its PDU, release the modem and let the PDU finish first.
  checkSerial1URC();
  if (smsReceiverAwaitingPdu()) {
    releaseWire();
    nextActionAt = millis() + 100;
    return false;
  }
  responseLength = 0;
  responseBuffer[0] = '\0';
  wireStage = stage;
  commandStartedAt = millis();
  commandTimeout = timeout;
  Serial1.println(command);
  return true;
}

void scheduleConfigurationRetry() {
  smsReady = false;
  needsConfigure = true;
  if (configAttempts < 255) ++configAttempts;
  if (configAttempts >= MAX_CONFIG_ATTEMPTS) {
    publishState(SIM_ERROR, true, true);
  } else {
    publishState(SIM_DETECTING, true, true);
  }
  unsigned long retryDelay = 1000UL * (configAttempts ? configAttempts : 1);
  if (retryDelay > 5000UL) retryDelay = 5000UL;
  nextActionAt = millis() + retryDelay;
}

void handleWireResult(WireStage completed, bool ok) {
  if (completed == WIRE_CPIN) {
    Observation observation = parseCpinObservation();
    releaseWire();
    observe(observation);
    return;
  }

  if (completed == WIRE_CEREG_PROBE) {
    int registration = ok ? parseCeregStatus() : -1;
    releaseWire();
    if (registration >= 0) modemReady = registration == 1 || registration == 5;
    if (modemReady) signalNextAt = millis();
    probeRegistrationNext = false;
    nextActionAt = millis() + DETECT_INTERVAL_READY_MS;
    return;
  }

  if (completed == WIRE_CESQ) {
    bool parsed = ok && updateSignalFromCesq();
    releaseWire();
    if (ok && !parsed) {
      signalKnown = false;
      signalRsrqKnown = false;
    }
    signalNextAt = millis() + SIGNAL_INTERVAL_MS;
    return;
  }

  if (!ok) {
    Observation observation = parseCpinObservation();
    releaseWire();
    if (observation == OBS_ABSENT || observation == OBS_PIN_REQUIRED ||
        observation == OBS_PUK_REQUIRED) {
      // A configuration command can reveal a card-state change, but still use
      // the normal two-sample debounce before publishing it.
      needsConfigure = false;
      observe(observation);
    } else if (completed == WIRE_CGACT) {
      // Disabling the PDP context is a safety best effort. SMS reception does
      // not depend on it, so continue restoring the mandatory settings.
      startWire("AT+ICCID", WIRE_ICCID);
    } else if (completed == WIRE_ICCID) {
      // ICCID is receiver metadata only. Never block SMS recovery when a
      // modem temporarily refuses this optional query.
      activeIccidTail = "";
      startWire("AT+CNMI=2,2,0,0,0", WIRE_CNMI);
    } else if (completed == WIRE_CMEE) {
      // During insertion recovery CMEE is best effort and configuration may
      // continue. During ordinary detection, retry CMEE later so a modem that
      // currently returns bare ERROR can recover numeric SIM diagnostics.
      if (needsConfigure) {
        startWire("AT+ICCID", WIRE_ICCID);
      } else {
        nextActionAt = millis() + DETECT_INTERVAL_RETRY_MS;
      }
    } else {
      scheduleConfigurationRetry();
    }
    return;
  }

  int registration = completed == WIRE_CEREG_QUERY ? parseCeregStatus() : -1;
  String iccidTail = completed == WIRE_ICCID ? parseIccidTail(responseBuffer) : "";
  releaseWire();
  switch (completed) {
    case WIRE_CMEE:
      if (needsConfigure) {
        startWire("AT+ICCID", WIRE_ICCID);
      } else {
        transportFailures = 0;
        nextActionAt = millis();
      }
      break;
    case WIRE_CGACT:
      startWire("AT+ICCID", WIRE_ICCID);
      break;
    case WIRE_ICCID:
      activeIccidTail = iccidTail;
      startWire("AT+CNMI=2,2,0,0,0", WIRE_CNMI);
      break;
    case WIRE_CNMI:
      startWire("AT+CMGF=0", WIRE_CMGF);
      break;
    case WIRE_CMGF:
      startWire("AT+CEREG=2", WIRE_CEREG_ENABLE);
      break;
    case WIRE_CEREG_ENABLE:
      startWire("AT+CEREG?", WIRE_CEREG_QUERY);
      break;
    case WIRE_CEREG_QUERY: {
      modemReady = registration == 1 || registration == 5;
      probeRegistrationNext = false;
      smsReady = true;
      needsConfigure = false;
      configAttempts = 0;
      publishState(SIM_READY, true, true);
      signalNextAt = millis();
      nextActionAt = millis() + DETECT_INTERVAL_READY_MS;
      break;
    }
    default: break;
  }
}

void drainWire() {
  while (Serial1.available()) {
    char value = static_cast<char>(Serial1.read());
    dispatchSerial1Byte(value, false);
    if (responseLength + 1 < RESPONSE_CAPACITY) {
      responseBuffer[responseLength++] = value;
      responseBuffer[responseLength] = '\0';
    } else {
      WireStage completed = wireStage;
      releaseWire();
      if (completed == WIRE_CPIN) {
        observe(OBS_UNKNOWN);
      } else if (completed == WIRE_CEREG_PROBE) {
        probeRegistrationNext = false;
        nextActionAt = millis() + DETECT_INTERVAL_READY_MS;
      } else if (completed == WIRE_CESQ) {
        signalNextAt = millis() + SIGNAL_INTERVAL_MS;
      } else if (completed == WIRE_CGACT) {
        startWire("AT+ICCID", WIRE_ICCID);
      } else if (completed == WIRE_ICCID) {
        activeIccidTail = "";
        startWire("AT+CNMI=2,2,0,0,0", WIRE_CNMI);
      } else if (completed == WIRE_CMEE && !needsConfigure) {
        nextActionAt = millis() + DETECT_INTERVAL_RETRY_MS;
      } else {
        scheduleConfigurationRetry();
      }
      return;
    }
  }

  if (lineEquals("OK") || responseError()) {
    WireStage completed = wireStage;
    handleWireResult(completed, lineEquals("OK") && !responseError());
    return;
  }
  if (millis() - commandStartedAt >= commandTimeout) {
    WireStage completed = wireStage;
    releaseWire();
    if (completed == WIRE_CPIN) {
      observe(OBS_UNKNOWN);
    } else if (completed == WIRE_CEREG_PROBE) {
      probeRegistrationNext = false;
      nextActionAt = millis() + DETECT_INTERVAL_READY_MS;
    } else if (completed == WIRE_CESQ) {
      signalNextAt = millis() + SIGNAL_INTERVAL_MS;
    } else if (completed == WIRE_CGACT) {
      startWire("AT+ICCID", WIRE_ICCID);
    } else if (completed == WIRE_ICCID) {
      activeIccidTail = "";
      startWire("AT+CNMI=2,2,0,0,0", WIRE_CNMI);
    } else if (completed == WIRE_CMEE && !needsConfigure) {
      nextActionAt = millis() + DETECT_INTERVAL_RETRY_MS;
    } else {
      scheduleConfigurationRetry();
    }
  }
}

}  // namespace

void simManagerBegin() {
  state = SIM_UNKNOWN;
  candidate = OBS_UNKNOWN;
  candidateCount = 0;
  wireStage = WIRE_IDLE;
  known = false;
  present = false;
  smsReady = false;
  needsConfigure = false;
  ownsExclusive = false;
  probeRegistrationNext = false;
  configAttempts = 0;
  transportFailures = 0;
  modemReady = false;
  clearSignalCache();
  generation = 1;
  changedAt = millis();
  nextActionAt = millis();
}

void simManagerInvalidate() {
  if (wireStage != WIRE_IDLE) releaseWire();
  candidate = OBS_UNKNOWN;
  candidateCount = 0;
  transportFailures = 0;
  needsConfigure = false;
  configAttempts = 0;
  invalidateCardCaches();
  publishState(SIM_DETECTING, false, false);
  nextActionAt = millis();
}

void simManagerLoop() {
  if (wireStage != WIRE_IDLE) {
    drainWire();
    return;
  }
  if (esimIsBusy() || operatorManagerIsBusy() || modemIsBooting()) return;
  unsigned long now = millis();
  bool signalDue = state == SIM_READY && smsReady && elapsed(now, signalNextAt);
  if (!elapsed(now, nextActionAt) && !signalDue) return;

  if (needsConfigure) {
    // Numeric CME errors make an absent card unambiguous on subsequent probes.
    // The remaining configuration stages stay asynchronous and keep HTTP live.
    startWire("AT+CMEE=1", WIRE_CMEE);
    return;
  }
  if (transportFailures >= 3) {
    startWire("AT+CMEE=1", WIRE_CMEE);
    return;
  }
  if (state == SIM_READY && smsReady && !modemReady && probeRegistrationNext) {
    if (startWire("AT+CEREG?", WIRE_CEREG_PROBE)) probeRegistrationNext = false;
    return;
  }
  if (signalDue) {
    if (startWire("AT+CESQ", WIRE_CESQ)) signalNextAt = now + SIGNAL_INTERVAL_MS;
    return;
  }
  startWire("AT+CPIN?", WIRE_CPIN);
}

bool simManagerIsBusy() {
  return wireStage != WIRE_IDLE || ownsExclusive;
}

bool simManagerIsKnown() {
  return known;
}

bool simManagerIsPresent() {
  return present;
}

bool simManagerIsReady() {
  return state == SIM_READY && known && present;
}

bool simManagerSmsReady() {
  return smsReady;
}

String simManagerIccidTail() {
  return activeIccidTail;
}

void simManagerCaptureIccid(const String &response) {
  activeIccidTail = parseIccidTail(response.c_str());
}

bool simManagerSignalKnown() {
  return signalKnown && signalUpdatedAt && millis() - signalUpdatedAt <= SIGNAL_STALE_MS;
}

bool simManagerSignalRsrqKnown() {
  return simManagerSignalKnown() && signalRsrqKnown;
}

int simManagerSignalRsrpDbm() {
  return signalRsrpDbm;
}

int simManagerSignalRsrqTenthsDb() {
  return signalRsrqTenthsDb;
}

int simManagerSignalRsrpRaw() {
  return signalRsrpRaw;
}

unsigned long simManagerSignalUpdatedAt() {
  return signalUpdatedAt;
}

const char* simManagerStateName() {
  return stateName(state);
}

const char* simManagerMessage() {
  return stateMessage(state);
}

uint32_t simManagerGeneration() {
  return generation;
}

unsigned long simManagerChangedAt() {
  return changedAt;
}
