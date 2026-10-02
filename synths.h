#include "stuff.h"
#include <esp_attr.h>
#include <esp_timer.h>

// cafe_ble: two presets — COCO_OG (Apple π's "original Cocoquantus" coco) and ECHO (the original firmware's echo).
// A preset is the audio interrupt: called once per sample, it reads the input, writes the outputs and restarts
// the I2S at its end (the HEARTBEAT lines).

// ==========================================
// 1. COCO_OG (Apple π, ieat31415): the Cocoquantus v2's coco
// ==========================================
// main + ASH = the tape · YELLOW = the play head's address in binary (the "organ") · EARTH = record on / off
// FLIP = backwards · SKIP = back to the loop point (where SKIP went high) · BUTTON = freeze (lamp)

void IRAM_ATTR coco_og() {



 DACWRITER(pout)
 gyo=ADCREADER // Audio Input signal is read here

// --- WAKE UP & BOOT SYNC ---
    static bool is_first_run = true;
    static bool last_frozen = false;
    static int smoothed_earth = -1;    // for Earth Smoothing 
    if (is_first_run) {
        is_first_run = false;
        // Pre-read the Earth knob to anchor the state without toggling the lamp
        if (EARTHREAD > TRIGGER_ON_THRESHOLD) {
            earth_last_state = 1;
        } else {
            earth_last_state = 0;
        }
    }

// EARTHREAD SLEW
// Needed to prevent false triggers 
int raw_earth = EARTHREAD; 

if (smoothed_earth == -1) {
    smoothed_earth = raw_earth; 
} else {
    smoothed_earth += (raw_earth - smoothed_earth) >> 4; // LPF
}

// HYSTERESIS (Using earth_cv)
 if (earth_last_state == 0) {
     if (smoothed_earth  > TRIGGER_ON_THRESHOLD) {
         lamp = !lamp; 
         audio_frozen_state = lamp;
        if (lamp) { LAMP_ON; } 
        else { LAMP_OFF; }
         earth_last_state = 1; 
     }
 } 
 else { 
     if (smoothed_earth < TRIGGER_OFF_THRESHOLD) {
         earth_last_state = 0; 
     }
 }

 // CROSSFADE
 // Added as per Peter's crossfade
 //  This turns on the crossfade for the Button or Earth
 if (audio_frozen_state != last_frozen) {
     last_frozen = audio_frozen_state;
    TRIGGER_CROSSFADE(audio_frozen_state) // Trigger crossfade will trigger either xfado or yfado based on frozen state
 }

 pout=dellius(t,gyo,audio_frozen_state); //disabing lamp during preset selection to allow buffer transfer
 if (FLIPPERAT) t--; //inverted to make work with sampler based presets
 else t++; 
 t=t&0x1FFFF;
 if (SKIPPERAT)  {
  if (lastskp==0) delayskp = t;
  lastskp = 1;
 } else {
  if (lastskp) t=delayskp;
  lastskp = 0;
 } 


ASHWRITER(pout); //Sends wet audio through ASH. Try swapping out with other Ashes

YELLOW_BINARY(t)

 // HEARTBEAT
 REG(I2S_CONF_REG)[0] &= ~(BIT(5)); 
 REG(I2S_INT_CLR_REG)[0] = 0xFFFFFFFF;
 REG(I2S_CONF_REG)[0] |= (BIT(5)); //start rx 
}

// ==========================================
// 2. ECHO (the original firmware, Peter Blasser)
// ==========================================
// four taps of 32000 / 31578 / 22444 / 25111 samples, recorded over as they go (BUTTON = freeze, lamp)
// main = the four taps · ASH = EARTH, passed through · YELLOW = the taps' places in binary · FLIP = backwards

int myNumbers[] = {32000, 31578, 22444, 25111};
int myPlacers[] = {0, 0, 0, 0};
int tapsz = sizeof(myPlacers) >> 2;

void IRAM_ATTR echo() {
  static bool last_frozen = false;                     // (BUTTON: the recording crossfades out / in, as the original)
  if (audio_frozen_state != last_frozen) { last_frozen = audio_frozen_state; TRIGGER_CROSSFADE(audio_frozen_state) }
  DACWRITER(pout)
  gyo = ADCREADER
  pout = 0;
  for (int i = 0; i < tapsz; i++)
    pout += dellius((myPlacers[i] << 2) + i, gyo, audio_frozen_state);
  pout = pout >> 2;
  if (FLIPPERAT)
    for (int i = 0; i < tapsz; i++) myPlacers[i]++;
  else
    for (int i = 0; i < tapsz; i++) myPlacers[i]--;
  for (int i = 0; i < tapsz; i++) {
    myPlacers[i] %= myNumbers[i];
    if (myPlacers[i] < 0) myPlacers[i] += myNumbers[i];
  }
  REG(I2S_CONF_REG)[0] &= ~(BIT(5));
  ASH_RAW(EARTHREAD);
  REG(I2S_INT_CLR_REG)[0] = 0xFFFFFFFF;
  REG(I2S_CONF_REG)[0] |= (BIT(5));                    // start rx
  YELLOW_BINARY(myPlacers[0] + myPlacers[1] + myPlacers[2] + myPlacers[3]);
}
