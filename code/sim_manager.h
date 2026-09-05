#ifndef SIM_MANAGER_H
#define SIM_MANAGER_H

#include <Arduino.h>

void simManagerBegin();
void simManagerLoop();
void simManagerInvalidate();

bool simManagerIsBusy();
bool simManagerIsKnown();
bool simManagerIsPresent();
bool simManagerIsReady();
bool simManagerSmsReady();
String simManagerIccidTail();
void simManagerCaptureIccid(const String &response);
bool simManagerSignalKnown();
bool simManagerSignalRsrqKnown();
int simManagerSignalRsrpDbm();
int simManagerSignalRsrqTenthsDb();
int simManagerSignalRsrpRaw();
unsigned long simManagerSignalUpdatedAt();
const char* simManagerStateName();
const char* simManagerMessage();
uint32_t simManagerGeneration();
unsigned long simManagerChangedAt();

#endif
