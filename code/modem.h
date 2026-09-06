#ifndef MODEM_H
#define MODEM_H

#include "globals.h"

String sendATCommand(const char* cmd, unsigned long timeout);
void modemPowerCycle();
void resetModule();
void modemInit();
void queryPhoneNumber();
bool sendATandWaitOK(const char* cmd, unsigned long timeout);
bool waitCEREG();
void blink_short(unsigned long gap_time = 500);
bool sendSMS(const char* phoneNumber, const char* message);
bool modemIsBusy();
bool modemIsBooting();
bool modemAcquireExclusive();
void modemReleaseExclusive();

#endif
