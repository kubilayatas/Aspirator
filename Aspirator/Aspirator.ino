/*
 * Akilli Aspirator - ESP32-C3 Super Mini
 * SinricPro (SmartThings / Google Home / Alexa) + WiFiManager + GitHub OTA
 *
 * SinricPro'da IKI cihaz kullanilir (SmartThings ve asistanlar sadece standart
 * cihaz tiplerini tanidigi icin hiz, Fan cihazinin kendi hiz ayari uzerinden gelir):
 *   - "Switch" tipi cihaz  -> Isik (ac/kapat)
 *   - "Fan" tipi cihaz     -> Motor: ac/kapat + hiz 1..3
 *     Fan acilinca en son kullanilan hizda baslar; kapaliyken hiz secilirse fan acilir.
 *
 * Fiziksel tuslar (TTP224, 4 kanal). Her islem tus BIRAKILINCA yapilir:
 *   - T1 Isik   kisa : isik ac/kapat
 *   - T1 Isik   8 sn : fabrika ayari (WiFi + SinricPro bilgileri silinir)
 *   - T2 Hiz 1  kisa : hiz 1 (zaten 1'deyse motor kapanir)
 *   - T2 Hiz 1  5 sn : kurulum agini (portal) 3 dk icin ac -> WiFi'yi ya da
 *                      SinricPro bilgilerini sifirlamadan degistirmek icin
 *   - T3 Hiz 2  kisa : hiz 2 (zaten 2'deyse motor kapanir)
 *   - T4 Hiz 3  kisa : hiz 3 (zaten 3'teyse motor kapanir)
 *   Uzun basma esigi gecilince tum LED'ler yanar: "simdi birakabilirsin" demektir.
 *   Ayni anda birden fazla tusa dokunulursa basma iptal edilir; tum tuslar
 *   birakilana kadar hicbir sey yapilmaz. 20 sn'den uzun basili kalan (takilan)
 *   kanal birakilana kadar yok sayilir, diger tuslar calismaya devam eder.
 *
 * Arduino IDE ayarlari:
 *   Board            : ESP32C3 Dev Module   (Nologo Super Mini menusunde OTA'li buyuk sema yok)
 *   Partition Scheme : No FS 4MB (2MB APP x2)   veya   Minimal SPIFFS (1.9MB APP with OTA)
 *   USB CDC On Boot  : Enabled   (Seri monitor USB uzerinden calissin diye)
 *
 * Test edilen surumler: esp32 core 3.3.2, SinricPro 5.1.0, WiFiManager 2.0.17
 *
 * PIN NOTU: GPIO21 (UART0 TX) acilista ROM boot mesajlariyla darbelenir; bu yuzden
 * orada triyak degil sadece bir LED var (acilista bir an kirpisabilir, zararsiz).
 * GPIO8/GPIO9 strapping pinleridir; GPIO8'de sadece LED (kart LED'i de orada),
 * GPIO9 (BOOT tusu) bos birakildi.
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <SinricPro.h>
#include <SinricProSwitch.h>
#include <SinricProFanUS.h>
#include "driver/gpio.h"

// Hiz 1 ve Hiz 2 LED'leri GPIO21/GPIO20'de; bunlar ayni zamanda UART0 TX/RX pinleri.
// "USB CDC On Boot: Disabled" iken Serial UART0'i kullanir ve bu iki pini ele gecirir:
// Hiz 2 LED'i hic yanmaz, Hiz 1 LED'i her seri mesajda kisa yanip soner.
// Bu yuzden Enabled secilmeden derlenmesin.
#if !ARDUINO_USB_CDC_ON_BOOT
  #error "Arduino IDE > Tools > USB CDC On Boot: 'Enabled' secmelisin (Disabled iken Serial GPIO20/21'deki LED'leri bozar)"
#endif

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
  #undef  SECRET_FAN_ID
  #define SECRET_APP_KEY    ""
  #define SECRET_APP_SECRET ""
  #define SECRET_DEVICE_ID  ""
#else
  #warning "EMBED_SECRETS=1: SinricPro APP SECRET bu derlemenin icinde. Bu .bin dosyasini GitHub'a KOYMA!"
#endif

// SECRET_DEVICE_ID = Isik (Switch) cihazi, SECRET_FAN_ID = Motor (Fan) cihazi.
// Eski secrets.h'de SECRET_FAN_ID yoksa bos kabul edilir; portalin Setup sayfasindan da girilebilir.
#ifndef SECRET_FAN_ID
  #define SECRET_FAN_ID ""
#endif

// Kurulum agi (Aspirator_Kurulum) sifresi. En az 8 karakter olmali.
// secrets.h icinde SECRET_AP_PASS tanimlarsan o kullanilir.
#ifndef SECRET_AP_PASS
  #define SECRET_AP_PASS "aspirator"
#endif

// WiFi verici gucunu dusur (19.5 dBm yerine 8.5 dBm). WiFi yayini sirasindaki akim
// darbelerini azaltir; zayif besleme yuzunden resetlenen ya da agi zor bulan Super Mini
// kartlarda ise yarar. Bedeli: kapsama alani kisalir. Router uzaksa 0 birak.
#define WIFI_LOW_TX_POWER 1

// ================== SURUM & OTA ==================
// Her yeni surumde BURAYI ve GitHub'daki version.txt'yi ayni degere guncelle.
// Yayinlarken ONCE firmware.bin'i, SONRA version.txt'yi yukle.
#define FW_VERSION "1.4.0"
const char* OTA_VERSION_URL  = "https://raw.githubusercontent.com/kubilayatas/Aspirator/main/version.txt";
const char* OTA_FIRMWARE_URL = "https://raw.githubusercontent.com/kubilayatas/Aspirator/main/firmware.bin";
const unsigned long OTA_CHECK_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;  // 6 saatte bir kontrol
const unsigned long OTA_IDLE_REQUIRED_MS  = 60UL * 1000UL;               // cihaz 1 dk bostaysa guncelle
const int           OTA_MAX_TRIES         = 3;   // ayni surum bu kadar denenip tutmazsa birakilir

// ================== PINLER ==================
// Dokunmatik tuslar: TTP224 OUT1..OUT4 (varsayilan mod: dokununca HIGH, birakinca LOW)
enum Key { KEY_LIGHT = 0, KEY_SPEED1 = 1, KEY_SPEED2 = 2, KEY_SPEED3 = 3, KEY_COUNT = 4 };
const uint8_t PIN_TOUCH[KEY_COUNT] = {2, 3, 4, 5};   // T1 Isik, T2 Hiz1, T3 Hiz2, T4 Hiz3
const char*   KEY_NAMES[KEY_COUNT] = {"Isik", "Hiz 1", "Hiz 2", "Hiz 3"};

const uint8_t PIN_OUT_LIGHT = 6;                 // Triyak, Active-LOW
const uint8_t PIN_OUT_SPEED[3] = {7, 10, 1};     // Triyak, Active-LOW  (Hiz1, Hiz2, Hiz3)
const uint8_t PIN_LED_LIGHT = 0;                 // LED, Active-LOW
const uint8_t PIN_LED_SPEED[3] = {21, 20, 8};    // LED, Active-LOW     (Hiz1, Hiz2, Hiz3)

// Panel LED'leri Active-LOW (pin LOW olunca yanar) - olculerek dogrulandi.
#define LED_ON   LOW
#define LED_OFF  HIGH

// ================== ZAMANLAMALAR ==================
const unsigned long DEBOUNCE_MS           = 50;
const unsigned long FACTORY_RESET_HOLD_MS = 8000;    // Isik tusu 8 sn basili + birak -> fabrika ayari
const unsigned long PORTAL_HOLD_MS        = 5000;    // Hiz 1 tusu 5 sn basili + birak -> kurulum portali
const unsigned long KEY_STUCK_MS          = 20000;   // Bundan uzun basili kalan kanal "takili" sayilir
// Her tusun uzun basma esigi (0 = uzun basma islevi yok, ne kadar basili tutulsa da kisa basma sayilir)
const unsigned long LONG_PRESS_MS[KEY_COUNT] = {FACTORY_RESET_HOLD_MS, PORTAL_HOLD_MS, 0, 0};
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
int  lastOnSpeed  = 1;     // fan "ac" komutu gelince kullanilacak son hiz
unsigned long lastActivityMs = 0;

// ================== AYARLAR (NVS) ==================
Preferences prefs;
String appKey, appSecret, lightId, fanId;   // lightId NVS'de "deviceId" anahtariyla durur (eski surumle uyumlu)

WiFiManager wm;
// 1. arguman formdaki alanin ADI'dir (deger degil); degerler NVS'den / secrets.h'den gelir.
// APP SECRET alani bilerek bos gosterilir: portal acikken yakindaki biri sayfadan okuyamasin.
WiFiManagerParameter pAppKey   ("appKey",    "SinricPro APP KEY",    "", 64);
WiFiManagerParameter pAppSecret("appSecret", "SinricPro APP SECRET", "", 128,
                                "placeholder='Bos birakirsan mevcut deger korunur'");
WiFiManagerParameter pLightId  ("deviceId",  "Isik cihazi Device ID (Switch)", "", 40);
WiFiManagerParameter pFanId    ("fanId",     "Motor cihazi Device ID (Fan)",   "", 40);
bool paramsChanged = false;
unsigned long paramsChangedAt = 0;

// ================== SINRICPRO ==================
SinricProSwitch* lightDev = nullptr;
SinricProFanUS*  fanDev   = nullptr;
bool sinricStarted = false;
bool reportedLight = false,    lightReportValid = false;
bool reportedFanOn = false,    fanPowerReportValid = false;
int  reportedFanSpeed = 0;     bool fanSpeedReportValid = false;

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
//  DOKUNMATIK PANEL (4 tus, ayni anda tek tus, islem birakinca)
// =====================================================================
// Kurallar:
//  - Her kanal ayri ayri debounce edilir.
//  - Tum tuslar bosken TEK bir tusa basilirsa o tus "aday" olur.
//  - Aday basiliyken baska bir tusa da dokunulursa basma IPTAL edilir ve tum tuslar
//    birakilana kadar hicbir sey kabul edilmez (iki tusa birden basmak = hicbir sey).
//  - Islem ancak aday tus birakildiginda ve o an baska tus basili degilken uretilir.
//    Basili kalma suresi uzun basma esigini gectiyse LONG, gecmediyse SHORT olur.
//  - Acilista basili gorunen ya da KEY_STUCK_MS'den uzun basili kalan kanal "takili"
//    sayilir; o kanal birakilana kadar yok sayilir, diger tuslar calismaya devam eder.
class TouchPanel {
  public:
    enum Type { NONE, SHORT_PRESS, LONG_PRESS };
    struct Event { Type type; int key; };

    void begin() {
      unsigned long now = millis();
      for (int i = 0; i < KEY_COUNT; i++) {
        pinMode(PIN_TOUCH[i], INPUT);
        raw[i] = stable[i] = (digitalRead(PIN_TOUCH[i]) == HIGH);
        changedAt[i] = now;
        blocked[i] = stable[i];                  // acilista basili -> birakilana kadar yok say
        if (blocked[i]) Serial.printf("[Tus] %s acilista basili, birakilana kadar yok sayiliyor\n", KEY_NAMES[i]);
      }
    }

    Event update() {
      unsigned long now = millis();

      // 1) Her kanali debounce et
      for (int i = 0; i < KEY_COUNT; i++) {
        bool r = digitalRead(PIN_TOUCH[i]) == HIGH;
        if (r != raw[i]) { raw[i] = r; changedAt[i] = now; }
        if (stable[i] != raw[i] && now - changedAt[i] >= DEBOUNCE_MS) {
          stable[i] = raw[i];
          if (!stable[i] && blocked[i]) {
            blocked[i] = false;
            Serial.printf("[Tus] %s birakildi, tekrar aktif\n", KEY_NAMES[i]);
          }
        }
      }

      // 2) Gecerli (takili olmayan) basili tuslari say
      int count = 0, which = -1;
      for (int i = 0; i < KEY_COUNT; i++)
        if (stable[i] && !blocked[i]) { count++; which = i; }

      // 3) Durum makinesi
      switch (state) {
        case IDLE:
          if (count == 1) {
            state = HOLD; active = which; pressStart = now; longReached = false;
          } else if (count > 1) {
            state = CANCELLED;
            Serial.println("[Tus] Ayni anda birden fazla tus, yok sayildi");
          }
          break;

        case HOLD: {
          if (count > 1 || (count == 1 && which != active)) {
            state = CANCELLED; longReached = false;
            Serial.println("[Tus] Basiliyken baska tusa dokunuldu, iptal");
            break;
          }
          unsigned long held = now - pressStart;
          if (count == 0) {                       // aday tus birakildi -> islem
            state = IDLE;
            bool isLong = LONG_PRESS_MS[active] > 0 && held >= LONG_PRESS_MS[active];
            longReached = false;
            return { isLong ? LONG_PRESS : SHORT_PRESS, active };
          }
          if (held >= KEY_STUCK_MS) {             // cok uzun basili: takili kanal
            blocked[active] = true;
            state = CANCELLED; longReached = false;
            Serial.printf("[Tus] %s %lu sn'dir basili, takili sayildi\n", KEY_NAMES[active], KEY_STUCK_MS / 1000);
            break;
          }
          if (!longReached && LONG_PRESS_MS[active] > 0 && held >= LONG_PRESS_MS[active]) {
            longReached = true;
            Serial.printf("[Tus] %s uzun basma esigi gecildi, birakinca uygulanacak\n", KEY_NAMES[active]);
          }
          break;
        }

        case CANCELLED:
          if (count == 0) state = IDLE;          // hepsi birakilinca yeniden hazir
          break;
      }
      return { NONE, -1 };
    }

    // Uzun basma esigi gecildi ve tus hala basili mi? (LED geri bildirimi icin)
    bool longReady() const { return state == HOLD && longReached; }

  private:
    enum State { IDLE, HOLD, CANCELLED };
    State state = IDLE;
    int active = -1;
    bool longReached = false;
    unsigned long pressStart = 0;
    bool raw[KEY_COUNT] = {}, stable[KEY_COUNT] = {}, blocked[KEY_COUNT] = {};
    unsigned long changedAt[KEY_COUNT] = {};
};

TouchPanel panel;

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
  if (currentSpeed > 0) lastOnSpeed = currentSpeed;
  lastActivityMs = millis();
}

// =====================================================================
//  SINRICPRO
// =====================================================================
// Sadece degisen durumlari gonderir. SinricPro her olay tipi icin saniyede 1 olay
// sinirina sahip; gonderilemeyen olay kaybolmaz, sonraki denemede gider.
void syncCloud() {
  if (!sinricStarted || WiFi.status() != WL_CONNECTED || !SinricPro.isConnected()) return;
  static unsigned long lastTry = 0;
  if (millis() - lastTry < 200) return;
  lastTry = millis();

  if (lightDev && (!lightReportValid || reportedLight != lightState)) {
    if (lightDev->sendPowerStateEvent(lightState)) {
      reportedLight = lightState; lightReportValid = true;
    }
  }

  if (fanDev) {
    bool fanOn = currentSpeed > 0;
    if (!fanPowerReportValid || reportedFanOn != fanOn) {
      if (fanDev->sendPowerStateEvent(fanOn)) {
        reportedFanOn = fanOn; fanPowerReportValid = true;
      }
    }
    // Fan hizi 1..3 araliginda; kapaliyken hiz gonderilmez (uygulama son hizi gostermeye devam eder)
    if (fanOn && (!fanSpeedReportValid || reportedFanSpeed != currentSpeed)) {
      if (fanDev->sendRangeValueEvent(currentSpeed)) {
        reportedFanSpeed = currentSpeed; fanSpeedReportValid = true;
      }
    }
  }
}

// Uygulama / asistan: Isik cihazi ac-kapat
bool onLightPower(const String&, bool& state) {
  Serial.printf("[Sinric] Isik %s\n", state ? "ac" : "kapat");
  lightState = state;
  applyOutputs();
  reportedLight = state; lightReportValid = true;   // cevap zaten bu durumu bildiriyor
  return true;
}

// Uygulama / asistan: Fan cihazi ac-kapat. Acilinca en son kullanilan hiz.
bool onFanPower(const String&, bool& state) {
  Serial.printf("[Sinric] Fan %s\n", state ? "ac" : "kapat");
  if (state) { if (currentSpeed == 0) currentSpeed = lastOnSpeed; }
  else       currentSpeed = 0;
  applyOutputs();
  reportedFanOn = state; fanPowerReportValid = true;
  return true;   // acilista hiz degistiyse syncCloud hiz olayini ayrica gonderir
}

// Uygulama / asistan: fan hizi 1..3. Fan kapaliyken hiz secilirse fan o hizda acilir.
bool onFanSpeed(const String&, int& value) {
  int v = constrain(value, 1, 3);
  Serial.printf("[Sinric] Fan hizi %d\n", v);
  currentSpeed = v;
  applyOutputs();
  value = v;
  reportedFanSpeed = v; fanSpeedReportValid = true;
  return true;   // fan kapaliyken acildiysa syncCloud "acik" olayini ayrica gonderir
}

// Alexa "hizi artir/azalt" gibi goreli komutlar: value = degisim miktari, cevapta yeni mutlak hiz
bool onFanSpeedAdjust(const String&, int& delta) {
  int base = currentSpeed > 0 ? currentSpeed : lastOnSpeed;
  int v = constrain(base + delta, 1, 3);
  Serial.printf("[Sinric] Fan hizi %+d -> %d\n", delta, v);
  currentSpeed = v;
  applyOutputs();
  delta = v;
  reportedFanSpeed = v; fanSpeedReportValid = true;
  return true;
}

bool idLooksValid(const String& id) { return id.length() >= 20; }

bool credentialsValid() {
  return appKey.length() >= 30 && appSecret.length() >= 30 && (idLooksValid(lightId) || idLooksValid(fanId));
}

void startSinric() {
  if (sinricStarted) return;
  if (!credentialsValid()) {
    Serial.println("[Sinric] Bilgiler eksik, sadece yerel calisma. (Hiz 1 tusuna 5 sn basip birak, portaldan gir)");
    return;
  }
  // SinricPro[id] gecici bir Proxy dondurur; referansa baglamak onu gercek cihaza cevirir.
  if (idLooksValid(lightId)) {
    SinricProSwitch& l = SinricPro[lightId];
    lightDev = &l;
    l.onPowerState(onLightPower);
  } else {
    Serial.println("[Sinric] Isik cihazi ID'si yok, isik sadece tustan calisir");
  }
  if (idLooksValid(fanId)) {
    SinricProFanUS& f = SinricPro[fanId];
    fanDev = &f;
    f.onPowerState(onFanPower);
    f.onRangeValue(onFanSpeed);
    f.onAdjustRangeValue(onFanSpeedAdjust);
  } else {
    Serial.println("[Sinric] Fan cihazi ID'si yok, motor sadece tustan calisir");
  }

  SinricPro.onConnected([]() {
    Serial.println("[Sinric] Baglandi");
    // yeniden baglaninca tum durumu esitle
    lightReportValid = false; fanPowerReportValid = false; fanSpeedReportValid = false;
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
  lightId   = loadOrSeed("deviceId",  SECRET_DEVICE_ID);
  fanId     = loadOrSeed("fanId",     SECRET_FAN_ID);

  pAppKey.setValue(appKey.c_str(), 64);
  pAppSecret.setValue("", 128);                 // gizli: formda gosterilmez
  pLightId.setValue(lightId.c_str(), 40);
  pFanId.setValue(fanId.c_str(), 40);
}

String cleanParam(WiFiManagerParameter& p) {
  String s = p.getValue();
  s.trim();
  return s;
}

// Portalda "Save" basilinca WiFiManager cagirir (WiFi sayfasi veya Setup sayfasi)
void onParamsSaved() {
  String k = cleanParam(pAppKey), s = cleanParam(pAppSecret);
  String l = cleanParam(pLightId), f = cleanParam(pFanId);
  pAppSecret.setValue("", 128);                 // yazilan secret sayfada tekrar gorunmesin
  if (k.isEmpty()) k = appKey;                  // bos birakilan alan eski degeri korur
  if (s.isEmpty()) s = appSecret;
  if (l.isEmpty()) l = lightId;
  if (f.isEmpty()) f = fanId;
  if (k == appKey && s == appSecret && l == lightId && f == fanId) return;

  appKey = k; appSecret = s; lightId = l; fanId = f;
  prefs.putString("appKey", appKey);
  prefs.putString("appSecret", appSecret);
  prefs.putString("deviceId", lightId);
  prefs.putString("fanId", fanId);
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

void applyTxPower() {
#if WIFI_LOW_TX_POWER
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
}

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

  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[Aspirator] Surum %s\n", FW_VERSION);
  ledSelfTest();      // ~1.6 sn; bu arada TTP224 de acilis kalibrasyonunu bitirir

  panel.begin();

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
  wm.addParameter(&pLightId);
  wm.addParameter(&pFanId);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (wm.getWiFiIsSaved()) startConnecting();
  else                     startPortal(0);   // ilk kurulum: portal kurulum yapilana kadar acik

  lastActivityMs = millis();
}

void loop() {
  // --- Fiziksel tuslar (internetten bagimsiz, her zaman calisir) ---
  TouchPanel::Event ev = panel.update();

  // Uzun basma esigi gecildiyse tum LED'ler yanar ("simdi birak"); bitince eski hale doner
  static bool longFeedback = false;
  if (panel.longReady() != longFeedback) {
    longFeedback = panel.longReady();
    if (longFeedback) {
      digitalWrite(PIN_LED_LIGHT, LED_ON);
      for (int i = 0; i < 3; i++) digitalWrite(PIN_LED_SPEED[i], LED_ON);
    } else {
      refreshLeds();
    }
  }

  if (ev.type == TouchPanel::SHORT_PRESS) {
    Serial.printf("[Tus] %s\n", KEY_NAMES[ev.key]);
    if (ev.key == KEY_LIGHT) {
      lightState = !lightState;
    } else {
      int s = ev.key;                                  // KEY_SPEED1..3 = 1..3
      currentSpeed = (currentSpeed == s) ? 0 : s;      // ayni hiza tekrar basmak motoru kapatir
    }
    applyOutputs();
  } else if (ev.type == TouchPanel::LONG_PRESS) {
    Serial.printf("[Tus] %s (uzun)\n", KEY_NAMES[ev.key]);
    if (ev.key == KEY_LIGHT) {
      factoryReset();
    } else if (ev.key == KEY_SPEED1) {
      Serial.println("[Tus] Kurulum portali elle aciliyor");
      blinkAllLeds(2, 150);
      startPortal(PORTAL_TIMEOUT_S);
    }
  }

  // --- Ag / bulut ---
  handleNetwork();
  if (sinricStarted && WiFi.status() == WL_CONNECTED) SinricPro.handle();
  syncCloud();
  handleOta();
}
