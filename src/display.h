#pragma once
#include "config.h"

// SSD1306 status display (FR-5.x).
void displayInit();
void displayRender();   // call periodically from loop()
