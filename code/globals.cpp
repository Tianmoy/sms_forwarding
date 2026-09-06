#include "globals.h"

Config config;
Preferences preferences;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);
WebServer server(80);
bool configValid = false;
bool modemReady = false;
ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];
String simPhoneNumber = "";
