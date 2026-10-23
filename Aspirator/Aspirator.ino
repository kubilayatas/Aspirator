/*
 * Akilli Aspirator - ESP32-C3 Super Mini
 * SinricPro (Google Home / Alexa / Siri) + WiFiManager + GitHub OTA
 *
 * SinricPro'da TEK cihaz kullanilir:
 *   - Power (Ac/Kapat)        -> Isik
 *   - Setting "fan speed"     -> Motor hizi (0: kapali, 1, 2, 3)
 *
 * Fiziksel tuslar:
 *   - Isik tusu kisa  : isik ac/kapat
 *   - Isik tusu 8 sn  : fabrika ayari (WiFi + SinricPro bilgileri silinir)
 *   - Hiz tusu kisa   : 0 > 1 > 2 > 3 > 0
 *   - Hiz tusu 5 sn   : kurulum agini (portal) 3 dk icin ac -> WiFi'yi ya da
 *                       SinricPro bilgilerini sifirlamadan degistirmek icin
 *
 * Arduino IDE ayarlari:
 *   Board            : ESP32C3 Dev Module   (Nologo Super Mini menusunde OTA'li buyuk sema yok)
 *   Partition Scheme : No FS 4MB (2MB APP x2)   veya   Minimal SPIFFS (1.9MB APP with OTA)
 *   USB CDC On Boot  : Enabled   (Seri monitor USB uzerinden calissin diye)
 *
 * Test edilen surumler: esp32 core 3.3.2, SinricPro 5.1.0, WiFiManager 2.0.17
 *
 * DONANIM NOTU: GPIO21 = UART0 TX. ESP32-C3'un ROM'u acilista (setup() calismadan
 * once, ~100 ms) boot mesajlarini bu pinden basar; pin bu sirada LOW-HIGH darbeler
 * gorur ve Active-LOW surulen 3. hiz triyagi kisa sure tetiklenebilir. Acilista motor
 * bir an "silkiniyorsa" sebep budur; 3. hiz triyagini GPIO21 yerine bir LED pinine
 * (orn. GPIO1) alip LED'i GPIO21'e tasimak kalici cozumdur.
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <SinricPro.h>
#include <SinricProSwitch.h>
#include "driver/gpio.h"

// ================== GIZLI BILGILER ==================
// secrets.h repoya GIRMEZ (.gitignore). EMBED_SECRETS 0 iken SinricPro bilgileri
// koda gomulmez; cihaz onlari NVS'den (onceki kurulum) ya da kurulum portalindan alir.
//
// Onerilen akis:
//   1) Cihaza ILK kez USB'den yuklerken EMBED_SECRETS 1 kalabilir. Bilgiler acilista
//      NVS'ye de yazilir, yani sonraki surumlerde koda gomulmelerine gerek kalmaz.
//   2) GitHub'a koyacagin firmware.bin'i HER ZAMAN EMBED_SECRETS 0 ile derle.
//      1 iken derlersen asagidaki #warning uyarisi derleme ciktisinda gorunur.
#define EMBED_SECRETS 1

#if __has_include("secrets.h")
  #include "secrets.h"
#endif

#if !EMBED_SECRETS || !defined(SECRET_APP_KEY)
  #undef  SECRET_APP_KEY
  #undef  SECRET_APP_SECRET
  #undef  SECRET_DEVICE_ID
  #define SECRET_APP_KEY    ""
  #define SECRET_APP_SECRET ""
  #define SECRET_DEVICE_ID  ""
#else
  #warning "EMBED_SECRETS=1: SinricPro APP SECRET bu derlemenin icinde. Bu .bin dosyasini GitHub'a KOYMA!"
#endif

// Kurulum agi (Aspirator_Kurulum) sifresi. En az 8 karakter olmali.
// secrets.h icinde SECRET_AP_PASS tanimlarsan o kullanilir.
#ifndef SECRET_AP_PASS
  #define SECRET_AP_PASS "aspirator"
#endif

// SinricPro portalinda olusturdugun hiz ayarinin ID'si (portaldaki "ID" alani).
// Emin degilsen uygulamadan hizi bir kez degistir; seri monitor gelen ID'yi yazar.
const char* SPEED_SETTING_ID = "id_fan_speed";

// ================== SURUM & OTA ==================
// Her yeni surumde BURAYI ve GitHub'daki version.txt'yi ayni degere guncelle.
// Yayinlarken ONCE firmware.bin'i, SONRA version.txt'yi yukle.
#define FW_VERSION "1.2.0"
const char* OTA_VERSION_URL  = "https://raw.githubusercontent.com/kubilayatas/Aspirator/main/version.txt";
const char* OTA_FIRMWARE_URL = "https://raw.githubusercontent.com/kubilayatas/Aspirator/main/firmware.bin";
const unsigned long OTA_CHECK_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;  // 6 saatte bir kontrol
const unsigned long OTA_IDLE_REQUIRED_MS  = 60UL * 1000UL;               // cihaz 1 dk bostaysa guncelle
const int           OTA_MAX_TRIES         = 3;   // ayni surum bu kadar denenip tutmazsa birakilir

// ================== PINLER ==================
const uint8_t PIN_BTN_LIGHT = 2;   // TTP224 kanal 1
const uint8_t PIN_BTN_SPEED = 3;   // TTP224 kanal 2 -> dongusel hiz (0>1>2>3>0)
// GPIO 4 ve 5 (arizali / yedek kanallar) bilerek kullanilmiyor.

const uint8_t PIN_OUT_LIGHT = 6;                 // Triyak, Active-LOW
const uint8_t PIN_OUT_SPEED[3] = {7, 10, 21};    // Triyak, Active-LOW  (GPIO21 icin ust nota bak)
const uint8_t PIN_LED_LIGHT = 0;                 // LED, Active-LOW
const uint8_t PIN_LED_SPEED[3] = {1, 20, 8};     // LED, Active-LOW

// Panel LED'leri Active-LOW (pin LOW olunca yanar) - olculerek dogrulandi.
#define LED_ON   LOW
#define LED_OFF  HIGH

// ================== ZAMANLAMALAR ==================
const unsigned long DEBOUNCE_MS           = 50;
const unsigned long FACTORY_RESET_HOLD_MS = 8000;    // Isik tusuna 8 sn basili tut -> fabrika ayari
const unsigned long PORTAL_HOLD_MS        = 5000;    // Hiz tusuna 5 sn basili tut -> kurulum portali
const unsigned long SPEED_DEADTIME_MS     = 30;      // Hiz degisiminde iki sargi ayni anda enerjilenmesin
const unsigned long WIFI_CONNECT_TIMEOUT  = 30000;   // Kayitli aga baglanma denemesi
const unsigned long PORTAL_TIMEOUT_S      = 180;     // Kayitli ag varken portal ne kadar acik kalsin
const unsigned long WIFI_LOST_BEFORE_PORTAL = 120000;// Baglanti 2 dk koparsa portali ac
const unsigned long RESTART_DELAY_MS      = 1500;    // Ayar kaydedilince sayfa cevabi gitsin, sonra reset

const char* AP_NAME = "Aspirator_Kurulum";
const char* AP_PASS = SECRET_AP_PASS;

// ================== DURUM ==================
bool lightState   = false;
int  currentSpeed = 0;     // istenen hiz
int  appliedSpeed = 0;     // donanimda su an uygulanan hiz
unsigned long lastActivityMs = 0;

// ================== AYARLAR (NVS) ==================
Preferences prefs;
String appKey, appSecret, deviceId;

WiFiManager wm;
// 1. arguman formdaki alanin ADI'dir (deger degil); degerler NVS'den / secrets.h'den gelir.
// APP SECRET alani bilerek bos gosterilir: portal acikken yakindaki biri sayfadan okuyamasin.
WiFiManagerParameter pAppKey   ("appKey",    "SinricPro APP KEY",    "", 64);
WiFiManagerParameter pAppSecret("appSecret", "SinricPro APP SECRET", "", 128,
                                "placeholder='Bos birakirsan mevcut deger korunur'");
WiFiManagerParameter pDeviceId ("deviceId",  "SinricPro Device ID",  "", 40);
bool paramsChanged = false;
unsigned long paramsChangedAt = 0;

// ================== SINRICPRO ==================
SinricProSwitch* sinricDev = nullptr;
bool sinricStarted = false;
bool reportedLight = false, lightReportValid = false;
int  reportedSpeed = 0;     bool speedReportValid = false;

// ================== AG DURUM MAKINESI ==================
enum NetState { NET_CONNECTING, NET_PORTAL, NET_ONLINE };
NetState netState = NET_CONNECTING;
unsigned long netTimer = 0;
unsigned long wifiLostAt = 0;

// ================== OTA DURUMU ==================
bool otaCheckDue = false;
unsigned long lastOtaCheck = 0;
bool otaPending = false;
String otaNewVersion;

// =====================================================================
//  DOKUNMATIK TUS (debounce + kisa basma + uzun basma)
// =====================================================================
class TouchButton {
  public:
    enum Event { NONE, SHORT_PRESS, LONG_PRESS };
    TouchButton(uint8_t p, unsigned long holdMs) : pin(p), longMs(holdMs) {}
    void begin() { pinMode(pin, INPUT); }

    Event update() {
      bool raw = digitalRead(pin) == HIGH;
      unsigned long now = millis();
      if (raw != lastRaw) { lastRaw = raw; lastChange = now; }
      if (now - lastChange < DEBOUNCE_MS) return NONE;
      if (!raw && !stable) armed = true;   // tus bos gorulduyse hemen hazir

      if (raw != stable) {
        stable = raw;
        if (!stable) {                       // birakildi
          bool wasArmedPress = armed && pressed;
          armed = true;                      // tus en az bir kez bos goruldu
          pressed = false;
          if (wasArmedPress && !longFired) return SHORT_PRESS;
          return NONE;
        }
        pressed = true;                      // basildi
        pressStart = now;
        longFired = false;
        return NONE;
      }
      if (armed && pressed && !longFired && now - pressStart >= longMs) {
        longFired = true;
        return LONG_PRESS;
      }
      return NONE;
    }

  private:
    uint8_t pin;
    unsigned long longMs;
    bool lastRaw = false, stable = false, pressed = false, longFired = false;
    bool armed = false;   // acilista takili (surekli HIGH) kanal yanlislikla uzun basma uretmesin
    unsigned long lastChange = 0, pressStart = 0;
};

TouchButton btnLight(PIN_BTN_LIGHT, FACTORY_RESET_HOLD_MS);
TouchButton btnSpeed(PIN_BTN_SPEED, PORTAL_HOLD_MS);

// =====================================================================
//  DONANIM
// =====================================================================
// Pini OUTPUT yapmadan ONCE seviyesini ayarla -> acilista triyaklara LOW glitch'i gitmez
void initOutput(uint8_t pin, uint8_t level) {
  gpio_set_level((gpio_num_t)pin, level);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, level);
}

// Panel LED'lerini o anki duruma gore yeniden yakar (yanip sonme efektlerinden sonra)
void refreshLeds() {
  digitalWrite(PIN_LED_LIGHT, lightState ? LED_ON : LED_OFF);
  for (int i = 0; i < 3; i++)
    digitalWrite(PIN_LED_SPEED[i], appliedSpeed == i + 1 ? LED_ON : LED_OFF);
}

// Tum LED'leri birlikte yakip sondurur (sadece LED; triyaklara dokunmaz)
void blinkAllLeds(int times, unsigned long ms) {
  for (int n = 0; n < times * 2; n++) {
    uint8_t lvl = (n % 2 == 0) ? LED_ON : LED_OFF;
    digitalWrite(PIN_LED_LIGHT, lvl);
    for (int i = 0; i < 3; i++) digitalWrite(PIN_LED_SPEED[i], lvl);
    delay(ms);
  }
  refreshLeds();
}

void applyOutputs() {
  digitalWrite(PIN_OUT_LIGHT, lightState ? LOW : HIGH);
  digitalWrite(PIN_LED_LIGHT, lightState ? LED_ON : LED_OFF);

  if (currentSpeed != appliedSpeed) {
    // 1) Once tum hiz triyaklarini kapat
    for (int i = 0; i < 3; i++) {
      digitalWrite(PIN_OUT_SPEED[i], HIGH);
      digitalWrite(PIN_LED_SPEED[i], LED_OFF);
    }
    // 2) Eski triyak sifir geciste sonsun (50 Hz'de yarim periyot 10 ms)
    if (appliedSpeed != 0 && currentSpeed != 0) delay(SPEED_DEADTIME_MS);
    // 3) Yeni hizi ac
    if (currentSpeed >= 1 && currentSpeed <= 3) {
      digitalWrite(PIN_OUT_SPEED[currentSpeed - 1], LOW);
      digitalWrite(PIN_LED_SPEED[currentSpeed - 1], LED_ON);
      Serial.printf("[Hiz] %d. kademe -> triyak GPIO%d, LED GPIO%d\n", currentSpeed,
                    PIN_OUT_SPEED[currentSpeed - 1], PIN_LED_SPEED[currentSpeed - 1]);
    } else {
      Serial.println("[Hiz] Motor kapali");
    }
    appliedSpeed = currentSpeed;
  }
  lastActivityMs = millis();
}

// =====================================================================
//  SINRICPRO
// =====================================================================
// Sadece degisen durumlari gonderir. SinricPro her olay tipi icin saniyede 1 olay
// sinirina sahip; gonderilemeyen olay kaybolmaz, sonraki denemede gider.
void syncCloud() {
  if (!sinricStarted || !sinricDev || WiFi.status() != WL_CONNECTED || !SinricPro.isConnected()) return;
  static unsigned long lastTry = 0;
  if (millis() - lastTry < 200) return;
  lastTry = millis();

  if (!lightReportValid || reportedLight != lightState) {
    if (sinricDev->sendPowerStateEvent(lightState)) {
      reportedLight = lightState; lightReportValid = true;
    }
  }
  if (!speedReportValid || reportedSpeed != currentSpeed) {
    if (sinricDev->sendSettingEvent(SPEED_SETTING_ID, currentSpeed)) {
      reportedSpeed = currentSpeed; speedReportValid = true;
    }
  }
}

// Uygulama / asistan: Ac-Kapat -> isik
bool onPowerState(const String&, bool& state) {
  lightState = state;
  applyOutputs();
  reportedLight = state; lightReportValid = true;   // cevap zaten bu durumu bildiriyor
  return true;
}

// Uygulama: hiz ayari (0-3) -> motor
bool onSetting(const String&, const String& settingId, SettingValue& value) {
  int v;
  if      (std::holds_alternative<int>(value))    v = std::get<int>(value);
  else if (std::holds_alternative<float>(value))  v = (int)lroundf(std::get<float>(value));  // sayilar float gelir
  else if (std::holds_alternative<String>(value)) v = std::get<String>(value).toInt();
  else {
    Serial.printf("[Sinric] Ayar '%s' desteklenmeyen tipte\n", settingId.c_str());
    return false;
  }
  Serial.printf("[Sinric] Ayar geldi: id='%s' deger=%d\n", settingId.c_str(), v);

  if (settingId != SPEED_SETTING_ID) {
    Serial.printf("[Sinric] Bilinmeyen ayar ID'si. Koddaki SPEED_SETTING_ID'yi \"%s\" yap.\n", settingId.c_str());
    return false;
  }
  if (v < 0 || v > 3) return false;

  currentSpeed = v;
  applyOutputs();
  value = v;                                        // cevapta tam sayi donsun
  reportedSpeed = v; speedReportValid = true;
  return true;
}

bool credentialsValid() {
  return appKey.length() >= 30 && appSecret.length() >= 30 && deviceId.length() >= 20;
}

void startSinric() {
  if (sinricStarted) return;
  if (!credentialsValid()) {
    Serial.println("[Sinric] Bilgiler eksik, sadece yerel calisma. (Hiz tusuna 5 sn basip portaldan gir)");
    return;
  }
  // SinricPro[id] gecici bir Proxy dondurur; referansa baglamak onu gercek cihaza cevirir.
  SinricProSwitch& dev = SinricPro[deviceId];
  sinricDev = &dev;
  dev.onPowerState(onPowerState);
  dev.onSetSetting(onSetting);

  SinricPro.onConnected([]() {
    Serial.println("[Sinric] Baglandi");
    lightReportValid = false; speedReportValid = false;   // yeniden baglaninca durumu esitle
  });
  SinricPro.onDisconnected([]() { Serial.println("[Sinric] Baglanti koptu"); });

  SinricPro.begin(appKey, appSecret);
  sinricStarted = true;
  Serial.println("[Sinric] Baslatildi");
}

// =====================================================================
//  AYARLAR
// =====================================================================
// NVS'de anahtar varsa onu, yoksa varsayilani dondurur. Varsayilan (secrets.h) doluysa
// NVS'ye de yazar: boylece sonraki surumler EMBED_SECRETS 0 ile derlense de bilgiler kalir.
String loadOrSeed(const char* key, const char* fallback) {
  if (prefs.isKey(key)) return prefs.getString(key, "");
  if (fallback[0] != '\0') {
    prefs.putString(key, fallback);
    Serial.printf("[Ayar] '%s' secrets.h'den NVS'ye kopyalandi\n", key);
  }
  return String(fallback);
}

void loadSettings() {
  prefs.begin("aspirator", false);
  appKey    = loadOrSeed("appKey",    SECRET_APP_KEY);
  appSecret = loadOrSeed("appSecret", SECRET_APP_SECRET);
  deviceId  = loadOrSeed("deviceId",  SECRET_DEVICE_ID);

  pAppKey.setValue(appKey.c_str(), 64);
  pAppSecret.setValue("", 128);                 // gizli: formda gosterilmez
  pDeviceId.setValue(deviceId.c_str(), 40);
}

String cleanParam(WiFiManagerParameter& p) {
  String s = p.getValue();
  s.trim();
  return s;
}

// Portalda "Save" basilinca WiFiManager cagirir (WiFi sayfasi veya Setup sayfasi)
void onParamsSaved() {
  String k = cleanParam(pAppKey), s = cleanParam(pAppSecret), d = cleanParam(pDeviceId);
  pAppSecret.setValue("", 128);                 // yazilan secret sayfada tekrar gorunmesin
  if (k.isEmpty()) k = appKey;                  // bos birakilan alan eski degeri korur
  if (s.isEmpty()) s = appSecret;
  if (d.isEmpty()) d = deviceId;
  if (k == appKey && s == appSecret && d == deviceId) return;

  appKey = k; appSecret = s; deviceId = d;
  prefs.putString("appKey", appKey);
  prefs.putString("appSecret", appSecret);
  prefs.putString("deviceId", deviceId);
  paramsChanged = true;
  paramsChangedAt = millis();
  Serial.println("[Ayar] SinricPro bilgileri kaydedildi");
}

void factoryReset() {
  Serial.println("[Reset] Fabrika ayarlarina donuluyor...");
  lightState = false; currentSpeed = 0; applyOutputs();
  blinkAllLeds(3, 250);                         // tum LED'ler 3 kez yanip soner
  prefs.clear();
  wm.resetSettings();
  delay(300);
  ESP.restart();
}

// =====================================================================
//  AG
// =====================================================================
void startConnecting() {
  WiFi.mode(WIFI_STA);
  WiFi.begin();                       // WiFiManager'in kaydettigi SSID/sifre
  netState = NET_CONNECTING;
  netTimer = millis();
  Serial.println("[WiFi] Kayitli aga baglaniliyor...");
}

void startPortal(unsigned long timeoutS) {
  if (wm.getConfigPortalActive()) return;
  wm.setConfigPortalTimeout(timeoutS);          // 0 = suresiz
  wm.startConfigPortal(AP_NAME, AP_PASS);       // bloklamaz, tuslar calismaya devam eder
  netState = NET_PORTAL;
  Serial.printf("[WiFi] Kurulum agi acildi: %s (sifre: %s) -> 192.168.4.1\n", AP_NAME, AP_PASS);
}

void goOnline() {
  netState = NET_ONLINE;
  wifiLostAt = 0;
  Serial.print("[WiFi] Baglandi, IP: ");
  Serial.println(WiFi.localIP());
  startSinric();
  if (lastOtaCheck == 0) otaCheckDue = true;   // ilk baglantida bir kez kontrol et
}

void handleNetwork() {
  switch (netState) {
    case NET_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) goOnline();
      else if (millis() - netTimer > WIFI_CONNECT_TIMEOUT) startPortal(PORTAL_TIMEOUT_S);
      break;

    case NET_PORTAL:
      if (wm.process()) {                          // kullanici yeni WiFi girdi ve baglandi
        goOnline();
      } else if (!wm.getConfigPortalActive()) {    // portal zaman asimi
        if (WiFi.status() == WL_CONNECTED) {       // elle acilmisti, ag zaten var
          WiFi.mode(WIFI_STA);
          netState = NET_ONLINE;
          Serial.println("[WiFi] Portal kapandi, baglanti devam ediyor");
        } else {
          startConnecting();                       // kayitli agi tekrar dene
        }
      }
      break;

    case NET_ONLINE:
      if (WiFi.status() == WL_CONNECTED) {
        wifiLostAt = 0;
      } else {
        if (wifiLostAt == 0) wifiLostAt = millis();          // kisa kopmalarda ESP kendisi baglanir
        else if (millis() - wifiLostAt > WIFI_LOST_BEFORE_PORTAL) startPortal(PORTAL_TIMEOUT_S);
      }
      break;
  }

  // Calisan SinricPro baglantisi eski bilgilerle kuruldu; yenileri icin bastan basla.
  // (Ilk kurulumda SinricPro henuz baslamadigindan reset gerekmez.)
  if (paramsChanged && sinricStarted && millis() - paramsChangedAt > RESTART_DELAY_MS) {
    Serial.println("[Ayar] Yeni SinricPro bilgileriyle yeniden baslatiliyor...");
    delay(100);
    ESP.restart();
  }
}

// =====================================================================
//  OTA
// =====================================================================
// NVS'deki "otaVer"/"otaTries": indirilen son surum ve kac kez denendigi.
// firmware.bin'deki FW_VERSION, version.txt ile uyusmazsa cihaz surekli ayni dosyayi
// indirip flash'i yipratmasin diye ayni surum en fazla OTA_MAX_TRIES kez denenir.
bool isNewerVersion(const String& remote, const char* local) {
  int r[3] = {0, 0, 0}, l[3] = {0, 0, 0};
  if (sscanf(remote.c_str(), "%d.%d.%d", &r[0], &r[1], &r[2]) < 1) return false;
  sscanf(local, "%d.%d.%d", &l[0], &l[1], &l[2]);
  for (int i = 0; i < 3; i++) {
    if (r[i] > l[i]) return true;
    if (r[i] < l[i]) return false;
  }
  return false;
}

// Acilista: son OTA gercekten bekledigimiz surumu mu getirdi?
void otaBootCheck() {
  if (!prefs.isKey("otaVer")) return;
  String tried = prefs.getString("otaVer", "");
  if (tried == FW_VERSION) {
    Serial.printf("[OTA] %s basariyla kuruldu\n", FW_VERSION);
    prefs.remove("otaVer");
    prefs.remove("otaTries");
    return;
  }
  Serial.printf("[OTA] UYARI: %s indirildi ama cihaz %s calistiriyor. "
                "firmware.bin icindeki FW_VERSION ile version.txt ayni mi?\n", tried.c_str(), FW_VERSION);
  lastOtaCheck = millis() | 1;   // hemen tekrar indirme; normal kontrol araligini bekle
}

void checkForUpdate() {
  lastOtaCheck = millis() | 1;
  otaCheckDue = false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, OTA_VERSION_URL)) return;

  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String v = http.getString();
    v.trim();
    if (!isNewerVersion(v, FW_VERSION)) {
      Serial.printf("[OTA] Guncel (%s)\n", FW_VERSION);
    } else if (prefs.getString("otaVer", "") == v && prefs.getInt("otaTries", 0) >= OTA_MAX_TRIES) {
      Serial.printf("[OTA] %s zaten %d kez denendi, atlaniyor (yeni bir surum yayinlanana kadar)\n",
                    v.c_str(), OTA_MAX_TRIES);
    } else {
      otaPending = true;
      otaNewVersion = v;
      Serial.printf("[OTA] Yeni surum var: %s (mevcut %s)\n", v.c_str(), FW_VERSION);
    }
  } else {
    Serial.printf("[OTA] Surum kontrolu basarisiz, HTTP %d\n", code);
  }
  http.end();
}

void performUpdate() {
  Serial.printf("[OTA] %s indiriliyor...\n", otaNewVersion.c_str());
  lightState = false; currentSpeed = 0; applyOutputs();   // guvenli durum

  // Denemeyi reset oncesi kaydet (basarida cihaz yeniden baslar, geri donmeyiz)
  String prevVer = prefs.getString("otaVer", "");
  int prevTries  = (prevVer == otaNewVersion) ? prefs.getInt("otaTries", 0) : 0;
  prefs.putString("otaVer", otaNewVersion);
  prefs.putInt("otaTries", prevTries + 1);

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(client, OTA_FIRMWARE_URL);

  // Buraya sadece hata olursa gelinir. Indirme hatasi (ag vs.) deneme sayilmaz.
  Serial.printf("[OTA] Hata (%d): %s\n", httpUpdate.getLastError(),
                httpUpdate.getLastErrorString().c_str());
  if (prevVer.isEmpty()) prefs.remove("otaVer"); else prefs.putString("otaVer", prevVer);
  if (prevTries == 0)    prefs.remove("otaTries"); else prefs.putInt("otaTries", prevTries);
  (void)ret;
  otaPending = false;   // bir sonraki periyodik kontrolde tekrar denenir
}

void handleOta() {
  if (netState != NET_ONLINE || WiFi.status() != WL_CONNECTED) return;

  // Ag islemleri birkac saniye bloklar; sadece aspirator kapaliyken ve bir sure dokunulmamissa yap
  bool idle = !lightState && currentSpeed == 0 && (millis() - lastActivityMs > OTA_IDLE_REQUIRED_MS);
  if (!idle) return;

  if (otaPending) { performUpdate(); return; }
  if (otaCheckDue || millis() - lastOtaCheck > OTA_CHECK_INTERVAL_MS) checkForUpdate();
}

// Acilista LED'leri sirayla yakar: Isik > Hiz1 > Hiz2 > Hiz3
// (Sadece LED'ler; triyaklara dokunmaz, motor/lamba calismaz)
void ledSelfTest() {
  const uint8_t order[4] = {PIN_LED_LIGHT, PIN_LED_SPEED[0], PIN_LED_SPEED[1], PIN_LED_SPEED[2]};
  const char* names[4] = {"Isik", "Hiz 1", "Hiz 2", "Hiz 3"};
  for (int i = 0; i < 4; i++) {
    Serial.printf("[LED testi] %s LED'i (GPIO%d)\n", names[i], order[i]);
    digitalWrite(order[i], LED_ON);
    delay(400);
    digitalWrite(order[i], LED_OFF);
  }
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================
void setup() {
  // Cikislari ILK is olarak guvenli seviyeye cek
  initOutput(PIN_OUT_LIGHT, HIGH);
  for (int i = 0; i < 3; i++) initOutput(PIN_OUT_SPEED[i], HIGH);
  initOutput(PIN_LED_LIGHT, LED_OFF);
  for (int i = 0; i < 3; i++) initOutput(PIN_LED_SPEED[i], LED_OFF);

  btnLight.begin();
  btnSpeed.begin();

  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[Aspirator] Surum %s\n", FW_VERSION);
  ledSelfTest();

  loadSettings();
  otaBootCheck();

  wm.setConfigPortalBlocking(false);
  wm.setSaveParamsCallback(onParamsSaved);
  wm.setTitle("Akilli Aspirator");
  // "param" = sadece SinricPro bilgilerini degistirip WiFi sifresini yeniden yazmadan kaydetme sayfasi
  std::vector<const char*> menu = {"wifi", "param", "restart"};
  wm.setMenu(menu);
  wm.addParameter(&pAppKey);
  wm.addParameter(&pAppSecret);
  wm.addParameter(&pDeviceId);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (wm.getWiFiIsSaved()) startConnecting();
  else                     startPortal(0);   // ilk kurulum: portal kurulum yapilana kadar acik

  lastActivityMs = millis();
}

void loop() {
  // --- Fiziksel tuslar (internetten bagimsiz, her zaman calisir) ---
  switch (btnLight.update()) {
    case TouchButton::SHORT_PRESS:
      lightState = !lightState;
      applyOutputs();
      break;
    case TouchButton::LONG_PRESS:
      factoryReset();
      break;
    default: break;
  }

  switch (btnSpeed.update()) {
    case TouchButton::SHORT_PRESS:
      currentSpeed = (currentSpeed + 1) % 4;   // 0 > 1 > 2 > 3 > 0
      applyOutputs();
      break;
    case TouchButton::LONG_PRESS:
      Serial.println("[Tus] Kurulum portali elle aciliyor");
      blinkAllLeds(2, 150);
      startPortal(PORTAL_TIMEOUT_S);
      break;
    default: break;
  }

  // --- Ag / bulut ---
  handleNetwork();
  if (sinricStarted && WiFi.status() == WL_CONNECTED) SinricPro.handle();
  syncCloud();
  handleOta();
}
