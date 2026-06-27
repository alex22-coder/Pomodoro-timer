// ======================== Pomodoro Timer ===========================
// ESP32-C3 Super Mini  
// Work: 25 min, Short break: 5 min, Long break: 15 min (every 4 cycles).
// Touch: short tap = start/pause, long press (≥1 s) = reset.
// Display auto‑off after 30 s, light sleep after 10 more seconds (only when idle/paused).
// ===================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <esp_sleep.h>
#include <driver/adc.h>

// ========== DISPLAY ==========
#define OLED_ADDR       0x3C
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define I2C_SDA         8
#define I2C_SCL         9
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ========== TOUCH ==========
#define TOUCH_PIN       4
#define TOUCH_ACTIVE    HIGH

// ========== BATTERY ==========
#define BAT_PIN         1
#define BAT_MAX_VOLTAGE 4.2
#define BAT_MIN_VOLTAGE 3.0
float batteryVoltage = 0.0;
int batteryPercent = 0;

// ========== POMODORO TIMINGS (seconds) ==========
const unsigned long WORK_TIME     = 25 * 60;      // 25 min
const unsigned long SHORT_BREAK   =  5 * 60;      //  5 min
const unsigned long LONG_BREAK    = 15 * 60;      // 15 min
const unsigned long CYCLES_BEFORE_LONG_BREAK = 4;

// ========== STATES ==========
enum TimerState {
  STATE_IDLE,
  STATE_WORKING,
  STATE_SHORT_BREAK,
  STATE_LONG_BREAK,
  STATE_PAUSED
};
TimerState state = STATE_IDLE;
TimerState prevState = STATE_IDLE;          // to resume after pause
unsigned long remainingSeconds = WORK_TIME;
unsigned int completedCycles = 0;

// ========== TIMING ==========
unsigned long lastTickTime = 0;
const unsigned long TICK_INTERVAL = 1000;

// ========== DISPLAY & SLEEP ==========
bool displayOn = true;
unsigned long lastActivityTime = 0;
const unsigned long DISPLAY_TIMEOUT_MS = 30000;
const unsigned long SLEEP_DELAY_MS     = 10000;

// ========== TOUCH DEBOUNCE ==========
bool lastTouchState = LOW;
unsigned long touchPressStart = 0;
bool longPressTriggered = false;
const unsigned long LONG_PRESS_MS = 1000;

// ------------------------------------------------------------------
//  OLED helpers
// ------------------------------------------------------------------
void oledSleep() {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00);
  Wire.write(0xAE);
  Wire.endTransmission();
}

void oledWake() {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00);
  Wire.write(0xAF);
  Wire.endTransmission();
}

// ------------------------------------------------------------------
//  Battery reading
// ------------------------------------------------------------------
void readBattery() {
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_12);
  int raw = adc1_get_raw(ADC1_CHANNEL_1);
  float voltage = (raw / 4095.0) * 3.3 * 1.92;
  batteryVoltage = voltage;
  batteryPercent = constrain(map(batteryVoltage * 100, BAT_MIN_VOLTAGE*100, BAT_MAX_VOLTAGE*100, 0, 100), 0, 100);
}

// ------------------------------------------------------------------
//  Draw battery icon
// ------------------------------------------------------------------
void drawBattery(int x, int y) {
  display.drawRect(x, y, 20, 10, SH110X_WHITE);
  display.fillRect(x+21, y+2, 2, 6, SH110X_WHITE);
  int fill = map(batteryPercent, 0, 100, 0, 18);
  if (fill > 0) display.fillRect(x+2, y+2, fill, 6, SH110X_WHITE);
}

// ------------------------------------------------------------------
//  Main screen
// ------------------------------------------------------------------
void drawMainScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);

  // Battery
  display.print("Bat ");
  display.print(batteryPercent);
  display.print("% ");
  display.print(batteryVoltage, 1);
  display.print("V");
  drawBattery(90, 0);

  // State name
  display.setCursor(0, 12);
  const char* stateName = "IDLE";
  switch(state) {
    case STATE_IDLE:        stateName = "IDLE"; break;
    case STATE_WORKING:     stateName = "WORK"; break;
    case STATE_SHORT_BREAK: stateName = "BREAK"; break;
    case STATE_LONG_BREAK:  stateName = "LONG BR"; break;
    case STATE_PAUSED:      stateName = "PAUSED"; break;
  }
  display.print("State: ");
  display.println(stateName);

  // Timer
  display.setCursor(0, 24);
  int mins = remainingSeconds / 60;
  int secs = remainingSeconds % 60;
  display.printf("%02d:%02d", mins, secs);

  // Progress bar
  unsigned long total = 0;
  switch(state) {
    case STATE_WORKING:     total = WORK_TIME; break;
    case STATE_SHORT_BREAK: total = SHORT_BREAK; break;
    case STATE_LONG_BREAK:  total = LONG_BREAK; break;
    default: total = 1; break;
  }
  int progress = (total > 0) ? map(WORK_TIME - remainingSeconds, 0, total, 0, 118) : 0;
  progress = constrain(progress, 0, 118);
  display.drawRect(5, 42, 118, 6, SH110X_WHITE);
  display.fillRect(6, 43, progress, 4, SH110X_WHITE);

  // Cycle count
  display.setCursor(50, 24);
  display.print("Cycles: ");
  display.println(completedCycles);

  // Footer
  display.setCursor(0, 56);
  display.print("Tap:start/pause  Long:reset");
  display.display();
}

// ------------------------------------------------------------------
//  Update display (wake if off)
// ------------------------------------------------------------------
void updateDisplay() {
  if (!displayOn) {
    oledWake();
    displayOn = true;
  }
  drawMainScreen();
  lastActivityTime = millis();
}

// ------------------------------------------------------------------
//  Touch handling
// ------------------------------------------------------------------
void handleTouch() {
  bool now = digitalRead(TOUCH_PIN);
  if (now == TOUCH_ACTIVE && !lastTouchState) {
    // Pressed
    touchPressStart = millis();
    longPressTriggered = false;
    if (!displayOn) {
      oledWake();
      displayOn = true;
      drawMainScreen();
    }
    lastActivityTime = millis();
  }
  else if (now != TOUCH_ACTIVE && lastTouchState) {
    // Released
    unsigned long duration = millis() - touchPressStart;
    if (!longPressTriggered) {
      if (duration < LONG_PRESS_MS) {
        // ---------- SHORT TAP ----------
        if (state == STATE_IDLE) {
          // Start work
          state = STATE_WORKING;
          remainingSeconds = WORK_TIME;
          lastTickTime = millis();
          updateDisplay();
        }
        else if (state == STATE_WORKING || state == STATE_SHORT_BREAK || state == STATE_LONG_BREAK) {
          // Pause
          prevState = state;
          state = STATE_PAUSED;
          updateDisplay();
        }
        else if (state == STATE_PAUSED) {
          // Resume
          if (remainingSeconds == 0) {
            // If timer already expired, go to idle
            state = STATE_IDLE;
            remainingSeconds = WORK_TIME;
          } else {
            state = prevState;
          }
          updateDisplay();
        }
      }
      else {
        // ---------- LONG PRESS (reset) ----------
        longPressTriggered = true;
        state = STATE_IDLE;
        remainingSeconds = WORK_TIME;
        completedCycles = 0;
        updateDisplay();
      }
    }
    lastActivityTime = millis();
  }
  lastTouchState = now;
}

// ------------------------------------------------------------------
//  Timer tick (called every second)
// ------------------------------------------------------------------
void timerTick() {
  if (state == STATE_WORKING || state == STATE_SHORT_BREAK || state == STATE_LONG_BREAK) {
    if (remainingSeconds > 0) {
      remainingSeconds--;
      if (displayOn) {
        drawMainScreen();
        lastActivityTime = millis();
      }
    }
    // Transition when time reaches zero
    if (remainingSeconds == 0) {
      switch(state) {
        case STATE_WORKING:
          completedCycles++;
          if (completedCycles % CYCLES_BEFORE_LONG_BREAK == 0) {
            state = STATE_LONG_BREAK;
            remainingSeconds = LONG_BREAK;
          } else {
            state = STATE_SHORT_BREAK;
            remainingSeconds = SHORT_BREAK;
          }
          break;
        case STATE_SHORT_BREAK:
        case STATE_LONG_BREAK:
          state = STATE_WORKING;
          remainingSeconds = WORK_TIME;
          break;
        default:
          break;
      }
      if (displayOn) {
        drawMainScreen();
        lastActivityTime = millis();
      }
    }
  }
}

// ------------------------------------------------------------------
//  Light sleep (only when idle or paused)
// ------------------------------------------------------------------
void goToLightSleep() {
  if (displayOn) {
    oledSleep();
    displayOn = false;
  }
  esp_sleep_enable_gpio_wakeup();
  gpio_wakeup_enable(GPIO_NUM_4, TOUCH_ACTIVE == HIGH ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);

  esp_light_sleep_start();

  // Wake up
  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin(OLED_ADDR, true);
  oledWake();
  displayOn = true;
  drawMainScreen();
  lastActivityTime = millis();
  lastTickTime = millis();
}

// ------------------------------------------------------------------
//  Setup
// ------------------------------------------------------------------
void setup() {
  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin(OLED_ADDR, true);
  display.clearDisplay();
  display.display();
  pinMode(TOUCH_PIN, INPUT_PULLUP);
  readBattery();
  state = STATE_IDLE;
  remainingSeconds = WORK_TIME;
  completedCycles = 0;
  prevState = STATE_IDLE;
  updateDisplay();
  lastTickTime = millis();
  lastActivityTime = millis();
}

// ------------------------------------------------------------------
//  Loop
// ------------------------------------------------------------------
void loop() {
  handleTouch();
  readBattery();

  unsigned long now = millis();
  if (now - lastTickTime >= TICK_INTERVAL) {
    lastTickTime = now;
    timerTick();
    if (displayOn) {
      drawMainScreen();   // refresh (shows battery update)
      lastActivityTime = now;
    }
  }

  // Display timeout
  if (displayOn && (now - lastActivityTime >= DISPLAY_TIMEOUT_MS)) {
    oledSleep();
    displayOn = false;
  }

  // Light sleep only when idle or paused and display off for a while
  if (!displayOn && (state == STATE_IDLE || state == STATE_PAUSED) &&
      (now - lastActivityTime >= DISPLAY_TIMEOUT_MS + SLEEP_DELAY_MS)) {
    goToLightSleep();
  }

  delay(50);
}