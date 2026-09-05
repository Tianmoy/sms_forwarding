#ifndef CONFIG_H
#define CONFIG_H

#include "globals.h"

void saveConfig();
void loadConfig();
bool isPushChannelValid(const PushChannel& ch);
bool isConfigValid();
String getDeviceUrl();
uint32_t configLastKeepaliveDay();
void configRecordKeepalive(uint32_t day);

#endif
