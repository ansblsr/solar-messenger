#include <Arduino.h>

struct Battery {
    int level = 0;

    int cost_send = 30;
    int cost_update = 30;
    int cost_listen = 5;

    unsigned long tp_lastRefresh;

    void chargeBy(int points) {
        level += points;
        if (level > 100) level = 100;
    }

    bool canSend() {
        return level >= cost_send;
    }

    bool canFetch() {
        return level >= cost_update;
    }

    bool canListen() {
        return level >= cost_listen;
    }

    void subtractSend() {
        level -= cost_send;
    }

    void subtractUpdate() {
        level -= cost_update;
    }

    void subtractListen() {
        level -= cost_listen;
    }
};

Battery battery;
#define INTERVAL_BATTERY_UPDATE 5000

unsigned long tp_lastFetch = 0;
#define INTERVAL_AUTO_FETCH 39600000 // 11h in ms

void setup() {
    Serial.begin(115200);

    tp_lastFetch = millis();
}

void loop() {
    unsigned long tp_now = millis();
    handleBattery(tp_now);
    handleBehavior(tp_now);

    delay(100);
}

void handleBattery(unsigned long tp_now) {
    if(battery.level >= 100) return;

    if(tp_now - battery.tp_lastRefresh > INTERVAL_BATTERY_UPDATE) {
        // charging logic here

        battery.chargeBy(10);
        Serial.printf("Battery level: %d\n", battery.level);

        battery.tp_lastRefresh = tp_now;
    }
}

void handleBehavior(unsigned long tp_now) {
    if (tp_now - tp_lastFetch > 10000/*INTERVAL_AUTO_FETCH*/) {
        // sending logic here

        Serial.println("Fetching messages...");
        //battery.subtractUpdate();

        tp_lastFetch = tp_now;
    }
}