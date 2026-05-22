#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusOgg.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include <FastLED.h>


// Bot Token, Chat-IDs, WiFi credentials
#include "../Shared/arduino_secrets.h"

// ========================================================================================

// Forward Declarations
void playNewMessage();
void startRecording();
void stopRecording();
void doNothing();
void setLED (bool state);
void playChatWrapper();
void playChat(const char* chatId);
bool isValidChat(const char* chat_id, int* printIndex_optional = nullptr);
void chargeBattery();
void handleBehavior(unsigned long tp_now);
bool downloadTelegramFile(const char* fileId, const char* destination);
void cleanupChatFolder(const char* folderPath, const char* prefix);
bool hasChatsWithNews(int* printNumber_optional = nullptr);
void sendWavFile(const char* filePath, const char* fileName, const char* chat_id);
bool fetchMessages();
bool processTelegramUpdates();
void startPlayback(const char* filePath);
const char* getChatId();
int findChatId(const char* chatId);
//void handleButton(Button &btn, const char* name);
void convertExtension_ogg2wav(const char* oggStr, char* wavStr);
int getNextFileIndex(const char* folderPath, const char* prefix);
bool tryConnectWiFi(int timeout_ms);
void disconnectWiFi();
void stopPlayback();
void processAudio();
void setI2SData_playback();
void recordTask(void *pvParameters);
void sdWriteTask(void *pvParameters);
void setI2SData_recording();
void addTranscodeJob(const char* oggFile, const char* wavFile);
void transcodeTask(void *pvParameters);

// ==========================================================================================

// POWER MANAGEMENT ============================

RTC_DATA_ATTR int bootCount = 0;

RTC_DATA_ATTR unsigned long battery_tpLastRefresh = 0;
#define INTERVAL_BATTERY_UPDATE_US 180000000 // 3min

RTC_DATA_ATTR unsigned long totalMillis = 0;

RTC_DATA_ATTR unsigned long tp_lastFetch = 0;
#define INTERVAL_AUTO_FETCH 39600000 // 11h

unsigned long tp_lastInteraction_millis = 0;
#define TIMER_AUTO_SLEEP 120000 // 2min


// USER INTERFACE ==============================

// Button and LED --------------

#define BUTTON 8

#define LED_NOTIFICATION 7
RTC_DATA_ATTR bool state_LED = false;

const unsigned long SHORT_PRESS_TIME = 500; // Threshold for long press

using ButtonEvent = void (*)();

struct Button {
  const int pin;
  bool lastState;
  unsigned long pressStartTime;
  bool isPressed;
  bool isHolding;
  ButtonEvent onShortPress;
  ButtonEvent onHold;
  ButtonEvent onRelease;
};

Button button = {BUTTON, HIGH, 0, false, false, playNewMessage, startRecording, stopRecording};
bool buttonActive = false;

// Rotary Encoder ----------------

#define CLK 4
#define DT  5
#define SW  6

volatile int rawEncoder = 0;
volatile uint8_t lastState = 0;
volatile unsigned long lastInterruptTime = 0;

const unsigned long debounceMicros = 1000; // encoder debounce

// RotEnc Button
bool buttonState = HIGH;
bool lastButtonState = HIGH;
unsigned long lastButtonDebounceTime = 0;
const unsigned long buttonDebounce = 50;

// Processed values
int dialPosition = 0;
int lastDialPosition = 0;
bool dialMovedFlag = false;
bool buttonPressedFlag = false;

// Quadrature lookup table
const int8_t encTable[16] = {
  0, -1,  1,  0,
  1,  0,  0, -1,
 -1,  0,  0,  1,
  0,  1, -1,  0
};


// LED strip ----------------

#define LED_PIN     13
#define NUM_LEDS    15
#define BRIGHTNESS  50
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

CRGB leds[NUM_LEDS];

bool showingBatteryLevel = false;
unsigned long tp_showBatteryLevelUntil = 0;

// CHAT & PLAYBACK CONTROL ===========================

#define QUEUE_SIZE 20 // maximum expected number of files
#define MAX_PATH_LEN 64 // maximum path length

struct PlaybackQueue {
    char queue[QUEUE_SIZE][MAX_PATH_LEN];
    int head = 0; // Where we add new items
    int tail = 0; // Where we read items
    int count = 0; // Current number of items

    bool push(const char* path) {
        if (count >= QUEUE_SIZE) return false; // Queue full
        strncpy(queue[head], path, MAX_PATH_LEN);
        head = (head + 1) % QUEUE_SIZE;
        count++;
        return true;
    }

    bool pop(char* dest) {
        if (!hasNext()) return false; // queue empty or waiting

        strncpy(dest, queue[tail], MAX_PATH_LEN);
        tail = (tail + 1) % QUEUE_SIZE;
        count--;

        return true;
    }

    void clear() {
        head = 0;
        tail = 0;
        count = 0;
    }

    bool hasNext() {
        if(count > 0) return true;
        return false;
    }
};

PlaybackQueue playbackQueue;

int chat_index = 0;
RTC_DATA_ATTR bool newMessages[max_chats] = {false};


// BATTERY & SYSTEM BEHAVIOR =========================

RTC_DATA_ATTR int battery_level = 100;

struct Battery {

    int cost_send = 30;
    int cost_update = 30;
    int cost_listen = 5;

    void chargeBy(int points) {
        battery_level += points;
        if (battery_level > 100) battery_level = 100;
    }

    bool canSend() {
        if (battery_level >= cost_send) {
            return true;
        } else {
            Serial.println("[BATTERY] Action not possible, need more sun :)1");

            // LED blink to signalize system
            digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW);
            delay(100);
            digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW);
            delay(100);
            digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW);

            return false;
        };
    }

    bool canFetch() {
        if (battery_level >= cost_update) {
            return true;
        } else {
            Serial.println("[BATTERY] Action not possible, need more sun :)2");

            // LED blink to signalize system
            digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW);
            delay(100);
            digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW);
            delay(100);
            digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW);

            return false;
        };
    }

    bool canListen() {
        if (battery_level >= cost_listen) {
            return true;
        } else {
            Serial.println("[BATTERY] Action not possible, need more sun :)3");

            // LED blink to signalize system
            digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW);
            delay(100);
            digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW);
            delay(100);
            digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW);

            return false;
        };
    }

    void subtractSend() {
        battery_level -= cost_send;
    }

    void subtractUpdate() {
        battery_level -= cost_update;
    }

    void subtractListen() {
        battery_level -= cost_listen;
    }
};

Battery battery;


// NETWORK COMMUNICATION =============================

WiFiClientSecure client_wifi;

RTC_DATA_ATTR long lastUpdateId = 0;
i2s_chan_handle_t rx_handle = NULL;

bool isDownloading = false;


// INTERNAL COMMUNICATION =======================

// I2S Pins
#define I2S_DOUT  12
#define I2S_DIN   11
#define I2S_BCLK  9
#define I2S_LRC   10

// SD Card Pins (SDMMC)
#define SD_CS     14
#define SD_CLK    39
#define SD_CMD    38
#define SD_D0     40


// AUDIO PLAYBACK ===========================

bool isAudioRunning = false;
File audioFile;

I2SStream i2s_playback;
WAVDecoder decoder_wav;
EncodedAudioStream stream_wavToI2s(&i2s_playback, &decoder_wav); // data written to 'stream_wavToI2s' gets decoded and sent to I2S.
StreamCopy copier_playback(stream_wavToI2s, audioFile); // pulls bytes from sd-file and pushes them to stream_wavToI2s



// AUDIO RECORDING =========================
// Mic → I2SStream → AudioStream → WAVEncoder → File

I2SStream i2s_record;
AudioInfo info_recording(16000, 2, 32); // for the I2S Stream [sample-rate, num_channels, bits per sample]

WAVEncoder encoderWav_record;
AudioInfo info_WavEncoder(16000, 1, 16); // for Wav Encoder AND EncodedStream (must match) [sample-rate, num_channels, bits per sample]

File audioFile_record;
EncodedAudioStream stream_wavToFile(&audioFile_record, &encoderWav_record);

volatile bool isRecording = false;
bool wasRecording = false;
char currentFilePath[64]; // where the file is stored, that is being recorded

// Memory pool for passing data without dynamic allocation overhead
#define NUM_CHUNKS 20
#define CHUNK_SIZE 1024

struct AudioBuffer {
    uint8_t data[CHUNK_SIZE];
    size_t size;
};

AudioBuffer buffers[NUM_CHUNKS];
QueueHandle_t emptyQueue;
QueueHandle_t fullQueue;


// AUDIO TRANSCODING ============================
// file.ogg --> OpusOggDecoder --StreamCopy--> WavEncoder --> File.wav

bool isTranscoding = false;
bool transcoderInitialized = false;

// Struct to pass jobs to the FreeRTOS task
struct TranscodeJob {
    char inFile[MAX_PATH_LEN];
    char outFile[MAX_PATH_LEN];
};

// Transcoding Queue
SemaphoreHandle_t startTranscodeGate;
QueueHandle_t jobQueue;
TaskHandle_t transcodeTaskHandle = NULL;

File audioFileIn;
File audioFileOut;

// Pipeline Components
OpusOggDecoder decoder_opusOgg;
WAVEncoder encoder_wav;

EncodedAudioStream stream_OggToFileIn(&audioFileIn, &decoder_opusOgg);
EncodedAudioStream stream_WavToFileOut(&audioFileOut, &encoder_wav);

StreamCopy copier_transcode(stream_WavToFileOut, stream_OggToFileIn, 16384);



// =============================================================================================

// =============================================================================================


// SETUP & LOOP =======================

// Wake up and figure out what to do
void setup() {
    Serial.begin(115200);

    // Wait for up to 5 seconds for the serial
    unsigned long start = millis();
    while (!Serial && millis() - start < 5000) {
        delay(10);
    }

    bootCount++;
    Serial.printf(" Just woke up. Boot count: %d\n", bootCount);

    resetAutoSleep();

    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();

    switch (wakeReason) {
        case ESP_SLEEP_WAKEUP_TIMER:
        setupSensingMode();
        break;

        case ESP_SLEEP_WAKEUP_GPIO:
        setupUsageMode();
        break;

        default:
        setupUsageMode();
    }
}

// This runs every X minutes
void setupSensingMode() {
    Serial.println("Running sensing mode...");

    totalMillis += INTERVAL_BATTERY_UPDATE_US / 1000; // add the time you spent sleeping

    chargeBattery(); // Charge battery according to conditions
    handleBehavior(totalMillis); // Check if fetching should happen

    // Wait for any transcoding to complete before sleeping
    while (isTranscoding) {
        delay(100);
    }

    delay(300);

    goToSleep();
}

// This runs on user interaction
void setupUsageMode() {
    Serial.println("Running usage mode...");

    // Pins
    pinMode(BUTTON, INPUT_PULLUP);
    pinMode(LED_NOTIFICATION, OUTPUT);

    //AudioLogger::instance().begin(Serial, AudioLogger::Debug);

    Serial.println("\n[SYSTEM] Initializing...");

    initSDCard();
    initDial();
    initLEDRing();

    setI2SData_playback();

    digitalWrite(LED_NOTIFICATION, state_LED);

    // Initialize Audio Recording -----------------------

    emptyQueue = xQueueCreate(NUM_CHUNKS, sizeof(AudioBuffer*));
    fullQueue = xQueueCreate(NUM_CHUNKS, sizeof(AudioBuffer*));
    
    // Fill up emptyQueue with (yet undefined) buffers
    for (int i = 0; i < NUM_CHUNKS; i++) {
        AudioBuffer* ptr = &buffers[i];
        xQueueSend(emptyQueue, &ptr, portMAX_DELAY);
    }

    setI2SData_recording();

    // Create FreeRTOS Tasks
    xTaskCreatePinnedToCore(recordTask, "RecordTask", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(sdWriteTask, "SDWriteTask", 8192, NULL, 3, NULL, 1);


    // Audio Transcoding -------------------------
    initTranscoder();

    Serial.println("[SYSTEM] Ready.");
}

void goToSleep() {

    digitalWrite(LED_NOTIFICATION, LOW);

    disconnectWiFi();

    FastLED.clear();
    FastLED.show();

    // Set timers to wake up
    esp_sleep_enable_timer_wakeup(INTERVAL_BATTERY_UPDATE_US); // 3 min

    // Set buttons to wake up
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_8, 0); // Wake on LOW
    rtc_gpio_pullup_en(GPIO_NUM_8);
    rtc_gpio_pulldown_dis(GPIO_NUM_8);

    totalMillis += millis(); // add the time you took for this execution cycle

    Serial.println("Going back to sleep...");

    // Go back to sleep
    esp_deep_sleep_start();
}

// runs repeatedly during active usage
void loop() {
    processAudio();

    if(!buttonActive && millis() > 4000) buttonActive = true; // Activate button with delay
    handleButton(button, "button");

    handleDial();

    handleLEDStrip();

    if (wasDialMoved()) {
        selectChat(dialPosition);
    }

    if (wasButtonPressed()) {
        playChatWrapper();
    }

    if(!isRecording && !isAudioRunning && !isTranscoding) {
        handleBehavior(totalMillis + millis());
        handleAutoSleep();
    }

    if(!isAudioRunning && playbackQueue.hasNext()) {
        static char filePlaying[MAX_PATH_LEN];
        playbackQueue.pop(filePlaying);
        delay(800);
        startPlayback(filePlaying);
    }
}



// =============================================================================================

// =============================================================================================


// USER CONTROL ==============================

void handleButton(Button &btn, const char* name) {
    if(!buttonActive) return;

    bool currentState = digitalRead(btn.pin);

    // Button Pressed (Falling edge, LOW --> HIGH)
    if (currentState == LOW && btn.lastState == HIGH) {
        btn.pressStartTime = millis();
        btn.isPressed = true;
        btn.isHolding = false;
        delay(20); // Basic debounce
    }

    // Button Released (Rising edge, HIGH --> LOW)
    if (currentState == HIGH && btn.lastState == LOW) {
        resetAutoSleep(); // Every interaction with the device resets autosleep

        if (!btn.isHolding) {
        Serial.printf("%s: Short Press\n", name);
        if (btn.onShortPress) btn.onShortPress();
        } 
        else { 
        if (btn.onRelease) btn.onRelease();
        }
        btn.isPressed = false;
    }

    // Check for Long Press
    if (btn.isPressed && !btn.isHolding && (millis() - btn.pressStartTime > SHORT_PRESS_TIME)) {
        Serial.printf("%s: Long Press (Hold)\n", name);
        if (btn.onHold) btn.onHold();
        btn.isHolding = true;
    }

    btn.lastState = currentState;
}

void doNothing() {} // Helper function to prevent crash if an event is unassigned


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

void initDial() {
  pinMode(CLK, INPUT_PULLUP);
  pinMode(DT, INPUT_PULLUP);
  pinMode(SW, INPUT_PULLUP);

  lastState = (digitalRead(CLK) << 1) | digitalRead(DT);

  attachInterrupt(digitalPinToInterrupt(CLK), handleEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DT), handleEncoderISR, CHANGE);
}

void handleDial() {
  // Handle rotation ----
  noInterrupts();
  int raw = rawEncoder;
  interrupts();

  int newDial = (raw / 4) % max_chats; // 4 steps per click

  if (newDial != lastDialPosition) {
    dialPosition = newDial;
    lastDialPosition = newDial;
    dialMovedFlag = true;
  }

  // Handle button ----
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

void selectChat(int pos) {
    if(isRecording) return;

    chat_index = pos;

    Serial.printf("[DIAL] Changed chat to index: %d \n", chat_index);
    Serial.print("New Messages: "); 
    for(int i = 0; i < 5; i++) {
      Serial.print(newMessages[i]);
    }
    Serial.println("");

    showDialPositionLED(chat_index);
}

void setLED (bool state) {
    if (state) {digitalWrite(LED_NOTIFICATION, HIGH);}
    else digitalWrite(LED_NOTIFICATION, LOW);

    state_LED = state;
}

// LED strip ---------------------

void initLEDRing() {
    // Initialize FastLED
    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
  
    // Set initial state
    showBatteryLevelLED(4000);
}

/** Updates the strip based on a single integer position.**/
void showDialPositionLED(int pos) {
  FastLED.clear();

  int ledPos = pos * 3; // Every third LED is used for indication

  leds[ledPos] = CRGB::White;
  //leds[ledPos+1] = CRGB::White;

  FastLED.show();
}

void handleLEDStrip() {
    if (showingBatteryLevel && tp_showBatteryLevelUntil - millis() < 0) {
        showDialPositionLED(dialPosition);
    }
}

void showBatteryLevelLED(int duration_ms) {
    FastLED.clear();

    int batteryLevel = constrain(battery_level, 0, 100); // Stay in range to be sure
    int ledsLit = NUM_LEDS * (batteryLevel / 100);

    // Fill from index 0 up to ledsLit with green
    for (int i = 0; i < ledsLit && i < NUM_LEDS; i++) {
        leds[i] = CRGB::Green;
    }

    FastLED.show();

    showingBatteryLevel = true;
    tp_showBatteryLevelUntil = millis() + duration_ms;
}

void indicatorFlash() {
  FastLED.clear();
  FastLED.show();

  delay(100);

  showDialPositionLED(dialPosition);
}



// CHAT & PLAYBACK CONTROL ==============================

/** Plays a new message, either already on device or fetched. For the user, this acts like it goes online everytime **/
void playNewMessage() {
    if (isRecording) return;
    if (isTranscoding) return;
    if (isAudioRunning) return;

    if (!fetchMessages()) {
        Serial.println("[MESSAGE] No new messages :(");
        return;
    };

    while (isTranscoding) {}
    
    // New messages have been "fetched"

    int chatsWithNews = 0; // Will hold number of chats with new messages

    // Pick a random chat with new messages
    if(hasChatsWithNews(&chatsWithNews)) {
        int chats_indices[max_chats]; // will hold the chatIndices of all chats containing unlistened messages
        int j = 0;
        for(int i = 0; i < max_chats; i++) {
            if(newMessages[i]) chats_indices[j++] = i; // fill array
        }
        int randomChatIndex = (totalMillis + millis()) % chatsWithNews; // pick a random chat
        chat_index = randomChatIndex;
        playChat(chat_ids[chats_indices[randomChatIndex]]); // play it back
    } else {
        Serial.println("[ERROR] Chats with new messages weren't found");
        return;
    }

    setLED(false);
}

void playChatWrapper() {
    if (!battery.canListen()) return;

    if (state_LED && newMessages[chat_index]) setLED(false); // if we manually selected the chat with new messages -> turn off light
    playChat(getChatId());
    battery.subtractListen();
}

/** Play the last messages in a chat folder, transcode them if specified**/
void playChat(const char* chatId) {
    if (isRecording) return;
    if (isTranscoding) return;
    if (isAudioRunning) stopPlayback();

    char folderPath[24]; snprintf(folderPath, sizeof(folderPath), "/%s", chatId);

    File dir = SD_MMC.open(folderPath);
    if (!dir) {
        Serial.println("[ERROR] Directory was not found");
        return;
    };

    playbackQueue.clear(); // Clear any pending items in the queue

    // We will collect file names, sort them to guarantee correct chronological playback
    char fileNames[QUEUE_SIZE][MAX_PATH_LEN];
    int fileCount = 0;

    // Loop through files of specified chat folder
    while (File entry = dir.openNextFile()) {
        if (!entry.isDirectory() && fileCount < QUEUE_SIZE) {
            strncpy(fileNames[fileCount], entry.name(), MAX_PATH_LEN);
            fileCount++;
        }
        entry.close();
    }
    dir.close();

    // Sort files alphabetically so "received_000", "received_001" are processed in order
    for (int i = 0; i < fileCount - 1; i++) {
        for (int j = i + 1; j < fileCount; j++) {
            if (strcmp(fileNames[i], fileNames[j]) > 0) {
                char temp[MAX_PATH_LEN];
                strncpy(temp, fileNames[i], MAX_PATH_LEN);
                strncpy(fileNames[i], fileNames[j], MAX_PATH_LEN);
                strncpy(fileNames[j], temp, MAX_PATH_LEN);
            }
        }
    }

    // Loop through files of specified chat folder
    for (int i = 0; i < fileCount; i++) {
        const char* name = fileNames[i];  // "received_000.ogg"; "recorded_001.wav"

        // If file is a wav
        if (strstr(name, ".wav") != nullptr) {
            char filePath[MAX_PATH_LEN];
            snprintf(filePath, sizeof(filePath), "%s/%s", folderPath, name);
            playbackQueue.push(filePath);
        }
    }

    newMessages[findChatId(chatId)] = false; // messages in this chat were all listened to
}

void convertExtension_ogg2wav(const char* oggStr, char* wavStr) {
    if (!oggStr || !wavStr) return;

    // Copy source to destination safely with bounds checking
    strncpy(wavStr, oggStr, MAX_PATH_LEN - 1);
    wavStr[MAX_PATH_LEN - 1] = '\0';

    size_t len = strlen(wavStr);
    if (len >= 4) {
        // Replace extension with .wav
        strncpy(wavStr + len - 4, ".wav", 4);
        wavStr[len] = '\0';
    }
}

/** Scans a folder to find the next available sequential number for a specific prefix**/
int getNextFileIndex(const char* folderPath, const char* prefix) {
    File dir = SD_MMC.open(folderPath);
    if (!dir) return 0;
    
    int maxIndex = -1;
    while (File entry = dir.openNextFile()) {
        if (!entry.isDirectory()) {
            const char* name = entry.name();
            // Check if the filename starts with the required prefix
            if (strncmp(name, prefix, strlen(prefix)) == 0) {
                // Extract number after the prefix
                int idx = atoi(name + strlen(prefix));
                if (idx > maxIndex) maxIndex = idx;
            }
        }
        entry.close();
    }
    dir.close();
    return maxIndex + 1; // Return the next number
}

/** Only ever keep all the last received OR all the last recorded messages in the chat folder**/
void cleanupChatFolder(const char* folderPath, const char* prefix) {
    File dir = SD_MMC.open(folderPath);
    if (!dir) return;

    while (File entry = dir.openNextFile()) {
        if (!entry.isDirectory()) {
            const char* name = entry.name();

            // Check if the filename starts with the required prefix
            if (strncmp(name, prefix, strlen(prefix)) == 0) {
                char filePath[MAX_PATH_LEN];
                snprintf(filePath, sizeof(filePath), "%s/%s", folderPath, name);
                SD_MMC.remove(filePath);
            }
        }
        entry.close();
    }
    dir.close();
}

/** Tells whether there are new messages already on the device**/
bool hasChatsWithNews(int* printNumber_optional) {
    if (printNumber_optional == nullptr) {
        for(int i = 0; i < max_chats; i++) {
            if(newMessages[i]) return true;
        }
        return false;
    } 
    else {
        int chatsWithNews = 0;
        for(int i = 0; i < max_chats; i++) {
            if(newMessages[i]) chatsWithNews++;
        }
        *printNumber_optional = chatsWithNews;
        return chatsWithNews > 0;
    }
}

/** Get chatId by chatIndex**/
const char* getChatId() {
    return chat_ids[chat_index];
}

/** Get the chatIndex by chatId**/
int findChatId(const char* chatId) {
  for(int i = 0; i < max_chats; i++) {
    if(strcmp(chatId, chat_ids[i]) == 0) return i;
  }
  return -1;
}

/**Is this chatId within the contacts?**/
bool isValidChat(const char* chat_id, int* printIndex_optional) {
    for (int i = 0; i < max_chats; i++)
    {
        if(strcmp(chat_id, chat_ids[i]) == 0) {
            if(printIndex_optional != nullptr) *printIndex_optional = i;
            return true;
        }
    }
    return false;
}





// BATTERY & SYSTEM BEHAVIOR ============================

void chargeBattery() {
    if (battery_level < 100) {

        battery.chargeBy(10);
        Serial.printf("Battery level: %d\n", battery_level);
    }
}

void handleBehavior(unsigned long tp_now) {
    unsigned long timeSinceLastFetch = tp_now - tp_lastFetch;

    // Fetch every 11h no matter what, to make sure no messages get lost on telegrams servers (after 24h)
    if (timeSinceLastFetch > INTERVAL_AUTO_FETCH) {
        initTranscoder();
        processTelegramUpdates();
        tp_lastFetch = tp_now;
    }

    // Every hour, if no new messages indicated, if enough battery -> "fetch" a new message
    else if (
        !state_LED &&
        timeSinceLastFetch > 60000/*3600000 /*1h*/ && 
        battery_level >= 60
    ){
        fetchMessages();
    }
}

void handleAutoSleep() {
    unsigned long timeSinceLastInteraction = millis() - tp_lastInteraction_millis;

    if (timeSinceLastInteraction > TIMER_AUTO_SLEEP) {
        goToSleep();
    }
}

void resetAutoSleep() {
    tp_lastInteraction_millis = millis();
}





// NETWORK COMMUNICATION ================================

// RECEIVING 

// "Fake" fetch: Pretend as if going online but only do so if no new messages are stored on device
bool fetchMessages() {
    if (!battery.canFetch()) return false;

    Serial.println("Fetching messages...");

    bool ret = state_LED; // is there already a new message signalized?
    if (!ret) ret = hasChatsWithNews(); // is there already a new message on the device?
    if (!ret) {
        initTranscoder();
        ret = processTelegramUpdates(); // are there new messages online?
    }
    if(ret) setLED(true); // turn on notification light

    battery.subtractUpdate();
    tp_lastFetch = totalMillis + millis();

    return ret;
}

bool processTelegramUpdates() {

    bool ret = false;

    // ensure WiFi connection
    if (!tryConnectWiFi(30000)) return false;

    Serial.println("Telegram API: /getUpdates");

    char getUpdates_url[256];
    snprintf(getUpdates_url, sizeof(getUpdates_url), "https://api.telegram.org/bot%s/getUpdates?offset=%ld&limit=5", BOT_TOKEN, lastUpdateId + 1);
    
    HTTPClient http_getFile;
    http_getFile.begin(client_wifi, getUpdates_url);
    int httpCode = http_getFile.GET();
    
    if (httpCode == HTTP_CODE_OK) {

        WiFiClient* stream = http_getFile.getStreamPtr();
        JsonDocument doc_updates;
        DeserializationError error = deserializeJson(doc_updates, *stream);

        if(!error) {
            JsonArray results = doc_updates["result"].as<JsonArray>();

            if (results.size() == 0) {
                Serial.println("[OK] No new messages found");
            }

            // ensure SD card initialized
            if (!initSDCard()) return false; // trying it here to maybe fix ordering bug...

            // For every new message
            for (JsonObject currentResult : results) {

                lastUpdateId = currentResult["update_id"] | 0;

                if (lastUpdateId == 0) {
                    Serial.print("[ERROR] Update ID was 0");
                    continue;
                }
                
                Serial.printf("\nNEW MESSAGE (Update ID: %ld\n)", lastUpdateId);
                
                JsonObject message = currentResult["message"];
                const char* fromName = message["from"]["first_name"] | "Unknown";
                long long chatId = message["chat"]["id"].as<long long>() | 0;
                
                Serial.printf("From: %s     Chat ID: %lld\n", fromName, chatId);

                char chatId_str[14]; snprintf(chatId_str, sizeof(chatId_str), "%lld", chatId);

                int chatIndex = -1; // gets filled in by isValidChat()
                if (!isValidChat(chatId_str, &chatIndex)) {
                    Serial.println("[ERROR] chat id is unknown");
                    continue;
                }

                // Voice Message?
                if (message.containsKey("voice")) {
                    Serial.println("Type: VoiceMessage");

                    const char* fileId = message["voice"]["file_id"];

                    // Create directory if needed
                    char folderPath[32]; snprintf(folderPath, sizeof(folderPath), "/%lld", chatId); 
                    if(!SD_MMC.exists(folderPath)) SD_MMC.mkdir(folderPath);

                    // Clear all previously recorded messages
                    cleanupChatFolder(folderPath, "recorded_");

                    // Set up target OGG file path with incremented numbering
                    int fileIndex = getNextFileIndex(folderPath, "received_");
                    char oggPath[64];
                    snprintf(oggPath, sizeof(oggPath), "%s/received_%03d.ogg", folderPath, fileIndex);

                    isDownloading = true;

                    // Download Opus/OGG file
                    if (downloadTelegramFile(fileId, oggPath)) {
                        newMessages[chatIndex] = true; // remember this chat has new, unlistened messages

                        char wavPath[MAX_PATH_LEN];
                        convertExtension_ogg2wav(oggPath, wavPath);  // changes ".ogg" to ".wav"

                        // Yield to watchdog before queuing
                        vTaskDelay(pdMS_TO_TICKS(1));

                        // Queue transcode job
                        addTranscodeJob(oggPath, wavPath);  // add job to the transcode queue

                        ret = true;
                    }

                    // Yield to watchdog between files
                    vTaskDelay(pdMS_TO_TICKS(1));
                }

                // Text Message?
                else if (message.containsKey("text")) {
                    const char* text = message["text"];
                    Serial.print("Type: Text -> Content: ");
                    Serial.println(text);
                }

                // Other Message?
                else {
                    Serial.println("Type: OTHER");
                }
            }
        } else {
            Serial.printf("[ERROR] Parsing failed: %s\n", error.c_str());
        }
    } else {
        Serial.println("[ERROR] GET request failed");
    }
    http_getFile.end();
    delay(500);
    disconnectWiFi();

    // allow transcoder task to start after all downloads are finished
    isDownloading = false;
    xSemaphoreGive(startTranscodeGate);

    return ret;
}

// Downloads audio file from Telegram Servers
bool downloadTelegramFile(const char* fileId, const char* destination) {

    // ensure SD card initialized
    if (!initSDCard()) return false;

    Serial.println("Telegram API: /getFile");
    
    char getFile_url[256];
    snprintf(getFile_url, sizeof(getFile_url), "https://api.telegram.org/bot%s/getFile?file_id=%s", BOT_TOKEN, fileId);
    
    HTTPClient http_getFile;
    http_getFile.begin(client_wifi, getFile_url);
    int httpCode = http_getFile.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        WiFiClient* stream = http_getFile.getStreamPtr();
        JsonDocument doc_file;
        DeserializationError error = deserializeJson(doc_file, *stream); 
        
        if (doc_file["ok"]) {
            const char* filePath = doc_file["result"]["file_path"]; 
            char download_url[256];
            snprintf(download_url, sizeof(download_url), "https://api.telegram.org/file/bot%s/%s", BOT_TOKEN, filePath); 
            
            Serial.print("Downloading File URL: ");
            Serial.println(download_url);

            HTTPClient http_download;
            http_download.begin(client_wifi, download_url);
            int downloadCode = http_download.GET();

            if (downloadCode == HTTP_CODE_OK) {
                File f = SD_MMC.open(destination, FILE_WRITE);
                if (f) {
                    http_download.writeToStream(&f);
                    f.close();
                    Serial.println("Download successful.");
                    http_download.end();
                    http_getFile.end();
                    return true;
                }
            }
            http_download.end();
        }
    }
    
    Serial.println("[ERROR] Failed to download file.");
    http_getFile.end();
    return false;
}

bool initSDCard() {
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);

    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("[CRITICAL] SD Card Mount Failed!");
        return false;
    }
    Serial.println("[OK] SD Card Initialized.");
    return true;
}





// SENDING ========================================================

void sendWavFile(const char* filePath, const char* fileName, const char* chat_id) {

    tryConnectWiFi(30000); // ensure WiFi connection

    File f = SD_MMC.open(filePath);
    if (!f) {
        Serial.println("[ERROR] Could not open file for upload");
        return;
    }

    Serial.println("Sending file...");

    size_t fileSize = f.size();
    WiFiClientSecure client_upload;
    client_upload.setInsecure();

    if (!client_upload.connect("api.telegram.org", 443)) {
        Serial.println("[ERROR] Connection to Telegram failed");
        f.close();
        return;
    }

    String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    String partHeader = "--" + boundary + "\r\n"
                        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
                        + chat_id + "\r\n"
                        "--" + boundary + "\r\n"
                        "Content-Disposition: form-data; name=\"document\"; filename=\""
                        + fileName + "\"\r\n"
                        "Content-Type: audio/wav\r\n\r\n";
    String partFooter = "\r\n--" + boundary + "--\r\n";
    size_t totalLength = partHeader.length() + fileSize + partFooter.length();

    client_upload.println("POST /bot" + String(BOT_TOKEN) + "/sendDocument HTTP/1.1");
    client_upload.println("Host: api.telegram.org");
    client_upload.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client_upload.println("Content-Length: " + String(totalLength));
    client_upload.println("Connection: close");
    client_upload.println();
    client_upload.print(partHeader);

    uint8_t buf[512];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        client_upload.write(buf, n);
    }
    f.close();

    client_upload.print(partFooter);

    client_upload.stop();
    Serial.printf("Senden beendet. %s\n", chat_id);

    delay(500);
    disconnectWiFi();
}

bool tryConnectWiFi(int timeout_ms) {    
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[SYSTEM] Connecting to WiFi");
    
    unsigned long startAttemptTime = millis();
    bool led_blink = false;
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < timeout_ms) {
        Serial.print(".");
        led_blink = !led_blink;
        digitalWrite(LED_NOTIFICATION, led_blink);

        delay(500);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[ERROR] WiFi connection failed (Timeout).");

        // LED blink to signalize system
        digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW); delay(100);
        digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW); delay(100);
        digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW); delay(100);
        digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW); delay(100);
        digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW); delay(100);
        digitalWrite(LED_NOTIFICATION, HIGH); delay(100); digitalWrite(LED_NOTIFICATION, LOW);

        return false;
    }
    else {
        Serial.println("\n[OK] WiFi connected.");
        digitalWrite(LED_NOTIFICATION, LOW);

        // Time syncing

        Serial.println("[SYSTEM] Syncing time...");
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        
        time_t now = time(nullptr);
        int retry = 0;
        while (now < 24 * 3600 && retry < 100) { 
            delay(100); 
            now = time(nullptr);
            retry++;
        }
        
        if (now < 24 * 3600) {
            Serial.println("[WARNING] Time sync failed. SSL certificates might fail.");
        } else {
            Serial.println("[OK] Time synced.");
        }
    }

    client_wifi.setInsecure(); 
    return true;
}

void disconnectWiFi() {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("Disconnected WiFi\n");
}





// AUDIO PLAYBACK ==============================================

void startPlayback(const char* filePath) {
    if (isRecording) return;
    if (isTranscoding) return;
    if (isAudioRunning) stopPlayback();
    
    audioFile = SD_MMC.open(filePath);
    if (!audioFile) {
        Serial.print("Error: Could not open ");
        Serial.println(filePath);
        return;
    }

    setI2SData_playback(); // reinstall i2s resources before every playback to avoid desyncing
    stream_wavToI2s.begin(); // Prepare output stream for new file (resets decoder state)
    
    // Point copier to newly opened file and output stream
    copier_playback.begin(stream_wavToI2s, audioFile);
    
    isAudioRunning = true;

    Serial.print("Playback started...");
}

void stopPlayback() {
    if (isAudioRunning) {
        isAudioRunning = false;
        stream_wavToI2s.end(); // Stop the decoder and stream cleanly
        i2s_playback.end(); // uninstall i2s resources
        
        if (audioFile) audioFile.close();

        Serial.println(" --> Done.");

        resetAutoSleep();
    }
}

// keeps the data flow to speaker alive while audio is running
void processAudio() {
    if (isAudioRunning) {
        // Is file still open and has data left?
        if (audioFile && audioFile.available()) {
            copier_playback.copy(); // Copy next chunk of audio
        } else {
            stopPlayback(); 
        }
    }
}

void setI2SData_playback() {

    auto config = i2s_playback.defaultConfig(TX_MODE);
    config.pin_bck = I2S_BCLK;
    config.pin_ws = I2S_LRC;
    config.pin_data = I2S_DOUT;

    i2s_playback.begin(config);
}





// AUDIO RECORDING ======================================

// Set things up for record task
void startRecording() {
    if (!battery.canSend()) return;
    if (isAudioRunning) stopPlayback();

    setI2SData_recording(); // reinstall i2s resources before every recording to avoid desyncing
    delay(200);

    // LED on
    digitalWrite(LED_NOTIFICATION, HIGH);

    isRecording = true;
}

void recordTask(void *pvParameters) {
    AudioBuffer* buf; // Struct that the xQueueReceive call will fill with recorded data
    
    while (true) {
        if (isRecording) {
            // Get an empty buffer from emptyQueue
            if (xQueueReceive(emptyQueue, &buf, pdMS_TO_TICKS(10)) == pdTRUE) {
                // Read audio data from microphone
                size_t bytes_read = i2s_record.readBytes(buf->data, CHUNK_SIZE);
                
                if (bytes_read > 0) {
                    size_t mono_size = 0;
                    const float gain = 1.0f; // Adjust gain factor as needed (e.g., 1.5f for 50% increase)
                    for (size_t i = 0; i < bytes_read; i += 8) { 
                        // A 32-bit stereo frame is 8 bytes: left (4 bytes), right (4 bytes)
                        // Extract left channel as 32-bit signed integer (assuming little-endian)
                        int32_t left = (buf->data[i+3] << 24) | (buf->data[i+2] << 16) | (buf->data[i+1] << 8) | buf->data[i];
                        
                        // Shift right by 8 bits to reduce to 24-bit
                        left >>= 8;
                        
                        // Apply gain
                        left = (int32_t)(left * gain);
                        
                        // Convert to 16-bit by taking the upper 16 bits of the 24-bit value
                        int16_t sample = (int16_t)(left >> 8);

                        // Apply clamping in case of clipping
                        if(sample > 32767) sample = 32767;
                        else if(sample < -32768) sample = -32768;
                        
                        // Write the 16-bit sample to the buffer (little-endian)
                        buf->data[mono_size++] = sample & 0xFF;
                        buf->data[mono_size++] = (sample >> 8) & 0xFF;
                        
                        // Right channel (i+4 to i+7) is ignored
                    }
                    buf->size = mono_size;
                    // Send filled buffer to the writing task
                    xQueueSend(fullQueue, &buf, portMAX_DELAY);
                } else {
                    // Nothing read, return buffer to empty queue
                    xQueueSend(emptyQueue, &buf, portMAX_DELAY);
                }
            } else {
                Serial.println("Warning: Queue Full/Buffer Overflow! SD card too slow.");
                // Prevent task watchdogs from panicking during overflow
                vTaskDelay(pdMS_TO_TICKS(5)); 
            }
        } else {
            // Sleep briefly when not recording
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// FreeRTOS task to store recorded bytes on sd card
void sdWriteTask(void *pvParameters) {
    AudioBuffer* buf;
    
    while (true) {
        if (isRecording) {
            // Check if we just started recording
            if (!wasRecording) {

                // Build folderpath and ensure it exists
                const char *chatId = getChatId();
                char folderPath[24];
                snprintf(folderPath, sizeof(folderPath), "/%s", chatId);

                if (!SD_MMC.exists(folderPath))
                    SD_MMC.mkdir(folderPath);

                // Create wav filepath with increased index
                int fileIndex = getNextFileIndex(folderPath, "recorded_");
                snprintf(currentFilePath, sizeof(currentFilePath), "%s/recorded_%03d.wav", folderPath, fileIndex);
                audioFile_record = SD_MMC.open(currentFilePath, FILE_WRITE);

                // Clear previously received messages in folder
                cleanupChatFolder(folderPath, "received_");

                // Start recording
                if (audioFile_record) {
                    encoderWav_record.setAudioInfo(info_WavEncoder);
                    stream_wavToFile.begin(info_WavEncoder); // WAV header must match the mono 16-bit PCM we actually write
                    wasRecording = true;
                    Serial.println("Recording started...");
                }
                else {
                    Serial.println("Failed to open file for writing!");
                    isRecording = false; // Abort
                }
            }

            // Wait for a filled buffer (timeout 50ms to allow checking isRecording again)
            if (xQueueReceive(fullQueue, &buf, pdMS_TO_TICKS(50)) == pdTRUE) {
                // Write audio data to SD Card
                stream_wavToFile.write(buf->data, buf->size);
                
                // Return buffer to empty queue for reuse
                xQueueSend(emptyQueue, &buf, portMAX_DELAY);
            }
            
        } else {
            // Check if we just stopped recording
            if (wasRecording) {

                // Flush any remaining buffers in the queue
                while (xQueueReceive(fullQueue, &buf, 0) == pdTRUE) {
                    stream_wavToFile.write(buf->data, buf->size);
                    xQueueSend(emptyQueue, &buf, portMAX_DELAY);
                }
                
                // Finalize the WAV file (rewrites header with final RIFF size)
                stream_wavToFile.end();
                audioFile_record.close();
                wasRecording = false;
                
                Serial.println("Recording stopped.");
            }
            // Sleep briefly when not recording
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// Tie things up after recording is done
void stopRecording() {
    if (!isRecording) return; // Button was released but recording couldn't even be started

    resetAutoSleep();

    isRecording = false;

    // LED off
    digitalWrite(LED_NOTIFICATION, LOW);

    // Give a little time and send as soon as recording actually stopped
    unsigned long tp = millis();
    while (millis() - tp < 2000) {
        if(!wasRecording) {
            sendWavFile(currentFilePath, "message.wav", getChatId());
            battery.subtractSend();
            return;
        }
    }

    i2s_record.end(); // uninstall i2s resources
    
    Serial.println("[ERROR] Sending failed.");
    battery.subtractSend();
}

void setI2SData_recording() {

    auto config = i2s_record.defaultConfig(RX_MODE);
    config.sample_rate = info_recording.sample_rate;
    config.channels = info_recording.channels;
    config.bits_per_sample = info_recording.bits_per_sample;
    config.i2s_format = I2S_STD_FORMAT;
    config.pin_ws = I2S_LRC;
    config.pin_bck = I2S_BCLK;
    config.pin_data = I2S_DIN;
    config.pin_data_rx = I2S_DIN;

    i2s_record.begin(config);
}





// AUDIO TRANSCODING ==========================================

void initTranscoder() {
    if(transcoderInitialized) return;
    
    // Initialize transcoding system for potential message processing
    // Create the gate (binary semaphore starts "taken")
    startTranscodeGate = xSemaphoreCreateBinary();

    AudioInfo info_transcoding;
    info_transcoding.sample_rate = 48000;
    info_transcoding.channels = 1;
    info_transcoding.bits_per_sample = 16;

    decoder_opusOgg.setAudioInfo(info_transcoding);
    encoder_wav.setAudioInfo(info_transcoding);

    // Create a queue capable of holding up to 5 pending transcode jobs
    jobQueue = xQueueCreate(5, sizeof(TranscodeJob));

    // Create the persistent FreeRTOS Task
    xTaskCreatePinnedToCore(transcodeTask, "TranscodeTask", 16384, NULL, 1, &transcodeTaskHandle, 1);

    transcoderInitialized = true;
}

void addTranscodeJob(const char* oggFile, const char* wavFile) {
    isTranscoding = true;
    TranscodeJob newJob;
    strncpy(newJob.inFile, oggFile, sizeof(newJob.inFile));
    strncpy(newJob.outFile, wavFile, sizeof(newJob.outFile));
    if (xQueueSend(jobQueue, &newJob, pdMS_TO_TICKS(2000)) != pdTRUE) {
        Serial.println("[ERROR] Failed to queue transcode job - queue full or timeout");
        isTranscoding = false;
    }
}

// Transcode OGG/Opus file into Wav File
void transcodeTask(void *pvParameters) {

    TranscodeJob job;
    
    while (true) {

        // Block until the current download batch is complete.
        xSemaphoreTake(startTranscodeGate, portMAX_DELAY);

        // Process all jobs that were enqueued during the batch.
        while (xQueueReceive(jobQueue, &job, 0) == pdPASS) {

            Serial.printf("\n[Transcoder] Started: %s -> %s\n", job.inFile, job.outFile);
            isTranscoding = true;

            audioFileIn = SD_MMC.open(job.inFile, FILE_READ);
            audioFileOut = SD_MMC.open(job.outFile, FILE_WRITE);

            if (!audioFileIn || !audioFileOut) {
                Serial.println("[Transcoder] Error: Failed to open files.");
                if (audioFileIn) audioFileIn.close();
                if (audioFileOut) audioFileOut.close();
                continue; // Skip this job and wait for the next one
            }

            // Set up audio information
            AudioInfo info_transcoding(48000, 1, 16);
            decoder_opusOgg.addNotifyAudioChange(encoder_wav); // Decoder notifies the encoder in case ogg file has different audio info

            // Ensure PSRAM is initialized and available
            if (!psramInit()) {
                Serial.println("ERROR: PSRAM failed to initialize. Aborting transcoding.");
                continue;
            }

            stream_OggToFileIn.resizeReadResultQueue(131072); // Large enough to hold raw PCM bytes worth of one page of decoded ogg/opus bytes

            stream_OggToFileIn.begin(info_transcoding);
            stream_WavToFileOut.begin(info_transcoding);

            int flushCounter = 0;

            // Main Transcoding Loop
            while (true) {
                size_t bytesCopied = copier_transcode.copy();

                Serial.print(".");

                // Handle the start-up delay and end-of-file buffer flushing
                if (bytesCopied == 0) {
                    // Check if the input file has data left.
                    // Yes? Do nothing. Decoder is likely parsing headers or spinning up.
                    if (audioFileIn.available() == 0) {
                        // No? File is read. Tick a counter to let the copier_transcode flush trapped PCM data.
                        flushCounter++;
                        if (flushCounter > 10) break;
                    }
                } 
                else flushCounter = 0; // Reset counter if all data was moved

                // Yield to FreeRTOS watchdog
                vTaskDelay(pdMS_TO_TICKS(2)); 
            }

            // Cleanup memory and close files for this job
            stream_WavToFileOut.end();
            stream_OggToFileIn.end();
            audioFileIn.close();
            audioFileOut.close();

            // Remove transcoded ogg file from card
            SD_MMC.remove(job.inFile);

            isTranscoding = false;

            Serial.println("\n[Transcoder] Transcoding finished.");

            resetAutoSleep();
        }
    }
}