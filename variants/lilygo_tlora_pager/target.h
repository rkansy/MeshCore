#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <TLoraPagerBoard.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#include <helpers/sensors/MicroNMEALocationProvider.h>
#include "TLoraPagerDisplay.h"
#include <helpers/ui/TCA8418Keyboard.h>
#include <helpers/ui/RotaryInputGPIO.h>
#include <helpers/ui/MomentaryButton.h>

extern TLoraPagerBoard board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

extern TLoraPagerDisplay display;
extern TCA8418Keyboard keyboard;
extern RotaryInputGPIO rotary;
extern MomentaryButton user_btn;

bool radio_init();
mesh::LocalIdentity radio_new_identity();
