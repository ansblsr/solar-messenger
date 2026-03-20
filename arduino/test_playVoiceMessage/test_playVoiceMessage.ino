/**
 * Arduino Nano ESP32 - Telegram Voice Message Player
 * * This sketch connects to WiFi, polls a Telegram Bot for voice messages,
 * downloads the OGG/Opus file to an SD card, and plays it via I2S.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <Audio.h>
#include <ArduinoJson.h>

// --- KONFIGURATION ---
#include "../Shared/arduino_secrets.h"

// I2S Pins for the Speaker (DAC)
#define I2S_DOUT  8
#define I2S_BCLK  5
#define I2S_LRC   6

// SD Card Pin
#define SD_CS     D10

// Globals
WiFiClientSecure client_wifi;
Audio audio;

unsigned long lastTimeBotRan;
const int botRequestDelay = 2000; // 2 second interval for polling
long lastUpdateId = 0;

// Downloads audio file from Telegram Servers
bool downloadTelegramFile(String fileId, String path) {
    Serial.println("Telegram API: /getFile");
    
    // This GET request will get us a File Object from Telegram
    String getFile_url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/getFile?file_id=" + fileId;
    
    HTTPClient http_getFile;
    http_getFile.begin(client_wifi, getFile_url);
    int httpCode = http_getFile.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        String response = http_getFile.getString(); // response contains info about the file in order to download it
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, response); // Telegram API always sends json back
        
        if (doc["ok"]) {
            String filePath = doc["result"]["file_path"]; // filePath we got from the get request
            String downloadUrl = "https://api.telegram.org/file/bot" + String(BOT_TOKEN) + "/" + filePath; // url to download the file
            
            Serial.print("Downloading File URL: ");
            Serial.println(downloadUrl);

            HTTPClient http_download;
            http_download.begin(client_wifi, downloadUrl);
            int downloadCode = http_download.GET();

            if (downloadCode == HTTP_CODE_OK) {
                File f = SD.open(path, FILE_WRITE);
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

void processTelegramUpdates() {
    Serial.println("Telegram API: /getUpdates");

    String getUpdates_url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/getUpdates?offset=" + String(lastUpdateId + 1);
    
    HTTPClient http_getFile;
    http_getFile.begin(client_wifi, getUpdates_url);
    int httpCode = http_getFile.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http_getFile.getString();
        DynamicJsonDocument doc(4096); // Larger buffer for updates
        deserializeJson(doc, payload);
        
        JsonArray results = doc["result"].as<JsonArray>();
        
        for (JsonObject currentResult : results) {
            lastUpdateId = currentResult["update_id"];
            
            Serial.print("\nNEW MESSAGE (Update ID: ");
            Serial.print(lastUpdateId); Serial.println(")");
            
            JsonObject message = currentResult["message"];
            String fromName = message["from"]["first_name"].as<String>();
            long chatId = message["chat"]["id"];
            
            Serial.print("From: "); Serial.print(fromName);
            Serial.print("      Chat ID: "); Serial.println(chatId);

            if (message.containsKey("voice")) {
                Serial.println("Type: VoiceMessage");
                String fileId = message["voice"]["file_id"];
                
                if (downloadTelegramFile(fileId, "/voice.ogg")) {
                    Serial.println("Starting Playback...");
                    audio.connecttoFS(SD, "/voice.ogg");
                }
            } 
            else if (message.containsKey("text")) {
                Serial.print("Type: Text -> Content: ");
                Serial.println(message["text"].as<String>());
            }
            else {
                Serial.println("Type: OTHER");
            }
            Serial.println("----------------------------\n");
        }
    }
    http_getFile.end();
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n[SYSTEM] Initializing...");
    
    if (!SD.begin(SD_CS)) {
        Serial.println("[CRITICAL] SD Card Mount Failed!");
        while (1);
    }
    Serial.println("[OK] SD Card Initialized.");

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    client_wifi.setInsecure(); // Telegram API uses HTTPS
    
    Serial.print("[SYSTEM] Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[OK] WiFi connected.");

    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(12);

    Serial.println("[SYSTEM] Ready. Waiting for Telegram messages...");
}

void loop() {
    audio.loop();

    if (!audio.isRunning()) {
        if (millis() - lastTimeBotRan > botRequestDelay) {
            processTelegramUpdates();
            lastTimeBotRan = millis();
        }
    }
}

// Gets calles by the audio library to send info about playback
void audio_info(const char *info) {
    Serial.print("[AUDIO INFO] ");
    Serial.println(info);
}

// Gets called by audio library to signalize end of playback
void audio_eof_mp3(const char *info) {
    Serial.println("[AUDIO] Playback finished.");
}