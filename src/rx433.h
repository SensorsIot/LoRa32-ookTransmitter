#pragma once
#include "config.h"

// 433 MHz OOK receive gateway (rtl_433_ESP) sharing the one SX1278 with the
// awning transmitter (FR-11). Decodes weather / sensor OOK protocols and
// republishes them to Home Assistant over MQTT auto-discovery. Half-duplex: TX
// and RX are mutually exclusive on the radio, serialised by a mutex.

// Create the radio mutex. Call once in setup(), before the motion TX task
// starts, so the boot transmit can serialise against the radio even though RX
// has not started yet.
void rx433EarlyInit();

// Configure the SX1278 for OOK RX and begin decoding. Call after the network is
// up so decoded packets can be published immediately.
void rx433Init();

// Service the decoder and publish decoded packets to HA. Call from loop().
void rx433Loop();

// Radio hand-off around a transmit burst, called by the motion TX task:
//   rx433TxBegin() - take the radio, pause RX, assert OOK transmit mode
//   rx433TxEnd()   - restore OOK RX, release the radio
// Safe (RX-inactive no-op, still asserts TX) before rx433Init().
void rx433TxBegin();
void rx433TxEnd();

// True once the receiver is running.
bool rx433Active();

// Count of decoded sensor packets since boot (for the OLED / diagnostics).
uint32_t rx433Count();
