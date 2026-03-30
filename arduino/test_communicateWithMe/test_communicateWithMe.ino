#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <driver/i2s_std.h>
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusOgg.h"

// Bot Token, Chat-IDs, WiFi credentials
#include "../Shared/arduino_secrets.h"

// ========================================================================================

// Forward Declarations
void playNewMessage();
void startRecording();
void finishRecording();
void dialUp();
void doNothing();
void playLastMessagesWrapper();
void playLastMessages(const char* chatId, bool transcodeOgg);
bool isValidChat(const char* chat_id, int* printIndex = nullptr);
bool downloadTelegramFile(const char* fileId, const char* destination);
bool transcodeOggToWav(const char* oggPath, const char* wavPath);
void cleanupChatFolder(const char* folderPath, const char* prefix);
void sendWavFile(const char* filePath, const char* fileName, const char* chat_id);
bool processTelegramUpdates();
void startPlayback(const char* filePath);
const char* getChatId();
int findChatId(const char* chatId);

// ==========================================================================================

// COMMUNICATION =======================

// I2S Pins
#define I2S_DOUT  8
#define I2S_DIN   7
#define I2S_BCLK  5
#define I2S_LRC   6

// SD Card Pin
#define SD_CS     D10

#define SAMPLE_RATE 16000


// BUTTONS ==============================

#define BUTTON A5
#define BUTTON_DIAL A4

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

Button button = {BUTTON, HIGH, 0, false, false, playNewMessage, startRecording, finishRecording};
Button button_dial = {BUTTON_DIAL, HIGH, 0, false, false, dialUp, playLastMessagesWrapper, doNothing};


// PLAYBACK ==============================

#define QUEUE_SIZE 20 // maximum expected number of files
#define MAX_PATH_LEN 64 // maximum path length

struct PlaybackQueue {
    char queue[QUEUE_SIZE][MAX_PATH_LEN];
    int head = 0; // Where we add new items
    int tail = 0; // Where we read items
    int count = 0; // Current number of items
    int waitCount = 0; // how many files are we waiting on?
    char waitingOnChatId [12] = "";

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

        waitCount = 0;
        snprintf(waitingOnChatId, sizeof(waitingOnChatId), "%s", "");
    }

    bool hasNext() {
        if(count > 0 && waitCount == 0) return true;
        return false;
    }

    bool pushAndWaitFor(const char* path, const char* chatId) {
        if(waitCount > 0 && strcmp(waitingOnChatId, chatId) != 0) return false; // If already waiting and this chatId isn't already waited for

        if(push(path)) {
            snprintf(waitingOnChatId, sizeof(waitingOnChatId), "%s", chatId);
            waitCount++;
            return true;
        }
        return false;
    }

    void signalizeFileReady(const char* wavPath) {
        if (strstr(wavPath, waitingOnChatId) != nullptr) waitCount--; // If we waited for this chat
        if (waitCount == 0) snprintf(waitingOnChatId, sizeof(waitingOnChatId), "%s", ""); // If everything is here, forget chatId
    }
};

PlaybackQueue playbackQueue;

int chat_index = 0;
bool newMessages[max_chats] = {false};


// Audio Player

bool isAudioRunning = false;
File audioFile;

I2SStream i2s_playback;
WAVDecoder decoder_wav;
EncodedAudioStream stream_wavToI2s(&i2s_playback, &decoder_wav); // data written to 'stream_wavToI2s' gets decoded and sent to I2S.
StreamCopy copier_playback(stream_wavToI2s, audioFile); // pulls bytes from sd-file and pushes them to stream_wavToI2s


// RECORDING =========================
// Mic → I2SStream → AudioStream → WAVEncoder → File

I2SStream i2s_record;

WAVEncoder encoderWav_record;
AudioInfo info_WavEncoder(16000, 1, 16); // [sample-rate, num_channels, bits per sample]

File audioFile_record;
EncodedAudioStream stream_wavToFile(&audioFile_record, &encoderWav_record);
AudioInfo info_recording(16000, 2, 16); // for the I2S Stream [sample-rate, num_channels, bits per sample]

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


// TRANSCODING ============================
// file.ogg --> OpusOggDecoder --StreamCopy--> WavEncoder --> File.wav

// Struct to pass jobs to the FreeRTOS task
struct TranscodeJob {
    char inFile[MAX_PATH_LEN];
    char outFile[MAX_PATH_LEN];
};

// Transcoding Queue
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


// NETWORKING =========================

WiFiClientSecure client_wifi;

long lastUpdateId = 0;
i2s_chan_handle_t rx_handle = NULL;



// =============================================================================================
// =============================================================================================



// SETUP & LOOP =======================

void setup() {
    Serial.begin(115200);
    while (!Serial);

    // Pins
    pinMode(BUTTON, INPUT_PULLUP);
    pinMode(BUTTON_DIAL, INPUT_PULLUP);

    AudioLogger::instance().begin(Serial, AudioLogger::Debug);
    
    Serial.println("\n[SYSTEM] Initializing...");
    

    // SD card mount -----------------------------
    if (!SD.begin(SD_CS)) {
        Serial.println("[CRITICAL] SD Card Mount Failed!");
        while (1);
    }
    Serial.println("[OK] SD Card Initialized.");


    if (!connectWiFi()) while(1);

    setI2SData_playback();


    // Audio Recording -----------------------

    emptyQueue = xQueueCreate(NUM_CHUNKS, sizeof(AudioBuffer*));
    fullQueue = xQueueCreate(NUM_CHUNKS, sizeof(AudioBuffer*));
    
    // Fill up emptyQueue with (yet undefined) buffers
    for (int i = 0; i < NUM_CHUNKS; i++) {
        AudioBuffer* ptr = &buffers[i];
        xQueueSend(emptyQueue, &ptr, portMAX_DELAY);
    }

    setI2SData_recording();

    // Create FreeRTOS Tasks
    xTaskCreate(recordTask, "RecordTask", 4096, NULL, 5, NULL);
    xTaskCreate(sdWriteTask, "SDWriteTask", 8192, NULL, 3, NULL);


    // Audio Transcoding -------------------------

    AudioInfo info_transcoding;
    info_transcoding.sample_rate = 48000;
    info_transcoding.channels = 1;
    info_transcoding.bits_per_sample = 16;

    decoder_opusOgg.setAudioInfo(info_transcoding);
    encoder_wav.setAudioInfo(info_transcoding);

    // Create a queue capable of holding up to 5 pending transcode jobs
    jobQueue = xQueueCreate(5, sizeof(TranscodeJob));

    // Create the persistent FreeRTOS Task
    xTaskCreatePinnedToCore(
        transcodeTask,
        "TranscodeTask",
        16384,               // Ogg+Opus needs a hefty stack, bumped to 32k to be safe
        NULL,
        1,
        &transcodeTaskHandle,
        1                    // Pin to Core 1
    );


    Serial.println("[SYSTEM] Ready. Waiting for Telegram messages...");
}

void loop() {
    processAudio();
    handleButton(button, "button");
    handleButton(button_dial, "dial");

    if(!isAudioRunning && playbackQueue.hasNext()) {
        static char filePlaying[MAX_PATH_LEN];
        playbackQueue.pop(filePlaying);
        startPlayback(filePlaying);
    }

    //vTaskDelay(pdMS_TO_TICKS(100));
}





// USER CONTROL ==============================

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

void doNothing() {} // Helper function to prevent crash if an event is unassigned

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





// CHAT & PLAYBACK CONTROL ==============================

void playNewMessage() {
    if(isAudioRunning) return;
    if(isRecording) return;

    // See if there already are chats that have unlistened messages in them
    int chatsWithNews = 0; // Number of chats with new messages
    for(int i = 0; i < max_chats; i++) {
        if(newMessages[i]) chatsWithNews++;
    }

    // If yes, pick a random one of them
    if(chatsWithNews > 0) {
        int chats_indices[max_chats]; // will hold the chatIndices of all chats containing unlistened messages
        int j = 0;
        for(int i = 0; i < max_chats; i++) {
            if(newMessages[i]) chats_indices[j++] = i; // fill array
        }
        int randomChatIndex = millis() % chatsWithNews; // pick a random chat

        chat_index = randomChatIndex;

        playLastMessages(chat_ids[chats_indices[randomChatIndex]], true); // play it back, transcode if necessary
    }

    // If no, fetch telegram servers
    else {
        if(processTelegramUpdates()) playNewMessage(); // fetch and if new messages, try again
        else Serial.println("[MESSAGE] No new messages :(");
    }
}

void playLastMessagesWrapper() {
  playLastMessages(getChatId(), true);
}

// Play the last messages in a chat folder, transcode them if specified
void playLastMessages(const char* chatId, bool transcodeOgg) {
    if (isRecording) return;
    if (isAudioRunning) stopPlayback();

    char folderPath[24]; snprintf(folderPath, sizeof(folderPath), "/%s", chatId);

    File dir = SD.open(folderPath);
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

        // If file should be transcoded before playing
        if (transcodeOgg && strstr(name, ".ogg") != nullptr) {
            // Prepare file paths
            char oggPath[MAX_PATH_LEN];
            snprintf(oggPath, sizeof(oggPath), "%s/%s", folderPath, name);
            char wavPath[MAX_PATH_LEN];
            convertExtension_ogg2wav(oggPath, wavPath);  // changes ".ogg" to ".wav"

            // Queue transcode job
            addTranscodeJob(oggPath, wavPath);  // add job to the queue

            // Add to playback queue but wait for transcoding to finish
            playbackQueue.pushAndWaitFor(wavPath, chatId);
        }

        // If file is a wav
        else if (strstr(name, ".wav") != nullptr) {
            char filePath[MAX_PATH_LEN];
            snprintf(filePath, sizeof(filePath), "%s/%s", folderPath, name);
            playbackQueue.push(filePath);
        }
    }

    newMessages[findChatId(chatId)] = false; // messages in this chat were all listened to
}

void convertExtension_ogg2wav(const char* oggStr, char* wavStr) {
    if (!oggStr || !wavStr) return;

    // Copy source to destination safely
    strcpy(wavStr, oggStr);

    size_t len = strlen(wavStr);
    if (len >= 4) {
        // Just overwrite the end of the existing buffer
        strcpy(wavStr + len - 4, ".wav");
    }
}

// Scans a folder to find the next available sequential number for a specific prefix
int getNextFileIndex(const char* folderPath, const char* prefix) {
    File dir = SD.open(folderPath);
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

// Only ever keep all the last received OR all the last recorded messages in the chat folder
void cleanupChatFolder(const char* folderPath, const char* prefix) {
    File dir = SD.open(folderPath);
    if (!dir) return;

    while (File entry = dir.openNextFile()) {
        if (!entry.isDirectory()) {
            const char* name = entry.name();

            // Check if the filename starts with the required prefix
            if (strncmp(name, prefix, strlen(prefix)) == 0) {
                char filePath[MAX_PATH_LEN];
                snprintf(filePath, sizeof(filePath), "%s/%s", folderPath, name);
                SD.remove(filePath);
            }
        }
        entry.close();
    }
    dir.close();
}

// Get chatId by chatIndex
const char* getChatId() {
    return chat_ids[chat_index];
}

// Get the chatIndex by chatId
int findChatId(const char* chatId) {
  for(int i = 0; i < max_chats; i++) {
    if(strcmp(chatId, chat_ids[i]) == 0) return i;
  }
  return -1;
}

// Is this chatId within the contacts?
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





// TELEGRAM COMMS ================================

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
                    if(!SD.exists(folderPath)) SD.mkdir(folderPath);

                    // Clear all previously recorded messages
                    cleanupChatFolder(folderPath, "recorded_");

                    // Set up target OGG file path with incremented numbering
                    int fileIndex = getNextFileIndex(folderPath, "received_");
                    char oggFilePath[64];
                    snprintf(oggFilePath, sizeof(oggFilePath), "%s/received_%03d.ogg", folderPath, fileIndex);

                    // Download Opus/OGG file
                    if (downloadTelegramFile(fileId, oggFilePath)) {
                        newMessages[chatIndex] = true; // signalize this chat has new, unlistened messages (in ogg format)
                        ret = true;
                    }
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


// SENDING

void sendWavFile(const char* filePath, const char* fileName, const char* chat_id) {
  File f = SD.open(filePath);
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
  Serial.println("Senden beendet.");
}

bool connectWiFi() {
    // WiFi connection ----------------------------

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[SYSTEM] Connecting to WiFi");
    
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 30000) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[ERROR] WiFi connection failed (Timeout).");
        return false;
    }
    else {
        Serial.println("\n[OK] WiFi connected.");
        

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





// PLAYBACK ==============================================

void startPlayback(const char* filePath) {
    if (isAudioRunning) stopPlayback();
    
    audioFile = SD.open(filePath);
    if (!audioFile) {
        Serial.print("Error: Could not open ");
        Serial.println(filePath);
        return;
    }

    // Prepare output stream for new file (resets decoder state)
    stream_wavToI2s.begin();
    
    // Point copier to newly opened file and output stream
    copier_playback.begin(stream_wavToI2s, audioFile);
    
    isAudioRunning = true;

    Serial.print("Playback started...");
}

void stopPlayback() {
    if (isAudioRunning) {
        isAudioRunning = false;
        stream_wavToI2s.end(); // Stop the decoder and stream cleanly
        
        if (audioFile) audioFile.close();

        Serial.println(" --> Done.");
    }
}

void processAudio() {
    if (!isAudioRunning) return;
    
    // Is file still open and has data left?
    if (audioFile && audioFile.available()) {
        copier_playback.copy(); // Copy next chunk of audio
    } else {
        stopPlayback(); 
    }
}

void setI2SData_playback() {
    auto config = i2s_playback.defaultConfig(TX_MODE);
    config.pin_bck = I2S_BCLK;
    config.pin_ws = I2S_LRC;
    config.pin_data = I2S_DOUT;
    i2s_playback.begin(config);
}





// RECORDING ======================================

// Set things up for record task
void startRecording() {
    if (isAudioRunning) stopPlayback();

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
                    for (size_t i = 0; i < bytes_read; i += 4) { 
                        // A 16-bit stereo frame is 4 bytes
                        buf->data[mono_size++] = buf->data[i];
                        buf->data[mono_size++] = buf->data[i + 1];
                        // i+2 and i+3 (Right channel) are ignored
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

                if (!SD.exists(folderPath))
                    SD.mkdir(folderPath);

                // Create wav filepath with increased index
                int fileIndex = getNextFileIndex(folderPath, "recorded_");
                snprintf(currentFilePath, sizeof(currentFilePath), "%s/recorded_%03d.wav", folderPath, fileIndex);
                audioFile_record = SD.open(currentFilePath, FILE_WRITE);

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
void finishRecording() {
  isRecording = false;

  sendWavFile(currentFilePath, "message.wav", getChatId());
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

void addTranscodeJob(const char* oggFile, const char* wavFile) {
    TranscodeJob newJob;
    strncpy(newJob.inFile, oggFile, sizeof(newJob.inFile));
    strncpy(newJob.outFile, wavFile, sizeof(newJob.outFile));
    xQueueSend(jobQueue, &newJob, portMAX_DELAY);
}

// Transcode OGG/Opus file into Wav File
void transcodeTask(void *pvParameters) {
    Serial.println("Transcoder task initialized. Sleeping until a job arrives...");

    TranscodeJob job;
    
    while (true) {
        // Block indefinitely until a job is pushed to the queue.
        if (xQueueReceive(jobQueue, &job, portMAX_DELAY) == pdPASS) {
            Serial.printf("\n[Transcoder] Started: %s -> %s\n", job.inFile, job.outFile);

            audioFileIn = SD.open(job.inFile, FILE_READ);
            audioFileOut = SD.open(job.outFile, FILE_WRITE);

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
            SD.remove(job.inFile);

            // Tell playback queue transcoding is finished
            playbackQueue.signalizeFileReady(job.outFile);

            Serial.println("\n[Transcoder] Transcoding finished. Back to sleep.");
        }
    }
}