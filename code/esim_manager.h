#ifndef ESIM_MANAGER_H
#define ESIM_MANAGER_H

#include "globals.h"

void esimManagerBegin();
void esimManagerLoop();
bool esimRefreshProfiles(String &error);
bool esimProfilesLoaded();
void esimManagerInvalidateProfiles();
String esimProfilesJson();
bool esimStartSwitch(const String &profileId, String &message);
bool esimIsBusy();
String esimActiveProfileLabel();
// Stable, inbox-friendly receiver label. Includes a masked ICCID tail when
// available so profiles with the same display name remain distinguishable.
String esimActiveProfileSmsLabel();
String esimJobJson();

#endif
