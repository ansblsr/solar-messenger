#include <algorithm>
#include "esp_sleep.h"

// SETUP
#define TASTER_PIN D2
#define LDR_PIN A1

#define uS_TO_S_FACTOR 1000000
#define TIME_TO_SLEEP 10

// BATTERY
RTC_DATA_ATTR int battery_charge = 0;

const int chargeDuration_bright_sec = 180;
const int chargeDuration_mid_sec = 600;

// |---dark---200---mid---1200---bright---|
const int tLux_dark = 200;
const int tLux_bright = 1200;


void setup() {

  Serial.begin(9600);
  delay(1000);

  Serial.println("-------------------------------------");
  //print_wakeup_reason();
  
  // -----------------------------------------------------
  
  if(battery_charge >= 100) {
    Serial.print("Battery is already full -> ");
    Serial.println(battery_charge);
  }
  else {
    int lightValue = readLightValue();

    if(lightValue < tLux_dark) {
      chargeBattery(0);
    }
    else if(lightValue < tLux_bright){
      float charge_amount = 100 / (chargeDuration_mid_sec / TIME_TO_SLEEP);
      chargeBattery(charge_amount);
    } 
    else {
      float charge_amount = 100 / (chargeDuration_bright_sec / TIME_TO_SLEEP);
      chargeBattery(charge_amount);
    }
  }

  // -----------------------------------------------------
  
  Serial.println("--> Werte wurden gespeichert. Schalte nun Funk und Komponenten ab.");

  // Setzt den ESP32 so, dass er nach der definierten Zeit aufwacht
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  
  Serial.println("Gehe jetzt in den Deep-Sleep-Modus für " + String(TIME_TO_SLEEP) + " Sekunden.");
  Serial.flush(); 

  // 4. Starte den Deep Sleep
  // Das Programm stoppt HIER! Nach dem Aufwachen startet der ESP32 wieder ganz oben bei setup().
  esp_deep_sleep_start();
}

void loop() {}

/*void print_wakeup_reason(){
  esp_sleep_wake_up_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  switch(wakeup_reason)
  {
    case ESP_SLEEP_WAKEUP_EXT0 : Serial.println("Aufgewacht durch externes Signal (EXT0)"); break;
    case ESP_SLEEP_WAKEUP_EXT1 : Serial.println("Aufgewacht durch externes Signal (EXT1)"); break;
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("Aufgewacht durch Timer (Deep Sleep)"); break; // Dies ist der Standardfall
    case ESP_SLEEP_WAKEUP_TOUCH: Serial.println("Aufgewacht durch Touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP  : Serial.println("Aufgewacht durch ULP-Co-Prozessor"); break;
    default : Serial.println("Normaler Power-On oder Reset"); break;
  }
}*/

int readLightValue() {

  const int count = 5;
  int measurements[count];

  for (int i = 0; i < count; i++) {
    measurements[i] = analogRead(LDR_PIN);
    delay(200);
  }

  std::sort(measurements, measurements + count);

  return measurements[count / 2];
}

void chargeBattery(int amount_percent) {

  battery_charge += amount_percent;

  if(battery_charge > 100) battery_charge = 100;

  Serial.print("Charged battery for " + String(amount_percent) + " %  -  ");
  Serial.println("Battery Charge is " + String(battery_charge) + " %");
}
