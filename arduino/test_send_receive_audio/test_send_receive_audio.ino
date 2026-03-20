#include "Arduino.h"

// Brute-Force: Fixe Definitionen entfernt. 
// Bitte ändere die UniversalTelegramBot.h wie im Chat beschrieben!

#include "Audio.h"
#include "SD.h"
#include "FS.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <driver/i2s_std.h>
#include <ArduinoJson.h>

// Pin-Definitionen (Shared I2S Bus)
#define I2S_DIN 7   
#define I2S_DOUT 8  
#define I2S_BCLK 5  
#define I2S_WS 6    
#define SD_CS D10
#define BUTTON A5

#define SAMPLE_RATE 16000

#include "../Shared/arduino_secrets.h"

const unsigned long BOT_MTBS = 1000; 
unsigned long bot_lasttime; 

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

Audio audio;
bool isPlaying = false;
int messageCount = 0;
i2s_chan_handle_t rx_handle = NULL;

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
      .ws = (gpio_num_t)I2S_WS,
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

// records audio in wav format and writes to sd card
String recordAudio() {
  if (isPlaying) {
    audio.stopSong();
    isPlaying = false;
  }
  messageCount++;
  String fileName = "rec_" + String(messageCount) + ".wav";
  String filePath = "/" + fileName;
  File file = SD.open(filePath, FILE_WRITE);
  if (!file) return "";
  uint8_t header[44] = { 0 };
  file.write(header, 44);
  setupI2S_record();
  Serial.println("Aufnahme gestartet...");
  uint32_t bytesRecorded = 0;
  while (digitalRead(BUTTON) == LOW) {
    int16_t buffer[128]; 
    size_t bytesRead = 0;
    if (i2s_channel_read(rx_handle, buffer, sizeof(buffer), &bytesRead, 100) == ESP_OK) {
      int16_t monoBuffer[64];
      int j = 0;
      for (int i = 0; i < (bytesRead / 2); i += 2) {
        monoBuffer[j++] = buffer[i];
      }
      file.write((const uint8_t*)monoBuffer, j * 2);
      bytesRecorded += (j * 2);
    }
  }
  stopI2S_record();
  file.seek(0);
  writeWavHeader(file, bytesRecorded);
  file.close();
  audio.setPinout(I2S_BCLK, I2S_WS, I2S_DOUT);
  Serial.println("Aufnahme beendet.");
  return fileName;
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

// --- TELEGRAM DOWNLOAD & PLAYBACK ---

bool downloadTelegramFile(String fileId, String localPath) {
  String getFileUrl = "/bot" + String(BOT_TOKEN) + "/getFile?file_id=" + fileId;
  String response = bot.sendGetToTelegram(getFileUrl);
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, response)) return false;
  const char* filePath = doc["result"]["file_path"];
  if (!filePath) return false;

  WiFiClientSecure downloadClient;
  downloadClient.setInsecure();
  if (downloadClient.connect("api.telegram.org", 443)) {
    downloadClient.print(String("GET ") + "/file/bot" + String(BOT_TOKEN) + "/" + String(filePath) + " HTTP/1.1\r\n" +
                         "Host: api.telegram.org\r\n" +
                         "Connection: close\r\n\r\n");
    unsigned long timeout = millis();
    while (downloadClient.available() == 0) {
      if (millis() - timeout > 5000) return false;
    }
    while (downloadClient.connected()) {
      String line = downloadClient.readStringUntil('\n');
      if (line == "\r") break;
    }
    File file = SD.open(localPath, FILE_WRITE);
    if (!file) return false;
    while (downloadClient.available()) {
      uint8_t downloadBuf[512];
      size_t n = downloadClient.read(downloadBuf, sizeof(downloadBuf));
      file.write(downloadBuf, n);
    }
    file.close();
    Serial.println("Voice-Datei heruntergeladen.");
    return true;
  }
  return false;
}

void playAudioFile(String path) {
  if (!playing)
  {
    Serial.println("Wiedergabe: " + path);
    audio.setPinout(I2S_BCLK, I2S_WS, I2S_DOUT);
    audio.connecttoFS(SD, path.c_str());
    isPlaying = true;
  }
}

void checkTelegramMessages() {
  if (millis() > bot_lasttime + BOT_MTBS) {
    Serial.println("checked messages");
    
    String response = bot.sendGetToTelegram("/bot" + String(BOT_TOKEN) + "/getUpdates?offset=" + String(bot.last_message_received + 1));
    
    if (response != "") {
      // Puffer vergrößert, da Voice-Nachrichten viel JSON-Metadaten enthalten
      DynamicJsonDocument doc(4096); 
      DeserializationError error = deserializeJson(doc, response);
      
      if (error) {
        Serial.print("JSON Parse Fehler: ");
        Serial.println(error.c_str());
      } else if (!doc["ok"]) {
        Serial.println("Telegram Antwort nicht OK.");
      } else {
        JsonArray updates = doc["result"];
        for (JsonObject update : updates) {
          bot.last_message_received = update["update_id"];
          JsonObject message = update["message"];
          String chat_id = String(message["chat"]["id"]);

          if (chat_id == CHAT_ID) {
            if (message.containsKey("voice")) {
              String fileId = message["voice"]["file_id"];
              Serial.println("Sprachnachricht erkannt. ID: " + fileId);
              String localPath = "/in_voice.ogg";
              if (downloadTelegramFile(fileId, localPath)) {
                playAudioFile(localPath);
              }
            } else if (message.containsKey("text")) {
              Serial.println("Textnachricht empfangen: " + String(message["text"]));
            }
          }
        }
      }
    }
    bot_lasttime = millis();
  }
}

// --- TELEGRAM UPLOAD ---

void sendWavFile(const char* filePath, const char* fileName) {
  File f = SD.open(filePath);
  if (!f) return;
  size_t fileSize = f.size();
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("api.telegram.org", 443)) return;
  String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
  String partHeader = "--" + boundary + "\r\n"
                      "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + CHAT_ID + "\r\n"
                      "--" + boundary + "\r\n"
                      "Content-Disposition: form-data; name=\"document\"; filename=\"" + fileName + "\"\r\n"
                      "Content-Type: audio/wav\r\n\r\n";
  String partFooter = "\r\n--" + boundary + "--\r\n";
  size_t totalLength = partHeader.length() + fileSize + partFooter.length();
  client.println("POST /bot" + String(BOT_TOKEN) + "/sendDocument HTTP/1.1");
  client.println("Host: api.telegram.org");
  client.println("Content-Type: multipart/form-data; boundary=" + boundary);
  client.println("Content-Length: " + String(totalLength));
  client.println("Connection: close");
  client.println();
  client.print(partHeader);
  uint8_t buf[512];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    client.write(buf, n);
  }
  f.close();
  client.print(partFooter);
  client.stop();
  Serial.println("Audio gesendet.");
}

// --- SETUP & LOOP ---

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON, INPUT_PULLUP);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  secured_client.setInsecure();
  configTime(0, 0, "pool.ntp.org");
  time_t now = time(nullptr);
  while (now < 24 * 3600) { delay(100); now = time(nullptr); }
  if (!SD.begin(SD_CS)) { Serial.println("SD Karte nicht gefunden!"); while (1); }
  audio.setPinout(I2S_BCLK, I2S_WS, I2S_DOUT);
  audio.setVolume(15); 
  Serial.println("\nSystem gestartet.");
}

void loop() {
  audio.loop();

  if (digitalRead(BUTTON) == LOW && !isPlaying) {
    String recFile = recordAudio();
    if (recFile != "") {
      delay(200);
      sendWavFile(("/" + recFile).c_str(), recFile.c_str());
    }
  }

  checkTelegramMessages();
}

void audio_eof_mp3(const char *info) { isPlaying = false; Serial.println("Wiedergabe beendet."); }
void audio_eof_stream(const char *info) { isPlaying = false; }