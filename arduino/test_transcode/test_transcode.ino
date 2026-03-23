#include <Arduino.h>
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusOgg.h" // Handles Opus wrapped in an Ogg container
#include <SD.h>
#include <SPI.h>

// Struct to pass jobs to our persistent FreeRTOS task
struct TranscodeJob {
    char inFile[64];
    char outFile[64];
};

// Global queue handle
QueueHandle_t jobQueue;
TaskHandle_t transcodeTaskHandle = NULL;

//

File audioFileIn;
File audioFileOut;

// Instantiate pipeline components locally for the current job.
// Using OpusOggDecoder handles the Ogg wrapper natively.
OpusOggDecoder decoder;
WAVEncoder encoder;

//BufferedStream bufferedIn(audioFileIn, 32768);   // 32 KB
//BufferedStream bufferedOut(audioFileOut, 16384); // 16 KB

EncodedAudioStream dec_stream(&audioFileIn, &decoder);
EncodedAudioStream enc_stream(&audioFileOut, &encoder);

StreamCopy copier(enc_stream, dec_stream, 16384);

// --- Persistent FreeRTOS Task ---
void transcodeTask(void *pvParameters) {
    TranscodeJob job;

    Serial.println("Transcoder task initialized. Sleeping until a job arrives...");

    while (true) {
        // Block indefinitely until a job is pushed to the queue.
        // While blocked, this task consumes no CPU cycles.
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

            // Set up audio information. The decoder will notify the encoder if 
            // the Ogg file specifies a different sample rate/channels in its headers.
            AudioInfo info(48000, 1, 16);
            decoder.addNotifyAudioChange(encoder);

            // 1. Sanity check: Ensure PSRAM is actually initialized and available
            if (psramInit()) {
                Serial.printf("PSRAM initialized! Total PSRAM: %d bytes\n", ESP.getPsramSize());
                Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
            } else {
                Serial.println("ERROR: PSRAM failed to initialize. Check Tools -> PSRAM in the IDE.");
                // You might want to halt execution here if PSRAM is strictly required
            }

            dec_stream.resizeReadResultQueue(131072);

            dec_stream.begin(info);
            enc_stream.begin(info);

            int flushCounter = 0;

            // Transcoding Loop
            while (true) {
                size_t bytesCopied = copier.copy();

                Serial.println(bytesCopied);

                // Handle the start-up delay and end-of-file buffer flushing
                if (bytesCopied == 0) {
                    // Check if the input file actually has data left.
                    // If it does, the decoder is just parsing headers or spinning up.
                    if (audioFileIn.available() == 0) {
                        // The file is fully read, but we might still have PCM data trapped 
                        // in the decoder buffers. We tick a counter to let the copier flush it.
                        flushCounter++;
                        if (flushCounter > 10) { 
                            break; // Completely flushed, exit the loop
                        }
                    }
                } else {
                    flushCounter = 0; // Reset counter if data was successfully moved
                }

                // Crucial: yield to FreeRTOS watchdog
                vTaskDelay(pdMS_TO_TICKS(2)); 
            }

            // Cleanup memory and close files for this job
            enc_stream.end();
            dec_stream.end();
            audioFileIn.close();
            audioFileOut.close();

            Serial.println("[Transcoder] Complete. Back to sleep.");
        }
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial);

    AudioLogger::instance().begin(Serial, AudioLogger::Debug);

    if (!SD.begin()) {
        Serial.println("Critical Error: SD Card Mount Failed!");
        return;
    }

    AudioInfo info;
    info.sample_rate = 48000;
    info.channels = 1;
    info.bits_per_sample = 16;

    decoder.setAudioInfo(info);
    encoder.setAudioInfo(info);

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

    // --- Example: Queueing a job from setup ---
    TranscodeJob newJob = {"/inFile.ogg", "/output.wav"};
    xQueueSend(jobQueue, &newJob, portMAX_DELAY);
}

void loop() {
    // Because of the FreeRTOS Queue, you can safely push new jobs to `jobQueue` 
    // from anywhere in your code (like a web server callback or button press).
    
    vTaskDelay(pdMS_TO_TICKS(1000));
}