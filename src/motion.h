#pragma once
#include "config.h"

// Awning motion state machine + dedicated FreeRTOS TX task (2.5, 2.6).

enum AwningState { ST_RETRACTED, ST_MOVING, ST_EXTENDED };
enum MoveDir     { DIR_NONE, DIR_EXTEND, DIR_RETRACT };

// Create the command queue and start the TX task pinned to the app core, then
// queue the boot full-retract that homes the awning to 0 (FR-4.10).
void motionInit();

// --- Command producers (safe to call from loop / MQTT / HTTP / serial) ---
void motionCommandTarget(float target_m);   // absolute target in metres (FR-4.4)
void motionCommandPct(float pct);            // 0..100 % of MAX_TRAVEL_M (FR-7.3)
void motionOpen();                           // extend fully
void motionClose();                          // retract to 0
void motionStop();                           // preempt an in-progress move (FR-10.2)
void motionEmergency();                      // full retract now (FR-4.5)
void motionRaw(uint8_t btnIdx);              // transmit a codeword directly (auto/manual, FR-8.1)

// Watchdog (FR-4.6). Feed on each heartbeat; unsafe payload forces emergency.
void motionFeedWatchdog(bool unsafe);
// Call periodically from loop(): trips an emergency retract on heartbeat loss.
void motionWatchdogTick();

// --- Status getters (for OLED / HTTP / MQTT) ---
AwningState motionState();
MoveDir     motionDir();
float       motionPositionM();     // metres
float       motionPositionPct();   // 0..100
float       motionTargetM();
bool        motionMoving();
float       motionProgress();      // 0..1 during a move
bool        motionSafetyRetract(); // true after an emergency/watchdog retract
const char* motionLastCmd();
