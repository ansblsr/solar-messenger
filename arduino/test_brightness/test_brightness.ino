#include <Arduino.h>

const int photoPin = 1; // Use an ADC1 pin (32-39)
const int samples = 64;  // Number of samples for averaging

void setup() {
  Serial.begin(115200);
  analogSetAttenuation(ADC_11db); // Allows measuring up to ~3.1V
}

void loop() {
  long sum = 0;
  
  // 1. Average multiple readings to eliminate noise
  for(int i = 0; i < samples; i++) {
    sum += analogRead(photoPin);
    delay(1); 
  }
  
  int averageValue = sum / samples;

  Serial.printf("Brightness measured: %d\n", averageValue);
  
  delay(500);
}