#include "Arduino.h"
#include "Audio.h"
#include "SD.h"
#include "FS.h"

#define I2S_DOUT  8
#define I2S_BCLK  5
#define I2S_LRC   6
#define SD_CS     D10
#define BUTTON    A5

Audio audio;
bool playing = false;

void setup() {
    Serial.begin(115200);

    pinMode(BUTTON, INPUT_PULLUP);
    SD.begin(SD_CS);
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(30);
}

void loop() {
    audio.loop();

    if (digitalRead(BUTTON) == LOW) {
        if (!playing) {
            audio.connecttoFS(SD, "/this_is_the_format.ogg");
            //audio.connecttoFS(SD, "/the_other_format.m4a");
            //audio.connecttoFS(SD, "/test.wav");
            playing = true;
        }
        delay(300); // debounce
    }
}

void audio_eof_stream(const char *info) {
    playing = false;
}