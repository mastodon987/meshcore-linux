#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <LinuxBoard.h>
#if !defined(RADIO_NONE)
  #include <helpers/radiolib/CustomSX1276Wrapper.h>
#endif
#include <helpers/sensors/EnvironmentSensorManager.h>
#ifdef DISPLAY_CLASS
  #include <helpers/ui/SSD1306Display.h>
  #include <helpers/ui/MomentaryButton.h>
#endif

#if defined(USE_CUSTOM_SX1262_WRAPPER)
  #include <helpers/radiolib/LinuxSX1262Wrapper.h>
#endif

#if defined(RADIO_NONE)
  #include <helpers/radiolib/NullRadio.h>
#endif

extern LinuxBoard board;
extern LinuxRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

#if defined(RADIO_NONE)
  extern NullRadio radio_driver;
#else
  extern WRAPPER_CLASS radio_driver;
#endif

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

bool radio_init();
uint32_t radio_get_rng_seed();
void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr);
void radio_set_tx_power(uint8_t dbm);
mesh::LocalIdentity radio_new_identity();
