#include <FastLED.h>

// Configuration
#define LED_PIN     13
#define NUM_LEDS    15
#define BRIGHTNESS  64
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
// ================= CONFIG =================
#define CLK 4
#define DT  5
#define SW  6

// ================= INTERNAL STATE =================
volatile int rawEncoder = 0;
volatile uint8_t lastState = 0;
volatile unsigned long lastInterruptTime = 0;

const unsigned long debounceMicros = 1000; // encoder debounce

// Button
bool buttonState = HIGH;
bool lastButtonState = HIGH;
unsigned long lastButtonDebounceTime = 0;
const unsigned long buttonDebounce = 50;

// Processed values
int dialPosition = 0;
int lastDialPosition = 0;
int max_chats = 5;
bool dialMovedFlag = false;
bool buttonPressedFlag = false;

// Quadrature lookup table
const int8_t encTable[16] = {
  0, -1,  1,  0,
  1,  0,  0, -1,
 -1,  0,  0,  1,
  0,  1, -1,  0
};

CRGB leds[NUM_LEDS];

// Global position of the "indicator" light
int currentPos = 0;

// ================= ISR =================
void IRAM_ATTR handleEncoderISR() {
  unsigned long now = micros();
  if (now - lastInterruptTime < debounceMicros) return;
  lastInterruptTime = now;

  uint8_t state = (digitalRead(CLK) << 1) | digitalRead(DT);
  uint8_t index = (lastState << 2) | state;

  rawEncoder += encTable[index];
  //rawEncoder = constrain(rawEncoder, 0, max_chats*4);
  lastState = state;
}

// ================= INIT =================
void initDial() {
  pinMode(CLK, INPUT_PULLUP);
  pinMode(DT, INPUT_PULLUP);
  pinMode(SW, INPUT_PULLUP);

  lastState = (digitalRead(CLK) << 1) | digitalRead(DT);

  attachInterrupt(digitalPinToInterrupt(CLK), handleEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DT), handleEncoderISR, CHANGE);
}

// ================= MAIN HANDLER =================
void handleDial() {
  // ---- Handle rotation ----
  noInterrupts();
  int raw = rawEncoder;
  interrupts();

  int newDial = (raw / 4) % max_chats; // 4 steps per click

  if (newDial != lastDialPosition) {
    dialPosition = newDial;
    lastDialPosition = newDial;
    dialMovedFlag = true;
  }

  // ---- Handle button ----
  int reading = digitalRead(SW);

  if (reading != lastButtonState) {
    lastButtonDebounceTime = millis();
  }

  if ((millis() - lastButtonDebounceTime) > buttonDebounce) {
    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState == LOW) {
        buttonPressedFlag = true;
      }
    }
  }

  lastButtonState = reading;
}

// ================= PUBLIC API =================
int getDialPosition() {
  return dialPosition;
}

bool wasDialMoved() {
  if (dialMovedFlag) {
    dialMovedFlag = false;
    return true;
  }
  return false;
}

bool wasButtonPressed() {
  if (buttonPressedFlag) {
    buttonPressedFlag = false;
    return true;
  }
  return false;
}

// Updates the strip based on a single integer position.
void updateIndicator(int pos) {
  FastLED.clear();

  int ledPos = pos * 3;

  leds[ledPos] = CRGB::White;
  //leds[ledPos+1] = CRGB::White;

  FastLED.show();
}

void indicatorFlash() {
  FastLED.clear();
  FastLED.show();

  delay(100);

  updateIndicator(dialPosition);
}

// =====================================================
// =====================================================

void setup() {
  Serial.begin(115200);
  initDial();

  // Initialize FastLED
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  
  // Set initial state
  updateIndicator(dialPosition);
}

void loop() {
  handleDial();

  if (wasDialMoved()) {
    Serial.print("Dial: ");
    Serial.println(getDialPosition());

    updateIndicator(getDialPosition());
  }

  if (wasButtonPressed()) {
    Serial.println("Button pressed");

    indicatorFlash();
  }
}