#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
//#include <Audio.h>
#include <ArduinoJson.h>
#include <driver/i2s_std.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// --- NEW DECODING LIBRARIES ---
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecWAV.h"     // WAV encoding
#include "AudioTools/AudioCodecs/CodecOpusOgg.h" // Combined Opus + OGG integration

#include "../Shared/arduino_secrets.h"

// ------------------------------

// KONFIGURATION 


int chat_index = 0;
bool newMessages[max_chats] = {false};

// I2S Pins
#define I2S_DOUT  8
#define I2S_DIN   7
#define I2S_BCLK  5
#define I2S_LRC   6

#define SAMPLE_RATE 16000

// SD Card Pin
#define SD_CS     D10

// Buttons
const unsigned long SHORT_PRESS_TIME = 500; // Threshold for long press

using ButtonEvent = void (*)();

// Forward Declarations
void playNewMessage();
void startRecording();
void finishRecording();
void dialUp();
void doNothing();
void playLastMessagesWrapper();
void playLastMessages(const char* chatId);
bool isValidChat(const char* chat_id, int* printIndex = nullptr);
bool downloadTelegramFile(const char* fileId, const char* destination);
bool transcodeOggToWav(const char* oggPath, const char* wavPath);
void cleanupChatFolder(const char* folderPath, const char* prefix);
void writeWavHeader(File file, uint32_t fileSize);
void sendWavFile(const char* filePath, const char* fileName, const char* chat_id);
bool processTelegramUpdates();
void startPlayback(char* filePath);
const char* getChatId();
int findChatId(const char* chatId);

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

#define BUTTON A5
#define BUTTON_DIAL A4

Button button = {BUTTON, HIGH, 0, false, false, playNewMessage, startRecording, finishRecording};
Button button_dial = {BUTTON_DIAL, HIGH, 0, false, false, dialUp, playLastMessagesWrapper, doNothing};

// Playback Queue

#define QUEUE_SIZE 5 // maximum expected number of files
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
        if (count == 0) return false; // Queue empty
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
};

PlaybackQueue playbackQueue;

// Globals
WiFiClientSecure client_wifi;
Audio audio;

long lastUpdateId = 0;
int messageCount = 0;
i2s_chan_handle_t rx_handle = NULL;

QueueHandle_t audioQueue;
volatile bool isRecording = false;
File audioFile;
char currentFilePath[64]; // where the file is stored, that is being recorded
uint32_t bytesRecorded = 0;

// BUTTON-CONTROL 

// Helper function to prevent crash if an event is unassigned
void doNothing() {}

void handleButton(Button &btn, const char* name) {
  bool currentState = digitalRead(btn.pin);

  // Button Pressed (Falling edge)
  if (currentState == LOW && btn.lastState == HIGH) {
    btn.pressStartTime = millis();
    btn.isPressed = true;
    btn.isHolding = false;
    delay(20); // Basic debounce
  }

  // Button Released (Rising edge)
  if (currentState == HIGH && btn.lastState == LOW) {
    if (!btn.isHolding) {
      Serial.print(name); Serial.println(": Short Press");
      if (btn.onShortPress) btn.onShortPress();
    } 
    else { 
      if (btn.onRelease) btn.onRelease();
    }
    btn.isPressed = false;
  }

  // Check for Long Press
  if (btn.isPressed && !btn.isHolding && (millis() - btn.pressStartTime > SHORT_PRESS_TIME)) {
    Serial.print(name); Serial.println(": Long Press (Hold)");
    if (btn.onHold) btn.onHold();
    btn.isHolding = true;
  }

  btn.lastState = currentState;
}

void playNewMessage() {
    if(audio.isRunning()) return;

    // See if there are chats that have unlistened messages in them
    int chatsWithNews = 0; // Number of chats with new messages
    for(int i = 0; i < max_chats; i++) {
        if(newMessages[i]) chatsWithNews++;
    }

    // If yes, pick a random one of them
    if(chatsWithNews > 0) {
        int chats_indices[max_chats];
        int j = 0;
        for(int i = 0; i < max_chats; i++) {
            if(newMessages[i]) chats_indices[j++] = i;
        }
        int randomChatIndex = millis() % chatsWithNews;
        playLastMessages(chat_ids[chats_indices[randomChatIndex]]);
    } 
    else {
        if(processTelegramUpdates()) playNewMessage();
        else Serial.println("[MESSAGE] No new messages :(");
    }
}

void dialUp() { 
    if(isRecording) return;

    chat_index = (chat_index + 1) % max_chats;
    Serial.print("[DIAL] Changed chat to index: "); Serial.println(chat_index);
    Serial.print("New Messages: "); 
    for(int i = 0; i < 5; i++) {
      Serial.print(newMessages[i]);
    }
    Serial.println("");
}

void playLastMessagesWrapper() {
  playLastMessages(getChatId());
}

void playLastMessages(const char* chatId) {
    if (isRecording) return;
    if (audio.isRunning()) audio.stopSong();

    char folderPath[24]; snprintf(folderPath, sizeof(folderPath), "/%s", chatId);

    File dir = SD.open(folderPath);
    if (!dir) return;

    playbackQueue.clear(); // Clear any pending items in the queue

    while (File entry = dir.openNextFile()) {
        if (!entry.isDirectory()) {
            const char* name = entry.name();
            
            // ONLY play files that are transcoded received messages (ignore outgoing recordings)
            if (strncmp(name, "received_", 9) == 0 && strstr(name, ".wav") != nullptr) {
                char filePath[64];
                snprintf(filePath, sizeof(filePath), "%s/%s", folderPath, name);
                playbackQueue.push(filePath);
            }
        }
        entry.close();
    }

    dir.close();
    newMessages[findChatId(chatId)] = false;
}

// Only ever keep all the last received OR all the last sent messages
void cleanupChatFolder(const char* folderPath, const char* prefix) {
    File dir = SD.open(folderPath);
    if (!dir) return;

    while (File entry = dir.openNextFile()) {
        if (!entry.isDirectory()) {
            const char* name = entry.name();

            // Check if the filename starts with the required prefix
            if (strncmp(name, prefix, strlen(prefix)) == 0) {
                char filePath[64];
                snprintf(filePath, sizeof(filePath), "%s/%s", folderPath, name);
                SD.remove(filePath);
            }
        }
        entry.close();
    }
    dir.close();
}

const char* getChatId() {
    return chat_ids[chat_index];
}

int findChatId(const char* chatId) {
  for(int i = 0; i < max_chats; i++) {
    if(strcmp(chatId, chat_ids[i]) == 0) return i;
  }
  return -1;
}

// --- TRANSCODING (OFFLINE PROCESS) ---

bool transcodeOggToWav(const char* oggPath, const char* wavPath) {
    Serial.println("[TRANSCODE] Starting OGG/Opus to WAV...");
    
    // Use the standard SD library classes
    File inFile = SD.open(oggPath, FILE_READ);
    File outFile = SD.open(wavPath, FILE_WRITE);
    
    if (!inFile || !outFile) {
        if (inFile) inFile.close();
        if (outFile) outFile.close();
        Serial.println("[TRANSCODE] Failed to open files!");
        return false;
    }

    // We use '::audio_tools' to force the compiler to look at the 
    // root namespace and ignore the 'ambiguous' nested ones.
    
    ::audio_tools::OpusOggDecoder opusDecoder;
    ::audio_tools::WAVEncoder wavEncoder;
    
    // EncodedAudioStream needs to know the specific types to avoid 'incomplete type' errors
    ::audio_tools::EncodedAudioStream inStream(&inFile, &opusDecoder);
    ::audio_tools::EncodedAudioStream outStream(&outFile, &wavEncoder);
    
    ::audio_tools::StreamCopy copier(outStream, inStream);

    // IMPORTANT: Opus usually requires a specific start. 
    // We pass default settings to ensure 'begin' is fully realized.
    auto opusSettings = opusDecoder.config();
    inStream.begin(opusSettings); 
    outStream.begin(); 

    Serial.println("[TRANSCODE] Copying...");

    // Perform the copy
    while (copier.copy() > 0) {
        yield(); 
    }
    
    // Cleanup and Header Finalization
    outStream.end();
    inStream.end();
    
    inFile.close();
    outFile.close();
    
    Serial.println("[TRANSCODE] Done!");
    return true;
}

// SENDING 

// --- I2S MANAGEMENT ---

void setupI2S_record() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, NULL, &rx_handle);

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_BCLK,
      .ws = (gpio_num_t)I2S_LRC,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)I2S_DIN,
    }
  };

  i2s_channel_init_std_mode(rx_handle, &std_cfg);
  i2s_channel_enable(rx_handle);
}

void stopI2S_record() {
  if (rx_handle) {
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    rx_handle = NULL;
  }
}

// --- AUDIO AUFNAHME ---

void startRecording() {
  if (audio.isRunning()) audio.stopSong();

  const char* chatId = getChatId();

  char folderPath[24]; snprintf(folderPath, sizeof(folderPath), "/%s", chatId);
  if (!SD.exists(folderPath)) {
    if (!SD.mkdir(folderPath)) {
      Serial.println("[ERROR] Failed to create directory on SD");
      return;
    }
  }

  // Clear previously recorded messages so they don't pile up
  cleanupChatFolder(folderPath, "recorded_");

  snprintf(currentFilePath, sizeof(currentFilePath), "%s/recorded_%ld.wav", folderPath, random(1000)); 
  audioFile = SD.open(currentFilePath, FILE_WRITE);

  uint8_t header[44] = { 0 };
  audioFile.write(header, 44);
  bytesRecorded = 0;
  isRecording = true;
  setupI2S_record();

  Serial.println("Aufnahme gestartet...");
}

void recordTask(void *pvParameters) {
  while (true) {
    if (isRecording) {
      int16_t buffer[128];
      size_t bytesRead = 0;
      if(i2s_channel_read(rx_handle, buffer, sizeof(buffer), &bytesRead, 100) == ESP_OK) {
        int16_t monoBuffer[64];
        for(int i = 0; i < (bytesRead / 4); i++) {
            monoBuffer[i] = buffer[i * 2];
        }
        xQueueSend(audioQueue, monoBuffer, pdMS_TO_TICKS(10));
      } else {
        vTaskDelay(pdMS_TO_TICKS(50)); 
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(50)); 
    }
  }
}

void sdWriteTask(void *pvParameters) {
  while (true) {
    if(isRecording) {
        int16_t buffer[64];
        if (xQueueReceive(audioQueue, buffer, pdMS_TO_TICKS(100))) {
            audioFile.write((uint8_t*)buffer, sizeof(buffer));
            bytesRecorded += sizeof(buffer);
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(50)); 
    }
  }
}

void finishRecording() {
  isRecording = false;
  stopI2S_record();
  audioFile.seek(0);
  writeWavHeader(audioFile, bytesRecorded);
  audioFile.close();
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);

  Serial.println("Aufnahme beendet. Senden wird vorbereitet...");
  sendWavFile(currentFilePath, "message.wav", getChatId());
}

void writeWavHeader(File file, uint32_t fileSize) {
  uint32_t sampleRate = SAMPLE_RATE;
  uint16_t numChannels = 1;
  uint16_t bitsPerSample = 16;
  uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
  file.write((const uint8_t*)"RIFF", 4);
  uint32_t chunkSize = fileSize + 36;
  file.write((const uint8_t*)&chunkSize, 4);
  file.write((const uint8_t*)"WAVE", 4);
  file.write((const uint8_t*)"fmt ", 4);
  uint32_t subchunk1Size = 16;
  file.write((const uint8_t*)&subchunk1Size, 4);
  uint16_t audioFormat = 1;
  file.write((const uint8_t*)&audioFormat, 2);
  file.write((const uint8_t*)&numChannels, 2);
  file.write((const uint8_t*)&sampleRate, 4);
  file.write((const uint8_t*)&byteRate, 4);
  uint16_t blockAlign = numChannels * (bitsPerSample / 8);
  file.write((const uint8_t*)&blockAlign, 2);
  file.write((const uint8_t*)&bitsPerSample, 2);
  file.write((const uint8_t*)"data", 4);
  file.write((const uint8_t*)&fileSize, 4);
}

// --- TELEGRAM UPLOAD ---

void sendWavFile(const char* filePath, const char* fileName, const char* chat_id) {
  File f = SD.open(filePath);
  if (!f) {
    Serial.println("[ERROR] Could not open file for upload");
    return;
  }

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
  Serial.println("Senden beendet.");
}

// RECEIVING 

bool processTelegramUpdates() {
    Serial.println("Telegram API: /getUpdates");

    bool ret = false;

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
            
            for (JsonObject currentResult : results) {

                lastUpdateId = currentResult["update_id"] | 0;

                if (lastUpdateId == 0) {
                    Serial.print("[ERROR] Update ID was 0");
                    continue;
                }
                
                Serial.print("\nNEW MESSAGE (Update ID: ");
                Serial.print(lastUpdateId); Serial.println(")");
                
                JsonObject message = currentResult["message"];
                const char* fromName = message["from"]["first_name"] | "Unknown";
                long long chatId = message["chat"]["id"].as<long long>() | 0;
                
                Serial.print("From: "); Serial.print(fromName);
                Serial.print("      Chat ID: "); Serial.println(chatId);

                char chatId_str[14]; snprintf(chatId_str, sizeof(chatId_str), "%lld", chatId);

                int chatIndex = -1;
                if(!isValidChat(chatId_str, &chatIndex)) {
                    Serial.println("[ERROR] chat id is unknown");
                    continue;
                } 

                if (message.containsKey("voice")) {
                    Serial.println("Type: VoiceMessage");
                    const char* fileId = message["voice"]["file_id"];

                    // Create directory if needed
                    char folderPath[32]; snprintf(folderPath, sizeof(folderPath), "/%lld", chatId); 
                    if(!SD.exists(folderPath)) SD.mkdir(folderPath);

                    // Remove all previously received messages (keeps SD clean)
                    cleanupChatFolder(folderPath, "received_");

                    // Download raw Opus/OGG first
                    char oggFilePath[64];
                    snprintf(oggFilePath, sizeof(oggFilePath), "%s/received_%ld.ogg", folderPath, lastUpdateId);

                    if (downloadTelegramFile(fileId, oggFilePath)) {
                        
                        // Set up target WAV file path
                        char wavFilePath[64];
                        snprintf(wavFilePath, sizeof(wavFilePath), "%s/received_%ld.wav", folderPath, lastUpdateId);
                        
                        // Run Offline Transcode
                        if (transcodeOggToWav(oggFilePath, wavFilePath)) {
                            SD.remove(oggFilePath); // Delete original compressed file
                            newMessages[chatIndex] = true;
                            ret = true;
                        } else {
                            Serial.println("[ERROR] Voice message decoding failed. Corrupt file?");
                            SD.remove(oggFilePath);
                            SD.remove(wavFilePath);
                        }
                    }
                } 
                else if (message.containsKey("text")) {
                    const char* text = message["text"];
                    Serial.print("Type: Text -> Content: ");
                    Serial.println(text);
                }
                else {
                    Serial.println("Type: OTHER");
                }
                Serial.println("----------------------------\n");
            }
        } else {
            Serial.print("[ERROR] Parsing failed: "); Serial.println(error.c_str());
        }
    }
    http_getFile.end();

    return ret;
}

// Downloads audio file from Telegram Servers
bool downloadTelegramFile(const char* fileId, const char* destination) {
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
                File f = SD.open(destination, FILE_WRITE);
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

// --- PLAYBACK ---

void startPlayback(char* filePath) {
    Serial.println("Starting Playback...");
    audio.connecttoFS(SD, filePath); // Playback starts
}

// SETUP & LOOP 

void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(BUTTON, INPUT_PULLUP);
    pinMode(BUTTON_DIAL, INPUT_PULLUP);
    
    Serial.println("\n[SYSTEM] Initializing...");
    
    if (!SD.begin(SD_CS)) {
        Serial.println("[CRITICAL] SD Card Mount Failed!");
        while (1);
    }
    Serial.println("[OK] SD Card Initialized.");

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[SYSTEM] Connecting to WiFi");
    
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[ERROR] WiFi connection failed (Timeout).");
    } else {
        Serial.println("\n[OK] WiFi connected.");
        
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

    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(50);

    audioQueue = xQueueCreate(20, 64 * sizeof(int16_t));
    xTaskCreate(recordTask, "Capture", 4096, NULL, 5, NULL);
    xTaskCreate(sdWriteTask, "Writer", 4096, NULL, 3, NULL);

    Serial.println("[SYSTEM] Ready. Waiting for Telegram messages...");
}

void loop() {
    audio.loop();

    handleButton(button, "button");
    handleButton(button_dial, "dial");

    if(!audio.isRunning() && playbackQueue.count > 0) {
        static char filePlaying[MAX_PATH_LEN];
        playbackQueue.pop(filePlaying);
        startPlayback(filePlaying);
    }
}

// UTILITY 

bool isValidChat(const char* chat_id, int* printIndex /*optional*/) {
    for (int i = 0; i < max_chats; i++)
    {
        if(strcmp(chat_id, chat_ids[i]) == 0) {
            if(printIndex != nullptr) *printIndex = i;
            return true;
        }
    }
    return false;
}

void audio_eof_mp3(const char *info){  // Wird am Ende einer Datei aufgerufen
    Serial.print("Playback beendet.");
    Serial.println(info);
}

void audio_info(const char *info){
    Serial.print("audio_info: "); Serial.println(info);
}