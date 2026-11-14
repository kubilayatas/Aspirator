/*
 * Akilli Aspirator - ESP32-C3 Super Mini
 * SinricPro (SmartThings / Google Home / Alexa) + WiFiManager + GitHub OTA + Web arayuzu
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
 * Web arayuzu (cihaz ev agina bagliyken): http://aspirator.local  ya da  http://<IP>
 *   Kullanici: admin   Sifre: kurulum agi sifresi (varsayilan "aspirator")
 *   - Durum (surum, IP, WiFi sinyali, SinricPro, calisma suresi)
 *   - Isik ve motor kontrolu
 *   - Guncellemeleri kontrol et / bulunan surumu yukle
 *   - Otomatik guncelleme ac/kapat
 *   - Bilgisayardaki .bin dosyasindan guncelleme (USB'siz deneme icin)
 *   - Yeniden baslat
 *
 * Otomatik guncelleme (web arayuzunden kapatilabilir):
 *   - Acilista, internete baglanip cihaz 1 dk bos kalinca bir kez kontrol eder.
 *   - Sonra her gece 03:00-05:00 arasinda (cihaz bossa) bir kez kontrol eder.
 *   - Yeni surum bulursa, cihaz bossa kendisi yukler. Elle "kontrol et" yalnizca
 *     haber verir; yuklemek icin web arayuzunden "yukle"ye basilir.
 *
 * Arduino IDE ayarlari:
 *   Board            : ESP32C3 Dev Module   (Nologo Super Mini menusunde OTA'li buyuk sema yok)
 *   Partition Scheme : No FS 4MB (2MB APP x2)   veya   Minimal SPIFFS (1.9MB APP with OTA)
 *   USB CDC On Boot  : Enabled   (Seri monitor USB uzerinden calissin diye)
 *
 * Test edilen surumler: esp32 core 3.3.2, SinricPro 5.1.0, WiFiManager 2.0.17
 *
 * PIN DUZENI (PCB'ye gore):
 *   Triyaklar : GPIO0 Isik, GPIO1 Hiz1, GPIO3 Hiz2, GPIO4 Hiz3   (ozel gorevi olmayan pinler)
 *   LED'ler   : GPIO2 Isik, GPIO21 Hiz1, GPIO20 Hiz2, GPIO8 Hiz3
 *   Tuslar    : GPIO5 Isik, GPIO6 Hiz1, GPIO7 Hiz2, GPIO10 Hiz3
 * GPIO21/20 (UART0 TX/RX) acilista boot mesajlariyla darbelenir; orada sadece LED var
 * (acilista Hiz 1 LED'i bir an kirpisabilir, zararsiz). GPIO2 ve GPIO8 strapping
 * pinleridir; Active-LOW LED'ler onlari acilista HIGH tarafinda tuttugu icin sorun yok.
 * GPIO8'de kart uzerindeki mavi LED de var (Hiz 3 LED'iyle birlikte yanar).
 * GPIO9 (BOOT tusu) ve GPIO18/19 (USB) bos birakildi.
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <SinricPro.h>
#include <SinricProSwitch.h>
#include <SinricProFanUS.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_system.h"
#include <stdarg.h>

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

// Kurulum agi (Aspirator_Kurulum) ve web arayuzu sifresi. En az 8 karakter olmali.
// secrets.h icinde SECRET_AP_PASS tanimlarsan o kullanilir.
#ifndef SECRET_AP_PASS
  #define SECRET_AP_PASS "aspirator"
#endif

// WiFi verici gucunu dusur (19.5 dBm yerine 8.5 dBm). Zayif besleme yuzunden resetlenen
// Super Mini kartlarda ise yarar; bedeli kapsama alaninin kisalmasi. Guc sorunu donanimda
// cozuldugu icin kapali. Gerekirse 1 yap.
#define WIFI_LOW_TX_POWER 1

// ================== SURUM & OTA ==================
// Her yeni surumde BURAYI ve GitHub'daki version.txt'yi ayni degere guncelle.
// Yayinlarken ONCE firmware.bin'i, SONRA version.txt'yi yukle.
// Surum numarasi hicbir zaman geri gitmemeli: cihaz sadece kendisinden BUYUK surumu indirir.
#define FW_VERSION "1.0.0"
const char* OTA_VERSION_URL  = "https://raw.githubusercontent.com/kubilayatas/Aspirator/main/version.txt";
const char* OTA_FIRMWARE_URL = "https://raw.githubusercontent.com/kubilayatas/Aspirator/main/firmware.bin";
const unsigned long OTA_IDLE_REQUIRED_MS     = 60UL * 1000UL;            // otomatik islem icin 1 dk bos
const unsigned long OTA_FALLBACK_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL; // saat alinamazsa 24 saatte bir
const int           OTA_NIGHT_START_H        = 3;    // otomatik kontrol penceresi: 03:00 ...
const int           OTA_NIGHT_END_H          = 5;    // ... 05:00 (yerel saat)
const int           OTA_MAX_TRIES            = 3;    // ayni surum otomatik olarak en fazla bu kadar denenir

// Saat dilimi (gece penceresi icin). Turkiye: yil boyu UTC+3.
const char* TZ_INFO = "<+03>-3";

// Web arayuzu
const char* HOSTNAME = "aspirator";   // http://aspirator.local
const char* WEB_USER = "admin";       // sifre = kurulum agi sifresi (AP_PASS)

// ================== PINLER ==================
// Dokunmatik tuslar: TTP224 OUT1..OUT4 (varsayilan mod: dokununca HIGH, birakinca LOW)
enum Key { KEY_LIGHT = 0, KEY_SPEED1 = 1, KEY_SPEED2 = 2, KEY_SPEED3 = 3, KEY_COUNT = 4 };
const uint8_t PIN_TOUCH[KEY_COUNT] = {5, 6, 7, 10};  // T1 Isik, T2 Hiz1, T3 Hiz2, T4 Hiz3
const char*   KEY_NAMES[KEY_COUNT] = {"Isik", "Hiz 1", "Hiz 2", "Hiz 3"};

const uint8_t PIN_OUT_LIGHT = 0;                 // Triyak, Active-LOW
const uint8_t PIN_OUT_SPEED[3] = {1, 3, 4};      // Triyak, Active-LOW  (Hiz1, Hiz2, Hiz3)
const uint8_t PIN_LED_LIGHT = 2;                 // LED, Active-LOW
const uint8_t PIN_LED_SPEED[3] = {21, 20, 8};    // LED, Active-LOW     (Hiz1, Hiz2, Hiz3)

// Panel LED'leri Active-LOW (pin LOW olunca yanar) - olculerek dogrulandi.
#define LED_ON   LOW
#define LED_OFF  HIGH

// ================== ZAMANLAMALAR ==================
const unsigned long DEBOUNCE_MS           = 80;      // dokunus bu kadar kesintisiz surmeli (parazite karsi)
const unsigned long SWITCH_BLANK_MS       = 500;     // triyak anahtarlandiktan sonra dokunmalar bu sure yok sayilir
const unsigned long BOOT_BLANK_MS         = 3000;    // acilistan sonra TTP224 kalibrasyonu icin dokunmalar yok sayilir
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
unsigned long restartAt = 0;   // 0 degilse bu zamanda yeniden baslat (web cevabi gitsin diye)
bool appliedLight = false;     // donanimda su an uygulanan isik durumu
unsigned long touchBlankUntil = 0;   // bu ana kadar yeni dokunmalar yok sayilir

// ================== OLAY KAYDI (web arayuzundeki "Tani" bolumu) ==================
// Her degisiklik kaynagiyla (tus / SinricPro / web) kaydedilir: "kendi kendine" degisimlerin
// nereden geldigi USB baglamadan telefondan gorulebilir.
struct LogEntry { unsigned long ms; char text[64]; };
const int LOG_SIZE = 30;
LogEntry logBuf[LOG_SIZE];
int logHead = 0, logCount = 0;

void logEvent(const char* fmt, ...) {
  LogEntry& e = logBuf[logHead];
  e.ms = millis();
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.text, sizeof(e.text), fmt, ap);
  va_end(ap);
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
  Serial.printf("[Kayit] %s\n", e.text);
}

const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "elektrik verildi";
    case ESP_RST_BROWNOUT: return "BESLEME DUSTU (brownout)";
    case ESP_RST_SW:       return "yazilim yeniden baslatti";
    case ESP_RST_PANIC:    return "yazilim hatasi (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_EXT:      return "reset pini";
    default:               return "diger";
  }
}

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
bool otaAuto = true;                // otomatik kontrol + yukleme (NVS "otaAuto")
bool otaBootCheckDue = false;       // acilistaki tek kontrol bekleniyor mu
bool otaAutoInstall = false;        // otomatik kontrol yeni surum buldu, bos kalinca yukle
bool otaManualInstall = false;      // web arayuzunden "yukle"ye basildi
String otaAvailable;                // bulunan yeni surum ("" = yok / bilinmiyor)
String otaStatus = "Henüz kontrol edilmedi";
unsigned long otaLastCheckMs = 0;   // son kontrolun millis() degeri (0 = hic)
int lastAutoDay = -1;               // gece penceresinde en son kontrol edilen gun (yday)

// ================== WEB ==================
WebServer web(80);
bool webRoutesReady = false, webRunning = false, mdnsStarted = false, ntpStarted = false;
bool uploadActive = false, uploadOk = false;
String uploadErr;
void startWeb();
void stopWeb();

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
        if (blocked[i]) logEvent("Tus %s acilista basili, birakilana kadar yok sayiliyor", KEY_NAMES[i]);
      }
    }

    Event update() {
      unsigned long now = millis();

      // 1) Her kanali debounce et
      for (int i = 0; i < KEY_COUNT; i++) {
        bool r = digitalRead(PIN_TOUCH[i]) == HIGH;
        if (r != raw[i]) {
          if (!r && !stable[i]) glitchCount[i]++;   // kabul suresinden once biten kisa darbe = parazit
          raw[i] = r; changedAt[i] = now;
        }
        if (stable[i] != raw[i] && now - changedAt[i] >= DEBOUNCE_MS) {
          stable[i] = raw[i];
          if (stable[i]) acceptCount[i]++;
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
          if (count >= 1 && (long)(touchBlankUntil - now) > 0) {
            state = CANCELLED;                    // anahtarlama / acilis sonrasi: parazit olabilir
            blankedCount++;
            logEvent("Tus %s yok sayildi (anahtarlama/acilis sonrasi bekleme)", KEY_NAMES[which]);
          } else if (count == 1) {
            state = HOLD; active = which; pressStart = now; longReached = false;
          } else if (count > 1) {
            state = CANCELLED;
            logEvent("Ayni anda birden fazla tus, yok sayildi");
          }
          break;

        case HOLD: {
          if (count > 1 || (count == 1 && which != active)) {
            state = CANCELLED; longReached = false;
            logEvent("Tus %s basiliyken baska tusa dokunuldu, iptal", KEY_NAMES[active]);
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
            logEvent("Tus %s %lu sn basili kaldi, takili sayildi", KEY_NAMES[active], KEY_STUCK_MS / 1000);
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
    bool rawNow(int i) const { return raw[i]; }
    void resetCounters() {
      for (int i = 0; i < KEY_COUNT; i++) { acceptCount[i] = 0; glitchCount[i] = 0; }
      blankedCount = 0;
    }

    // Tani sayaclari (web arayuzunde gosterilir)
    unsigned long acceptCount[KEY_COUNT] = {};   // kabul edilen dokunuslar
    unsigned long glitchCount[KEY_COUNT] = {};   // kabul suresinden kisa darbeler (parazit)
    unsigned long blankedCount = 0;              // bekleme suresinde yok sayilanlar

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
  bool switched = (lightState != appliedLight) || (currentSpeed != appliedSpeed);
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
  if (switched) {
    appliedLight = lightState;
    touchBlankUntil = millis() + SWITCH_BLANK_MS;   // triyak parazitinin dokunus sanilmamasi icin
  }
  lastActivityMs = millis();
}

// Lamba ve motor kapali, bir suredir dokunulmamis mi? (otomatik guncelleme icin)
bool isIdle() {
  return !lightState && currentSpeed == 0 && (millis() - lastActivityMs > OTA_IDLE_REQUIRED_MS);
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
  logEvent("SinricPro: isik %s", state ? "ac" : "kapat");
  lightState = state;
  applyOutputs();
  reportedLight = state; lightReportValid = true;   // cevap zaten bu durumu bildiriyor
  return true;
}

// Uygulama / asistan: Fan cihazi ac-kapat. Acilinca en son kullanilan hiz.
bool onFanPower(const String&, bool& state) {
  logEvent("SinricPro: motor %s", state ? "ac" : "kapat");
  if (state) { if (currentSpeed == 0) currentSpeed = lastOnSpeed; }
  else       currentSpeed = 0;
  applyOutputs();
  reportedFanOn = state; fanPowerReportValid = true;
  return true;   // acilista hiz degistiyse syncCloud hiz olayini ayrica gonderir
}

// Uygulama / asistan: fan hizi 1..3. Fan kapaliyken hiz secilirse fan o hizda acilir.
bool onFanSpeed(const String&, int& value) {
  int v = constrain(value, 1, 3);
  logEvent("SinricPro: hiz %d", v);
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
  logEvent("SinricPro: hiz %+d -> %d", delta, v);
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
  paramsChanged = false;   // simdi baslatiliyorsa zaten en guncel bilgilerle baslar; reset gerekmez
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
  otaAuto   = prefs.getBool("otaAuto", true);

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
//  OTA
// =====================================================================
// NVS'deki "otaVer"/"otaTries": indirilen son surum ve kac kez denendigi.
// firmware.bin'deki FW_VERSION, version.txt ile uyusmazsa cihaz surekli ayni dosyayi
// indirip flash'i yipratmasin diye ayni surum otomatik olarak en fazla OTA_MAX_TRIES kez denenir.
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

bool timeSynced() { return time(nullptr) > 1700000000; }   // NTP'den saat alindi mi

// Saat biliniyorsa ve gece penceresindeyse true; yday'e yilin gununu yazar
bool inNightWindow(int& yday) {
  if (!timeSynced()) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  yday = t.tm_yday;
  return t.tm_hour >= OTA_NIGHT_START_H && t.tm_hour < OTA_NIGHT_END_H;
}

// Acilista: son OTA gercekten bekledigimiz surumu mu getirdi?
void otaBootCheck() {
  if (!prefs.isKey("otaVer")) return;
  String tried = prefs.getString("otaVer", "");
  if (tried == FW_VERSION) {
    Serial.printf("[OTA] %s basariyla kuruldu\n", FW_VERSION);
    otaStatus = String(FW_VERSION) + " başarıyla kuruldu";
    prefs.remove("otaVer");
    prefs.remove("otaTries");
    return;
  }
  Serial.printf("[OTA] UYARI: %s indirildi ama cihaz %s calistiriyor. "
                "firmware.bin icindeki FW_VERSION ile version.txt ayni mi?\n", tried.c_str(), FW_VERSION);
  otaStatus = "UYARI: " + tried + " indirildi ama cihaz " FW_VERSION " çalıştırıyor "
              "(firmware.bin içindeki FW_VERSION ile version.txt aynı mı?)";
}

// GitHub'daki version.txt'yi okur. automatic=true ise (acilis / gece kontrolu) yeni surum
// bulundugunda cihaz bos kalinca kendisi yukler; elle kontrolde sadece haber verir.
void checkForUpdate(bool automatic) {
  otaLastCheckMs = millis() | 1;
  if (automatic) otaBootCheckDue = false;
  int yday;
  if (automatic && inNightWindow(yday)) lastAutoDay = yday;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, OTA_VERSION_URL)) { otaStatus = "Kontrol başarısız (bağlantı kurulamadı)"; return; }

  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String v = http.getString();
    v.trim();
    if (!isNewerVersion(v, FW_VERSION)) {
      otaAvailable = "";
      otaStatus = "Güncel (GitHub'daki sürüm: " + v + ")";
      Serial.printf("[OTA] Guncel (%s, GitHub: %s)\n", FW_VERSION, v.c_str());
    } else {
      otaAvailable = v;
      bool exhausted = prefs.getString("otaVer", "") == v && prefs.getInt("otaTries", 0) >= OTA_MAX_TRIES;
      if (exhausted) {
        otaStatus = "Yeni sürüm var: " + v + " (otomatik olarak " + OTA_MAX_TRIES + " kez denendi, elle yükleyebilirsin)";
      } else {
        otaStatus = "Yeni sürüm var: " + v;
        if (automatic) otaAutoInstall = true;
      }
      Serial.printf("[OTA] Yeni surum var: %s (mevcut %s)%s\n", v.c_str(), FW_VERSION, exhausted ? " - deneme hakki doldu" : "");
    }
  } else {
    otaStatus = "Kontrol başarısız (HTTP " + String(code) + ")";
    Serial.printf("[OTA] Surum kontrolu basarisiz, HTTP %d\n", code);
  }
  http.end();
}

// GitHub'daki firmware.bin'i indirir ve kurar. Basarida cihaz yeniden baslar.
void performUpdate() {
  if (otaAvailable.isEmpty()) return;
  String ver = otaAvailable;
  Serial.printf("[OTA] %s indiriliyor...\n", ver.c_str());
  lightState = false; currentSpeed = 0; applyOutputs();   // guvenli durum
  stopWeb();                                               // indirme sirasinda web istekleri beklemesin

  // Denemeyi reset oncesi kaydet (basarida cihaz yeniden baslar, geri donmeyiz)
  String prevVer = prefs.getString("otaVer", "");
  int prevTries  = (prevVer == ver) ? prefs.getInt("otaTries", 0) : 0;
  prefs.putString("otaVer", ver);
  prefs.putInt("otaTries", prevTries + 1);

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(client, OTA_FIRMWARE_URL);

  // Buraya sadece hata olursa gelinir. Indirme hatasi (ag vs.) deneme sayilmaz.
  Serial.printf("[OTA] Hata (%d): %s\n", httpUpdate.getLastError(),
                httpUpdate.getLastErrorString().c_str());
  otaStatus = ver + " yüklenemedi: " + httpUpdate.getLastErrorString();
  if (prevVer.isEmpty()) prefs.remove("otaVer"); else prefs.putString("otaVer", prevVer);
  if (prevTries == 0)    prefs.remove("otaTries"); else prefs.putInt("otaTries", prevTries);
  (void)ret;
  startWeb();
}

void handleOta() {
  // Web arayuzunden istenen yukleme: cihaz bos olmasa da yapilir (lamba/motor kapatilir)
  if (otaManualInstall) {
    otaManualInstall = false;
    if (WiFi.status() == WL_CONNECTED) performUpdate();
    return;
  }

  if (!otaAuto || netState != NET_ONLINE || WiFi.status() != WL_CONNECTED || !isIdle()) return;

  if (otaAutoInstall) { otaAutoInstall = false; performUpdate(); return; }

  bool due = false;
  if (otaBootCheckDue) {
    due = true;                                    // acilistan sonraki ilk kontrol
  } else if (timeSynced()) {
    int yday;
    if (inNightWindow(yday) && yday != lastAutoDay) due = true;   // gece, gunde bir kez
  } else if (otaLastCheckMs && millis() - otaLastCheckMs > OTA_FALLBACK_INTERVAL_MS) {
    due = true;                                    // saat yoksa 24 saatte bir
  }
  if (due) checkForUpdate(true);
}

// =====================================================================
//  WEB ARAYUZU
// =====================================================================
bool webAuth() {
  if (web.authenticate(WEB_USER, AP_PASS)) return true;
  web.requestAuthentication(BASIC_AUTH, "Aspirator", "Giris gerekli");
  return false;
}

void redirectHome() {
  web.sendHeader("Location", "/");
  web.send(303);
}

String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length());
  for (char c : s) {
    if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;"; else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

String durationText(unsigned long ms) {
  unsigned long m = ms / 60000UL;
  if (m < 1) return "1 dakikadan az";
  if (m < 60) return String(m) + " dk";
  unsigned long h = m / 60;
  if (h < 48) return String(h) + " sa " + String(m % 60) + " dk";
  return String(h / 24) + " gün " + String(h % 24) + " sa";
}

const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html lang=tr><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Akıllı Aspiratör</title><style>"
  "body{font-family:system-ui,sans-serif;max-width:520px;margin:0 auto;padding:12px;background:#f4f5f7;color:#1d2330}"
  "h1{font-size:22px}h2{font-size:16px;margin:0 0 10px}"
  ".c{background:#fff;border-radius:10px;padding:14px;margin-bottom:12px;box-shadow:0 1px 3px #0002}"
  "table{width:100%;border-collapse:collapse}td{padding:4px 0}td:first-child{color:#667;width:42%}"
  "form{display:inline}button{font-size:15px;padding:9px 14px;margin:4px 4px 4px 0;border:1px solid #c5cad3;border-radius:8px;background:#fff}"
  "button.p{background:#2563eb;color:#fff;border-color:#2563eb}button.on{background:#dbe7ff;border-color:#2563eb}"
  ".m{color:#667;font-size:13px}input[type=file]{margin:6px 0}"
  "</style></head><body><h1>Akıllı Aspiratör</h1>";

void sendMessagePage(const String& title, const String& msg, int refreshSec) {
  String h = FPSTR(PAGE_HEAD);
  if (refreshSec > 0) h.replace("<title>", "<meta http-equiv=refresh content='" + String(refreshSec) + ";url=/'><title>");
  h += "<div class=c><h2>" + title + "</h2><p>" + msg + "</p><p><a href='/'>Ana sayfa</a></p></div></body></html>";
  web.send(200, "text/html; charset=utf-8", h);
}

// "Tani" bolumu: hem ev agindaki web arayuzunde hem de kurulum agindaki (192.168.4.1/tani)
// sayfada gosterilir. portal=true iken formlar kurulum sunucusuna gore yazilir.
String taniCard(bool portal) {
  String c;
  c.reserve(5000);
  // Cihazin o anki durumu ve ev agindaki adresi (kurulum aginda da gorunsun)
  // (Ev agindaki ana sayfada bu bilgiler zaten "Durum" kartinda var.)
  if (portal) {
    c += "<div class=c><h2>Durum</h2><table>";
    c += "<tr><td>Yazılım sürümü</td><td>" FW_VERSION "</td></tr>";
    c += "<tr><td>Işık</td><td>" + String(lightState ? "Açık" : "Kapalı") + "</td></tr>";
    c += "<tr><td>Motor</td><td>" + (currentSpeed ? String(currentSpeed) + ". kademe" : String("Kapalı")) + "</td></tr>";
    c += "<tr><td>Ev ağına bağlantı</td><td>" +
         (WiFi.status() == WL_CONNECTED
            ? htmlEscape(WiFi.SSID()) + " — adres: <b>http://" + WiFi.localIP().toString() + "</b>"
            : String("Bağlı değil")) + "</td></tr>";
    c += "<tr><td>Çalışma süresi</td><td>" + durationText(millis()) + "</td></tr>";
    c += "</table></div>";
  }
  c += "<div class=c><h2>Tanı</h2><table><tr><td><b>Tuş</b></td><td><b>Kabul / parazit / şu an</b></td></tr>";
  for (int i = 0; i < KEY_COUNT; i++)
    c += "<tr><td>" + String(KEY_NAMES[i]) + " (GPIO" + String(PIN_TOUCH[i]) + ")</td><td>" +
         String(panel.acceptCount[i]) + " / " + String(panel.glitchCount[i]) + " / " +
         String(panel.rawNow(i) ? "BASILI" : "boş") + "</td></tr>";
  c += "<tr><td>Bekleme sırasında yok sayılan</td><td>" + String(panel.blankedCount) + "</td></tr>";
  c += "<tr><td>Son yeniden başlama</td><td>" + String(resetReasonText()) + "</td></tr></table>";
  c += "<p class=m>Kabul: tuşa basma sayılan dokunuşlar. Parazit: " + String(DEBOUNCE_MS) +
       " ms'den kısa sürüp kaybolan sinyaller. Dokunmadığın halde sayılar artıyorsa o kanal parazit alıyor.</p>";
  c += "<p><b>Son olaylar</b> (en yenisi üstte)</p><ul class=m>";
  for (int n = 0; n < logCount; n++) {
    const LogEntry& e = logBuf[(logHead - 1 - n + LOG_SIZE) % LOG_SIZE];
    unsigned long ago = (millis() - e.ms) / 1000;
    String agoTxt = ago < 120 ? String(ago) + " sn" : durationText(millis() - e.ms);
    c += "<li>" + agoTxt + " önce: " + htmlEscape(e.text) + "</li>";
  }
  if (logCount == 0) c += "<li>Henüz olay yok</li>";
  if (portal)
    c += "</ul><form method=get action='/tani'><input type=hidden name=clear value=1><button>Kaydı ve sayaçları temizle</button></form>"
         "<form method=get action='/tani'><button>Yenile</button></form>"
         "<form method=get action='/'><button>Kurulum menüsü</button></form></div>";
  else
    c += "</ul><form method=post action='/clearlog'><button>Kaydı ve sayaçları temizle</button></form>"
         "<form method=get action='/'><button>Yenile</button></form></div>";

  return c;
}

void handleRoot() {
  if (!webAuth()) return;
  String h = FPSTR(PAGE_HEAD);
  h.reserve(9000);

  String sinric = !sinricStarted ? "Bilgiler eksik (kurulum portalından gir)"
                  : (SinricPro.isConnected() ? "Bağlı" : "Bağlı değil");
  h += "<div class=c><h2>Durum</h2><table>";
  h += "<tr><td>Işık</td><td>" + String(lightState ? "Açık" : "Kapalı") + "</td></tr>";
  h += "<tr><td>Motor</td><td>" + (currentSpeed ? String(currentSpeed) + ". kademe" : String("Kapalı")) + "</td></tr>";
  h += "<tr><td>Yazılım sürümü</td><td>" FW_VERSION "</td></tr>";
  h += "<tr><td>IP adresi</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  h += "<tr><td>Wi-Fi</td><td>" + htmlEscape(WiFi.SSID()) + " (" + String(WiFi.RSSI()) + " dBm)</td></tr>";
  h += "<tr><td>SinricPro</td><td>" + sinric + "</td></tr>";
  h += "<tr><td>Çalışma süresi</td><td>" + durationText(millis()) + "</td></tr>";
  h += "</table></div>";

  h += "<div class=c><h2>Kontrol</h2>";
  h += "<form method=post action='/set'><input type=hidden name=light value='" + String(lightState ? 0 : 1) + "'>"
       "<button class='" + String(lightState ? "on" : "") + "'>Işık: " + String(lightState ? "Kapat" : "Aç") + "</button></form><br>";
  const char* speedNames[4] = {"Kapalı", "1", "2", "3"};
  for (int i = 0; i < 4; i++) {
    h += "<form method=post action='/set'><input type=hidden name=speed value=" + String(i) + ">"
         "<button class='" + String(currentSpeed == i ? "on" : "") + "'>" + (i ? "Hız " : "Motor ") + speedNames[i] + "</button></form>";
  }
  h += "</div>";

  h += "<div class=c><h2>Yazılım güncelleme</h2><table>";
  h += "<tr><td>Son durum</td><td>" + htmlEscape(otaStatus) + "</td></tr>";
  if (otaLastCheckMs) h += "<tr><td>Son kontrol</td><td>" + durationText(millis() - otaLastCheckMs) + " önce</td></tr>";
  h += "<tr><td>Otomatik</td><td>" + String(otaAuto ? "Açık (açılışta ve her gece 03:00–05:00)" : "Kapalı") + "</td></tr>";
  h += "</table>";
  h += "<form method=post action='/check'><button>Güncellemeleri kontrol et</button></form>";
  if (!otaAvailable.isEmpty())
    h += "<form method=post action='/install'><button class=p>" + htmlEscape(otaAvailable) + " sürümünü yükle</button></form>";
  h += "<form method=post action='/auto'><button>Otomatik güncellemeyi " + String(otaAuto ? "kapat" : "aç") + "</button></form>";
  h += "<p class=m>Dosyadan yükle: Arduino IDE'de Sketch &gt; Export Compiled Binary ile çıkan "
       "<b>Aspirator.ino.bin</b> dosyasını seç (bootloader / partitions / merged dosyalarını değil).</p>"
       "<form method=post action='/upload' enctype='multipart/form-data'>"
       "<input type=file name=fw accept='.bin'><br><button>Dosyadan yükle</button></form>";
  h += "</div>";

  h += taniCard(false);

  h += "<div class=c><form method=post action='/restart'><button>Yeniden başlat</button></form>"
       "<p class=m>Wi-Fi veya SinricPro bilgilerini değiştirmek için Hız 1 tuşuna 5 sn basıp bırak: "
       "Aspirator_Kurulum ağı 3 dk açılır.</p></div></body></html>";

  web.send(200, "text/html; charset=utf-8", h);
}

// Kurulum aginda (192.168.4.1/tani) ayni tani sayfasi. Kurulum agi zaten sifreli oldugu icin
// ayrica giris istemez.
void handlePortalTani() {
  if (wm.server->hasArg("clear")) {
    logCount = 0; logHead = 0;
    panel.resetCounters();
  }
  String h = FPSTR(PAGE_HEAD);
  h += taniCard(true);
  h += "</body></html>";
  wm.server->send(200, "text/html; charset=utf-8", h);
}

void handleClearLog() {
  if (!webAuth()) return;
  logCount = 0; logHead = 0;
  panel.resetCounters();
  redirectHome();
}

void handleSet() {
  if (!webAuth()) return;
  if (web.hasArg("light")) lightState = web.arg("light") == "1";
  if (web.hasArg("speed")) currentSpeed = constrain(web.arg("speed").toInt(), 0, 3);
  logEvent("Web: isik %s, hiz %d", lightState ? "acik" : "kapali", currentSpeed);
  applyOutputs();
  redirectHome();
}

void handleCheck() {
  if (!webAuth()) return;
  if (WiFi.status() == WL_CONNECTED) checkForUpdate(false);
  redirectHome();
}

void handleInstall() {
  if (!webAuth()) return;
  if (otaAvailable.isEmpty()) { redirectHome(); return; }
  otaManualInstall = true;   // cevap gittikten sonra loop'ta yapilir
  sendMessagePage("Güncelleniyor",
                  htmlEscape(otaAvailable) + " indiriliyor. Lamba ve motor kapatıldı. "
                  "Yaklaşık 1 dakika sonra cihaz yeniden başlar; bu sayfa kendiliğinden yenilenir.", 75);
}

void handleAuto() {
  if (!webAuth()) return;
  otaAuto = !otaAuto;
  prefs.putBool("otaAuto", otaAuto);
  if (!otaAuto) otaAutoInstall = false;
  Serial.printf("[OTA] Otomatik guncelleme %s\n", otaAuto ? "acik" : "kapali");
  redirectHome();
}

void handleRestart() {
  if (!webAuth()) return;
  restartAt = millis() + 1000;
  sendMessagePage("Yeniden başlatılıyor", "Cihaz birkaç saniye içinde yeniden başlayacak.", 20);
}

// .bin dosyasi parca parca gelir. Ilk parcada dosyanin gercek bir uygulama imaji oldugu
// kontrol edilir (0xE9 imaj basligi + 32. bayttaki uygulama tanimi 0xABCD5432). Boylece
// yanlislikla secilen bootloader / partitions / merged dosyalari reddedilir.
void handleUploadChunk() {
  HTTPUpload& up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadOk = false; uploadErr = ""; uploadActive = false;
    if (!web.authenticate(WEB_USER, AP_PASS)) { uploadErr = "Yetkisiz"; return; }
    Serial.printf("[OTA] Dosyadan yukleme basladi: %s\n", up.filename.c_str());
    lightState = false; currentSpeed = 0; applyOutputs();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploadErr.isEmpty()) return;
    if (!uploadActive) {                                   // ilk veri parcasi
      const uint8_t* b = up.buf;
      bool isApp = up.currentSize >= 36 && b[0] == 0xE9 &&
                   b[32] == 0x32 && b[33] == 0x54 && b[34] == 0xCD && b[35] == 0xAB;
      if (!isApp) { uploadErr = "Bu dosya bir uygulama imajı değil (Aspirator.ino.bin dosyasını seç)"; return; }
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { uploadErr = Update.errorString(); return; }
      uploadActive = true;
    }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      uploadErr = Update.errorString(); Update.abort(); uploadActive = false;
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadActive) {
      if (Update.end(true)) uploadOk = true; else uploadErr = Update.errorString();
      uploadActive = false;
    } else if (uploadErr.isEmpty()) {
      uploadErr = "Dosya seçilmedi ya da boş";
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (uploadActive) Update.abort();
    uploadActive = false;
    uploadErr = "Yükleme yarıda kesildi";
  }
}

void handleUploadDone() {
  if (!webAuth()) return;
  if (uploadOk) {
    Serial.println("[OTA] Dosyadan yukleme tamam, yeniden baslatiliyor");
    restartAt = millis() + 1000;
    sendMessagePage("Yüklendi", "Yeni yazılım yazıldı, cihaz yeniden başlıyor. Bu sayfa kendiliğinden yenilenir.", 20);
  } else {
    Serial.printf("[OTA] Dosyadan yukleme basarisiz: %s\n", uploadErr.c_str());
    sendMessagePage("Yüklenemedi", htmlEscape(uploadErr) + ". Eski yazılım çalışmaya devam ediyor.", 0);
  }
}

void startWeb() {
  if (WiFi.status() != WL_CONNECTED || webRunning) return;
  if (!webRoutesReady) {
    web.on("/", HTTP_GET, handleRoot);
    web.on("/set", HTTP_POST, handleSet);
    web.on("/check", HTTP_POST, handleCheck);
    web.on("/install", HTTP_POST, handleInstall);
    web.on("/auto", HTTP_POST, handleAuto);
    web.on("/restart", HTTP_POST, handleRestart);
    web.on("/clearlog", HTTP_POST, handleClearLog);
    web.on("/upload", HTTP_POST, handleUploadDone, handleUploadChunk);
    web.onNotFound(redirectHome);
    webRoutesReady = true;
  }
  if (!mdnsStarted && MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
  }
  web.begin();
  webRunning = true;
  Serial.printf("[Web] http://%s.local  ya da  http://%s  (kullanici: %s)\n",
                HOSTNAME, WiFi.localIP().toString().c_str(), WEB_USER);
}

// Kurulum portali da 80. portu kullanir; portal acilmadan once kapatilir.
void stopWeb() {
  if (!webRunning) return;
  web.stop();
  webRunning = false;
}

// =====================================================================
//  AG
// =====================================================================
// setTxPower ancak WiFi (STA ya da AP) baslatildiktan sonra ise yarar; bu yuzden
// her WiFi.begin() ve portal acilisindan SONRA cagrilir.
void applyTxPower() {
#if WIFI_LOW_TX_POWER
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
}

void startConnecting() {
  WiFi.mode(WIFI_STA);
  WiFi.begin();                       // WiFiManager'in kaydettigi SSID/sifre
  applyTxPower();
  netState = NET_CONNECTING;
  netTimer = millis();
  Serial.println("[WiFi] Kayitli aga baglaniliyor...");
}

void startPortal(unsigned long timeoutS) {
  if (wm.getConfigPortalActive()) return;
  stopWeb();
  wm.setConfigPortalTimeout(timeoutS);          // 0 = suresiz
  wm.startConfigPortal(AP_NAME, AP_PASS);       // bloklamaz, tuslar calismaya devam eder
  applyTxPower();
  netState = NET_PORTAL;
  Serial.printf("[WiFi] Kurulum agi acildi: %s (sifre: %s) -> 192.168.4.1\n", AP_NAME, AP_PASS);
}

void goOnline() {
  netState = NET_ONLINE;
  wifiLostAt = 0;
  Serial.print("[WiFi] Baglandi, IP: ");
  Serial.println(WiFi.localIP());
  if (!ntpStarted) {
    configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
  }
  startSinric();
  startWeb();
  static bool bootCheckScheduled = false;            // acilistan sonra bir kez kontrol et
  if (!bootCheckScheduled) { bootCheckScheduled = true; otaBootCheckDue = true; }
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
          startWeb();
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

  // Portalda SinricPro bilgileri degisti (sayfa cevabi gitsin diye kisa bir sure beklenir):
  //  - SinricPro eski bilgilerle calisiyorsa: yenileri icin bastan basla.
  //  - Hic baslamamissa (ilk kez "param" sayfasindan girildi) ve ag varsa: simdi baslat.
  //    (startSinric paramsChanged'i temizler; ilk kurulumda gereksiz reset olmaz.)
  if (paramsChanged && millis() - paramsChangedAt > RESTART_DELAY_MS) {
    if (sinricStarted) {
      Serial.println("[Ayar] Yeni SinricPro bilgileriyle yeniden baslatiliyor...");
      delay(100);
      ESP.restart();
    } else if (WiFi.status() == WL_CONNECTED) {
      startSinric();
    }
  }
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

  logEvent("Acilis: surum %s, neden: %s", FW_VERSION, resetReasonText());
  panel.begin();

  loadSettings();
  otaBootCheck();

  wm.setConfigPortalBlocking(false);
  wm.setSaveParamsCallback(onParamsSaved);
  wm.setTitle("Akilli Aspirator");
  wm.setHostname(HOSTNAME);
  // "param" = sadece SinricPro bilgilerini degistirip WiFi sifresini yeniden yazmadan kaydetme sayfasi
  std::vector<const char*> menu = {"wifi", "param", "custom", "restart"};
  wm.setMenu(menu);
  // Kurulum menusune "Tani / Durum" dugmesi: 192.168.4.1/tani
  wm.setCustomMenuHTML("<form action='/tani' method='get'><button>Tani / Durum</button></form><br/>");
  wm.setWebServerCallback([]() { wm.server->on("/tani", HTTP_GET, handlePortalTani); });
  wm.addParameter(&pAppKey);
  wm.addParameter(&pAppSecret);
  wm.addParameter(&pLightId);
  wm.addParameter(&pFanId);

  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (wm.getWiFiIsSaved()) startConnecting();
  else                     startPortal(0);   // ilk kurulum: portal kurulum yapilana kadar acik

  lastActivityMs = millis();
  touchBlankUntil = millis() + BOOT_BLANK_MS;   // TTP224 acilis kalibrasyonunu bitirsin
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
    if (ev.key == KEY_LIGHT) {
      lightState = !lightState;
    } else {
      int s = ev.key;                                  // KEY_SPEED1..3 = 1..3
      currentSpeed = (currentSpeed == s) ? 0 : s;      // ayni hiza tekrar basmak motoru kapatir
    }
    logEvent("Tus %s -> isik %s, hiz %d", KEY_NAMES[ev.key], lightState ? "acik" : "kapali", currentSpeed);
    applyOutputs();
  } else if (ev.type == TouchPanel::LONG_PRESS) {
    logEvent("Tus %s uzun basildi", KEY_NAMES[ev.key]);
    if (ev.key == KEY_LIGHT) {
      factoryReset();
    } else if (ev.key == KEY_SPEED1) {
      Serial.println("[Tus] Kurulum portali elle aciliyor");
      blinkAllLeds(2, 150);
      startPortal(PORTAL_TIMEOUT_S);
    }
  }

  // --- Ag / bulut / web ---
  handleNetwork();
  if (sinricStarted && WiFi.status() == WL_CONNECTED) SinricPro.handle();
  syncCloud();
  if (webRunning) web.handleClient();
  handleOta();

  if (restartAt && (long)(millis() - restartAt) >= 0) {
    Serial.println("[Aspirator] Yeniden baslatiliyor");
    delay(100);
    ESP.restart();
  }
}
