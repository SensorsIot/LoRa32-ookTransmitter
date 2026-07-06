#include "motion.h"
#include "ook.h"
#include "rx433.h"

// ---- Command queue ----
enum CmdType { CMD_TARGET, CMD_STOP, CMD_EMERGENCY, CMD_RAW, CMD_BOOTHOME };
struct Command { CmdType type; float target_m; uint8_t rawBtn; };

static QueueHandle_t s_queue;
static TaskHandle_t  s_task;

// ---- Shared status (written by TX task, read by loop) ----
static volatile AwningState s_state    = ST_RETRACTED;
static volatile MoveDir     s_dir      = DIR_NONE;
static volatile float       s_position = 0.0f;   // metres
static volatile float       s_target   = 0.0f;   // metres
static volatile float       s_progress = 0.0f;   // 0..1
static volatile bool        s_safety   = false;
static char                 s_lastCmd[40] = "boot";

// ---- Preemption ----
static volatile bool s_abort = false;  // set by stop/emergency, checked during waits

// ---- Watchdog ----
static volatile uint32_t s_lastHeartbeatMs = 0;
static volatile bool     s_watchdogArmed   = false;
static volatile bool     s_watchdogTripped = false;

static inline float clampTravel(float m) {
  if (m < 0.0f) return 0.0f;
  if (m > MAX_TRAVEL_M) return MAX_TRAVEL_M;
  return m;
}

static void setLastCmd(const char* s) {
  strncpy(s_lastCmd, s, sizeof(s_lastCmd) - 1);
  s_lastCmd[sizeof(s_lastCmd) - 1] = '\0';
}

// Wait `ms`, updating progress. Returns false if aborted (only when abortable).
static bool interruptibleWait(uint32_t ms, bool abortable) {
  uint32_t start = millis();
  if (ms == 0) { s_progress = 1.0f; return true; }
  for (;;) {
    uint32_t el = millis() - start;
    if (el >= ms) { s_progress = 1.0f; return true; }
    if (abortable && s_abort) { s_progress = (float)el / ms; return false; }
    s_progress = (float)el / ms;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Extend from `from` to `to` (to > from): drive down, run the delta, then a
// single up counter-stop that parks the awning (FR-4.2).
static void doExtend(float from, float to) {
  float delta = to - from;                 // > 0
  uint32_t ms = (uint32_t)(delta / SPEED_M_PER_S * 1000.0f);
  s_dir = DIR_EXTEND;
  s_state = ST_MOVING;
  s_progress = 0.0f;
  ookSend(BTN_DOWN, CMD_REPEATS);
  bool done = interruptibleWait(ms, /*abortable=*/true);
  ookSend(BTN_UP, 1);                       // counter-stop, once (FR-4.2)
  float traveled = delta * (done ? 1.0f : s_progress);
  s_position = clampTravel(from + traveled);
  s_dir = DIR_NONE;
  s_state = (s_position <= POS_EPSILON_M) ? ST_RETRACTED : ST_EXTENDED;
}

// Retract to the closed end-stop and re-home to 0 (FR-4.3/4.10). When
// `abortable` and a stop arrives, halt with a down counter-stop at the estimated
// position instead of completing the re-home.
static void doRetractHome(float from, bool abortable) {
  uint32_t ms = (uint32_t)(from / SPEED_M_PER_S * 1000.0f) + RETRACT_MARGIN_S * 1000;
  s_dir = DIR_RETRACT;
  s_state = ST_MOVING;
  s_progress = 0.0f;
  ookSend(BTN_UP, CMD_REPEATS);
  bool done = interruptibleWait(ms, abortable);
  if (!done) {
    ookSend(BTN_DOWN, 1);                   // counter-stop the retract
    float traveled = from * s_progress;     // margin ignored for the estimate
    s_position = clampTravel(from - traveled);
    s_dir = DIR_NONE;
    s_state = (s_position <= POS_EPSILON_M) ? ST_RETRACTED : ST_EXTENDED;
    return;
  }
  s_position = 0.0f;                        // re-home
  s_dir = DIR_NONE;
  s_state = ST_RETRACTED;
}

// Absolute-position move: extend, full re-home, or (partial retract) re-home
// then re-extend to the target using only the two supported primitives.
static void doMoveTo(float target) {
  target = clampTravel(target);
  s_target = target;
  float from = s_position;
  if (fabsf(target - from) < POS_EPSILON_M) { setLastCmd("noop (at target)"); return; }
  s_safety = false;                          // an explicit move clears the safety latch
  char buf[40];
  snprintf(buf, sizeof(buf), "move %.2f->%.2f m", from, target);
  setLastCmd(buf);

  if (target > from) {
    doExtend(from, target);
  } else if (target <= POS_EPSILON_M) {
    doRetractHome(from, /*abortable=*/true);
  } else {
    doRetractHome(from, /*abortable=*/true);
    if (!s_abort) doExtend(0.0f, target);
  }
}

static void doEmergency(bool boot) {
  s_safety = !boot;
  s_target = 0.0f;
  setLastCmd(boot ? "boot home" : "EMERGENCY retract");
  float from = boot ? (MAX_TRAVEL_M + 1.0f) : s_position;  // boot: assume worst case
  uint32_t ms = boot ? (FULL_RETRACT_S * 1000)
                     : ((uint32_t)(from / SPEED_M_PER_S * 1000.0f) + RETRACT_MARGIN_S * 1000);
  s_dir = DIR_RETRACT;
  s_state = ST_MOVING;
  s_progress = 0.0f;
  ookSend(BTN_UP, CMD_REPEATS);
  interruptibleWait(ms, /*abortable=*/false);  // safety retract cannot be preempted
  s_position = 0.0f;
  s_dir = DIR_NONE;
  s_state = ST_RETRACTED;
}

static void handleCommand(const Command& c) {
  s_abort = false;                          // start each queued command fresh

  // STOP carries no transmission; every other command keys the radio, so take
  // the radio away from the RX gateway for the whole command (FR-11 half-duplex).
  bool transmits = (c.type != CMD_STOP);
  if (transmits) rx433TxBegin();

  switch (c.type) {
    case CMD_BOOTHOME:  doEmergency(/*boot=*/true);  break;
    case CMD_EMERGENCY: doEmergency(/*boot=*/false); break;
    case CMD_TARGET:    doMoveTo(c.target_m);        break;
    case CMD_RAW:
      { char b[40]; snprintf(b, sizeof(b), "raw %s", BUTTONS[c.rawBtn].name); setLastCmd(b); }
      ookSend(c.rawBtn, CMD_REPEATS);
      break;
    case CMD_STOP:      /* a running move already saw s_abort; nothing to do */ break;
  }

  if (transmits) rx433TxEnd();
}

static void txTask(void*) {
  for (;;) {
    Command c;
    if (xQueueReceive(s_queue, &c, portMAX_DELAY) == pdTRUE) {
      handleCommand(c);
    }
  }
}

// ---- Producers ----
static void enqueue(const Command& c) {
  if (s_queue) xQueueSend(s_queue, &c, 0);
}

void motionInit() {
  s_queue = xQueueCreate(8, sizeof(Command));
  xTaskCreatePinnedToCore(txTask, "awning_tx", 4096, nullptr, 2, &s_task, APP_CPU_NUM);
  Command c{ CMD_BOOTHOME, 0.0f, 0 };
  enqueue(c);
}

void motionCommandTarget(float target_m) {
  Command c{ CMD_TARGET, target_m, 0 };
  enqueue(c);
}
void motionCommandPct(float pct) {
  motionCommandTarget(pct / 100.0f * MAX_TRAVEL_M);
}
void motionOpen()  { motionCommandTarget(MAX_TRAVEL_M); }
void motionClose() { motionCommandTarget(0.0f); }

void motionStop() {
  s_abort = true;                           // preempt any running move
  Command c{ CMD_STOP, 0.0f, 0 };
  enqueue(c);
}

void motionEmergency() {
  s_abort = true;                           // break out of a running move first
  Command c{ CMD_EMERGENCY, 0.0f, 0 };
  enqueue(c);
}

void motionRaw(uint8_t btnIdx) {
  if (btnIdx >= NUM_BUTTONS) return;
  Command c{ CMD_RAW, 0.0f, btnIdx };
  enqueue(c);
}

// ---- Watchdog ----
void motionFeedWatchdog(bool unsafe) {
  s_lastHeartbeatMs = millis();
  s_watchdogArmed = true;
  s_watchdogTripped = false;
  if (unsafe) motionEmergency();
}

void motionWatchdogTick() {
  if (!s_watchdogArmed || s_watchdogTripped) return;
  if (millis() - s_lastHeartbeatMs > EMERGENCY_TIMEOUT_S * 1000UL) {
    s_watchdogTripped = true;
    motionEmergency();
  }
}

// ---- Getters ----
AwningState motionState()       { return s_state; }
MoveDir     motionDir()         { return s_dir; }
float       motionPositionM()   { return s_position; }
float       motionPositionPct() { return s_position / MAX_TRAVEL_M * 100.0f; }
float       motionTargetM()     { return s_target; }
bool        motionMoving()      { return s_state == ST_MOVING; }
float       motionProgress()    { return s_progress; }
bool        motionSafetyRetract() { return s_safety; }
const char* motionLastCmd()     { return s_lastCmd; }
