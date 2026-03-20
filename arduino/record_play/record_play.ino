#include "Arduino.h"
#include "Audio.h"
#include "FS.h"
#include "SD.h"
#include "driver/i2s.h"

// Hardware-GPIOs
#define I2S_BCLK 5  // D2
#define I2S_WS 6    // D3
#define I2S_DIN 7   // D4 (mic)
#define I2S_DOUT 8  // D5 (amp)
#define BUTTON A5
#define SD_CS D10

#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000

Audio audio;
bool playing = false;

// writes WAV header into the file (after recording it)
void writeWavHeader(File file, uint32_t fileSize) {
  // define important values
  uint32_t sampleRate = SAMPLE_RATE;
  uint16_t numChannels = 1;
  uint16_t bitsPerSample = 16;
  uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);

  // write required signifiers for correct file reading
  file.write((const uint8_t*)"RIFF", 4);  // "this is a riff container" (Resource Interchange File Format)
  uint32_t chunkSize = fileSize + 36;
  file.write((const uint8_t*)&chunkSize, 4);  // "the entire file is this big"
  file.write((const uint8_t*)"WAVE", 4);      // "contains audio in wav format"
  file.write((const uint8_t*)"fmt ", 4);      // technical format
  uint32_t subchunk1Size = 16;
  file.write((const uint8_t*)&subchunk1Size, 4);

  // write audio details
  uint16_t audioFormat = 1;
  file.write((const uint8_t*)&audioFormat, 2);
  file.write((const uint8_t*)&numChannels, 2);
  file.write((const uint8_t*)&sampleRate, 4);
  file.write((const uint8_t*)&byteRate, 4);

  uint16_t blockAlign = numChannels * (bitsPerSample / 8);
  file.write((const uint8_t*)&blockAlign, 2);
  file.write((const uint8_t*)&bitsPerSample, 2);

  // write beginning of data block
  file.write((const uint8_t*)"data", 4);     // "now comes the actual sound data"
  file.write((const uint8_t*)&fileSize, 4);  // "this many bytes"
}


i2s_chan_handle_t rx_handle;

void setupI2S_record() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, NULL, &rx_handle);

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    // Wir nutzen STEREO, da viele Mikrofone das brauchen, um das Timing korrekt zu halten
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
  i2s_channel_disable(rx_handle);
  i2s_del_channel(rx_handle);
}

String recordAudio() {
  String fileName = "message.wav";
  String filePath = "/" + fileName;
  File file = SD.open(filePath, FILE_WRITE);
  if (!file) "";

  // at first, leave required wav-file-header empty
  uint8_t header[44] = { 0 };
  file.write(header, 44);

  setupI2S_record();
  Serial.println("Aufnahme...");
  uint32_t bytesRecorded = 0;

  while (digitalRead(BUTTON) == LOW) {
    int16_t buffer[128];  // Buffer für Stereo (L+R)
    size_t bytesRead = 0;

    // Daten lesen
    if (i2s_channel_read(rx_handle, buffer, sizeof(buffer), &bytesRead, 100) == ESP_OK) {
      // Da wir Mono aufnehmen wollen, aber Stereo lesen:
      // Wir extrahieren nur jeden zweiten Sample (den aktiven Kanal)
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

  // at the end, jump to beginning of file and write the header
  file.seek(0);
  writeWavHeader(file, bytesRecorded);

  file.close();
  Serial.println("WAV Gespeichert!");

  return fileName;
}

void playAudio() {
  File file = SD.open("/test.wav");
  if (!file) return;
  setupI2S_record();
  file.seek(44);
  Serial.println("Wiedergabe...");
  int16_t buffer[128];
  while (file.available()) {
    size_t bytesToRead = file.read((uint8_t*)buffer, sizeof(buffer));
    size_t bytesWritten;
    i2s_write(I2S_PORT, buffer, bytesToRead, &bytesWritten, portMAX_DELAY);
  }
  file.close();

  // fill amp buffer with silence (avoids weirdness)
  int16_t silence[128] = { 0 };
  size_t bytes_written;
  for (int i = 0; i < 10; i++) {
    i2s_write(I2S_PORT, silence, sizeof(silence), &bytes_written, portMAX_DELAY);
  }

  Serial.println("Wiedergabe beendet");
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON, INPUT_PULLUP);

  if (!SD.begin(SD_CS)) {
    Serial.println("SD Fehler!");
    while (1);
  }

  audio.setPinout(I2S_BCLK, I2S_WS, I2S_DOUT);
  audio.setVolume(30);

  Serial.println("Bereit!");
}

void loop() {
  audio.loop();

  if (digitalRead(BUTTON) == LOW) {
    recordAudio();
    delay(500);
    audio.connecttoFS(SD, "/message.wav");
    playing = true;
  }
}

void audio_eof_stream(const char *info) {
    playing = false;
}