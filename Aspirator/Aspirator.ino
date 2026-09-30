#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <SinricPro.h>
#include <SinricProSwitch.h>

// --- SİNRİC PRO BİLGİLERİ ---
#define APP_KEY           "SENIN_APP_KEY_BURAYA"
#define APP_SECRET        "SENIN_APP_SECRET_BURAYA"
#define LIGHT_ID          "SENIN_ISIK_DEVICE_ID"
#define SPD1_ID           "SENIN_HIZ1_DEVICE_ID"
#define SPD2_ID           "SENIN_HIZ2_DEVICE_ID"
#define SPD3_ID           "SENIN_HIZ3_DEVICE_ID"

// --- GITHUB OTA AYARLARI ---
const String CURRENT_VERSION = "1.0.1";
const String GITHUB_VERSION_URL = "https://raw.githubusercontent.com/kubilayatas/Aspirator/main/version.txt";
const String GITHUB_FIRMWARE_URL = "https://raw.githubusercontent.com/kubilayatas/Aspirator/main/firmware.bin";

// --- PİN TANIMLAMALARI ---
const int PIN_BTN_LIGHT = 2; 
const int PIN_BTN_SPD   = 3; // Döngüsel Hız Tuşu

const int PIN_OUT_LIGHT = 6;
const int PIN_OUT_SPD1  = 7;
const int PIN_OUT_SPD2  = 10;
const int PIN_OUT_SPD3  = 21;

const int PIN_LED_LIGHT = 0;
const int PIN_LED_SPD1  = 1;
const int PIN_LED_SPD2  = 20;
const int PIN_LED_SPD3  = 8;

// --- DURUM DEĞİŞKENLERİ ---
bool lightState = false;
int currentSpeed = 0; // 0: Kapalı, 1, 2, 3

// --- BUTON SINIFI ---
class SmartButton {
  private:
    int pin;
    bool lastReading = LOW;
    unsigned long lastDebounceTime = 0;
    const unsigned long debounceDelay = 50; 
    bool waitingForRelease = false;

  public:
    SmartButton(int p) { pin = p; pinMode(pin, INPUT); }
    bool onRelease() {
      bool reading = digitalRead(pin);
      bool triggered = false;
      unsigned long now = millis();
      if (reading != lastReading) lastDebounceTime = now;
      if ((now - lastDebounceTime) > debounceDelay) {
        if (reading == HIGH && !waitingForRelease) waitingForRelease = true;
        else if (reading == LOW && waitingForRelease) {
          waitingForRelease = false;
          triggered = true;
        }
      }
      lastReading = reading;
      return triggered;
    }
};

SmartButton btnLight(PIN_BTN_LIGHT);
SmartButton btnSpeedCycle(PIN_BTN_SPD);

// --- DONANIM VE BULUT GÜNCELLEME ---
void updateHardwareAndCloud() {
  digitalWrite(PIN_OUT_LIGHT, lightState ? LOW : HIGH);
  digitalWrite(PIN_OUT_SPD1, (currentSpeed == 1) ? LOW : HIGH);
  digitalWrite(PIN_OUT_SPD2, (currentSpeed == 2) ? LOW : HIGH);
  digitalWrite(PIN_OUT_SPD3, (currentSpeed == 3) ? LOW : HIGH);

  digitalWrite(PIN_LED_LIGHT, lightState ? HIGH : LOW);
  digitalWrite(PIN_LED_SPD1, (currentSpeed == 1) ? HIGH : LOW);
  digitalWrite(PIN_LED_SPD2, (currentSpeed == 2) ? HIGH : LOW);
  digitalWrite(PIN_LED_SPD3, (currentSpeed == 3) ? HIGH : LOW);

  if (WiFi.status() == WL_CONNECTED) {
    SinricPro[LIGHT_ID].as<SinricProSwitch>().sendPowerStateEvent(lightState);
    SinricPro[SPD1_ID].as<SinricProSwitch>().sendPowerStateEvent(currentSpeed == 1);
    SinricPro[SPD2_ID].as<SinricProSwitch>().sendPowerStateEvent(currentSpeed == 2);
    SinricPro[SPD3_ID].as<SinricProSwitch>().sendPowerStateEvent(currentSpeed == 3);
  }
}

// --- SİNRİC PRO CALLBACK FONKSİYONLARI ---
bool onPowerStateLight(const String &deviceId, bool &state) {
  lightState = state;
  updateHardwareAndCloud();
  return true;
}

bool onPowerStateSpeed(const String &deviceId, bool &state, int targetSpeed) {
  if (state) currentSpeed = targetSpeed; 
  else if (currentSpeed == targetSpeed) currentSpeed = 0; 
  updateHardwareAndCloud();
  return true;
}

// --- OTOMATİK GITHUB OTA KONTROLÜ ---
void autoUpdateCheck() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Guncelleme kontrol ediliyor...");
    WiFiClientSecure client;
    client.setInsecure(); // GitHub SSL doğrulaması için zorunlu
    
    HTTPClient http;
    http.begin(client, GITHUB_VERSION_URL);
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
      String newVersion = http.getString();
      newVersion.trim();
      
      if (newVersion != CURRENT_VERSION && newVersion.length() > 0) {
        Serial.println("Yeni surum bulundu (" + newVersion + "). İndiriliyor...");
        t_httpUpdate_return ret = httpUpdate.update(client, GITHUB_FIRMWARE_URL);
        if (ret == HTTP_UPDATE_FAILED) {
          Serial.printf("OTA Hatasi (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
        }
      } else {
        Serial.println("Sistem guncel.");
      }
    }
    http.end();
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_OUT_LIGHT, OUTPUT); pinMode(PIN_OUT_SPD1, OUTPUT); 
  pinMode(PIN_OUT_SPD2, OUTPUT); pinMode(PIN_OUT_SPD3, OUTPUT);
  pinMode(PIN_LED_LIGHT, OUTPUT); pinMode(PIN_LED_SPD1, OUTPUT); 
  pinMode(PIN_LED_SPD2, OUTPUT); pinMode(PIN_LED_SPD3, OUTPUT);
  updateHardwareAndCloud(); 

  // --- WiFiManager Kurgusu ---
  WiFiManager wm;
  // wm.resetSettings(); // Agi unutturmak istersen bu satiri ac
  wm.setConfigPortalTimeout(120); // 2 dakika icinde sifre girilmezse portali kapat ve offline devam et!
  
  bool res = wm.autoConnect("Aspirator_Kurulum"); 
  if(!res) {
    Serial.println("WiFi baglanamadi. Cevrimdisi (Offline) modda fiziksel tuslarla calisacak.");
  } else {
    Serial.println("WiFi Baglandi!");
    
    // İnternet varsa sessizce guncelleme kontrolu yap
    autoUpdateCheck();

    // SinricPro Kurulumu
    SinricProSwitch& myLight = SinricPro[LIGHT_ID];
    myLight.onPowerState(onPowerStateLight);
    
    SinricProSwitch& mySpd1 = SinricPro[SPD1_ID];
    mySpd1.onPowerState([](const String& id, bool& state) { return onPowerStateSpeed(id, state, 1); });
    
    SinricProSwitch& mySpd2 = SinricPro[SPD2_ID];
    mySpd2.onPowerState([](const String& id, bool& state) { return onPowerStateSpeed(id, state, 2); });
    
    SinricProSwitch& mySpd3 = SinricPro[SPD3_ID];
    mySpd3.onPowerState([](const String& id, bool& state) { return onPowerStateSpeed(id, state, 3); });

    SinricPro.begin(APP_KEY, APP_SECRET);
  }
}

void loop() {
  // Sadece internet baglantisi varsa SinricPro'yu dinle
  if (WiFi.status() == WL_CONNECTED) {
    SinricPro.handle();
  }

  // --- FİZİKSEL BUTON KONTROLLERİ ---
  if (btnLight.onRelease()) {
    lightState = !lightState;
    updateHardwareAndCloud();
  }

  if (btnSpeedCycle.onRelease()) {
    currentSpeed++;
    if (currentSpeed > 3) currentSpeed = 0; 
    updateHardwareAndCloud();
  }
}