/*******************************************************************
    A telegram bot for your ESP32 that responds
    with whatever message you send it.

    Parts:
    ESP32 D1 Mini stlye Dev board* - http://s.click.aliexpress.com/e/C6ds4my
    (or any ESP32 board)

      = Affilate

    If you find what I do useful and would like to support me,
    please consider becoming a sponsor on Github
    https://github.com/sponsors/witnessmenow/


    Written by Brian Lough
    YouTube: https://www.youtube.com/brianlough
    Tindie: https://www.tindie.com/stores/brianlough/
    Twitter: https://twitter.com/witnessmenow
 *******************************************************************/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <algorithm>
#include "../Shared/arduino_secrets.h"


const unsigned long BOT_MTBS = 1000;  // mean time between scan messages

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
unsigned long bot_lasttime;  // last time messages' scan has been done

const int TASTER_PIN = D2;
const int LDR_PIN = A1;

void setup() {

  Serial.begin(115200);
  Serial.println();

  pinMode(TASTER_PIN, INPUT_PULLUP);

  // attempt to connect to Wifi network:
  Serial.print("Connecting to Wifi SSID ");
  Serial.print(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);  // Add root certificate for api.telegram.org
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.print("\nWiFi connected. IP address: ");
  Serial.println(WiFi.localIP());

  Serial.print("Retrieving time: ");
  configTime(0, 0, "pool.ntp.org");  // get UTC time via NTP
  time_t now = time(nullptr);
  while (now < 24 * 3600) {
    Serial.print(".");
    delay(100);
    now = time(nullptr);
  }
  Serial.println(now);
}

void loop() {

  int tasterZustand = digitalRead(TASTER_PIN);

  if (tasterZustand == LOW) {
    int lightValue = readLightValue();

    botSay("Hi, fyi es ist " + String(lightValue) + " hell");

    // Ausgabe des Messwerts auf dem seriellen Monitor
    Serial.print("Fotowiderstandswert: ");
    Serial.println(lightValue);

    // Eine kleine Verzögerung, um Prellen (entprellen) zu handhaben
    // und nicht zu schnell hintereinander zu messen, wenn der Taster gehalten wird.
    delay(200);
  }
}

void handleNewMessages(int numNewMessages) {

  for (int i = 0; i < numNewMessages; i++) {
    String message = bot.messages[i].text;

    if (message == "light") {
      String value = String(readLightValue());
      botSay(value);
    } else {
      botSay("I don't understand :(");
    }
  }
}

int readLightValue() {

  const int COUNT = 5;

  // Median
  int measurements[COUNT];

  for (int i = 0; i < COUNT; i++) {
    measurements[i] = analogRead(LDR_PIN);
    Serial.println(measurements[i]);
    delay(200);
  }

  std::sort(measurements, measurements + COUNT);

  return measurements[COUNT / 2];
}

void botSay(String text) {
  bot.sendMessage(CHAT_ID, text, "");
  Serial.println("Bot says: " + text);
}
