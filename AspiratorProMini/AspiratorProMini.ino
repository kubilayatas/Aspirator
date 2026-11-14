/*
 * Aspirator - Arduino Pro Mini - Sadece dokunmatik kontrol
 * (Wi-Fi / bulut / OTA yok: tus -> cikis)
 *
 * Tuslar (TTP224, 4 kanal, dokununca HIGH). Her islem tus BIRAKILINCA yapilir:
 *   - Isik  : lamba ac/kapat
 *   - Hiz 1 : hiz 1 (zaten 1'deyse motor kapanir)
 *   - Hiz 2 : hiz 2 (zaten 2'deyse motor kapanir)
 *   - Hiz 3 : hiz 3 (zaten 3'teyse motor kapanir)
 *   Ayni anda birden fazla tusa dokunulursa hicbir sey yapilmaz.
 *   20 sn'den uzun basili kalan (takilan) kanal birakilana kadar yok sayilir.
 *
 * Arduino IDE: Board "Arduino Pro or Pro Mini", Processor kartina gore
 *   (ATmega328P 3.3V 8MHz  ya da  ATmega328P 5V 16MHz).
 *
 * Gerilim notu: ESP32-C3 cikislari 3.3V'tu. 5V Pro Mini kullanirsan triyak surucu
 * ve LED hatlari 5V gorur; LED direnclerinin ve triyak surucusunun buna uygun
 * oldugunu kontrol et. Ayni seviyeyi korumak icin 3.3V/8MHz surum en garantisi.
 */

// ================== PINLER ==================
// D0/D1 (seri port, yukleme icin) ve D13 (karttaki LED, acilista yanip soner)
// bilerek kullanilmadi. Triyaklari D13'e baglama: acilista tetiklenir.
const uint8_t KEY_COUNT = 4;
const uint8_t PIN_TOUCH[KEY_COUNT] = {6, 5, 3, 4};   // Isik, Hiz1, Hiz2, Hiz3

const uint8_t PIN_OUT_LIGHT    = 2;                  // Triyak, Active-LOW
const uint8_t PIN_OUT_SPEED[3] = {12, A3, 11};          // Triyak, Active-LOW (Hiz1, Hiz2, Hiz3)
const uint8_t PIN_LED_LIGHT    = 10;                 // LED, Active-LOW
const uint8_t PIN_LED_SPEED[3] = {9, 8, 7};       // LED, Active-LOW (Hiz1, Hiz2, Hiz3)

#define TRIAC_ON  LOW
#define TRIAC_OFF HIGH
#define LED_ON    LOW
#define LED_OFF   HIGH

// ================== ZAMANLAMALAR ==================
const unsigned long DEBOUNCE_MS       = 60;     // dokunus bu kadar kesintisiz surmeli (parazite karsi)
const unsigned long KEY_STUCK_MS      = 20000;  // bundan uzun basili kanal "takili" sayilir
const unsigned long SPEED_DEADTIME_MS = 30;     // hiz degisiminde iki sargi ayni anda enerjilenmesin

// Seri porttan tus olaylarini yazdirmak icin 1 yap (USB-seri donusturucu ile, 9600 baud)
#define DEBUG 0

// ================== DURUM ==================
bool lightState   = false;
int  currentSpeed = 0;   // 0 = kapali, 1..3
int  appliedSpeed = 0;

// ================== TUS OKUMA ==================
bool raw[KEY_COUNT], stable[KEY_COUNT], blocked[KEY_COUNT];
unsigned long changedAt[KEY_COUNT];

enum PanelState { IDLE, HOLD, CANCELLED };
PanelState panelState = IDLE;
int activeKey = -1;
unsigned long pressStart = 0;

// Pin cikis yapilmadan ONCE HIGH yazilir -> acilista triyaklara LOW darbesi gitmez
void initOutput(uint8_t pin, uint8_t level) {
  digitalWrite(pin, level);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, level);
}

void applyOutputs() {
  digitalWrite(PIN_OUT_LIGHT, lightState ? TRIAC_ON : TRIAC_OFF);
  digitalWrite(PIN_LED_LIGHT, lightState ? LED_ON : LED_OFF);

  if (currentSpeed != appliedSpeed) {
    // 1) Once tum hiz triyaklarini kapat
    for (uint8_t i = 0; i < 3; i++) {
      digitalWrite(PIN_OUT_SPEED[i], TRIAC_OFF);
      digitalWrite(PIN_LED_SPEED[i], LED_OFF);
    }
    // 2) Eski triyak sifir geciste sonsun
    if (appliedSpeed != 0 && currentSpeed != 0) delay(SPEED_DEADTIME_MS);
    // 3) Yeni hizi ac
    if (currentSpeed >= 1 && currentSpeed <= 3) {
      digitalWrite(PIN_OUT_SPEED[currentSpeed - 1], TRIAC_ON);
      digitalWrite(PIN_LED_SPEED[currentSpeed - 1], LED_ON);
    }
    appliedSpeed = currentSpeed;
  }
}

// Tus birakildiginda basilan tusun numarasini, yoksa -1 dondurur
int readPanel() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < KEY_COUNT; i++) {
    bool r = digitalRead(PIN_TOUCH[i]) == HIGH;
    if (r != raw[i]) { raw[i] = r; changedAt[i] = now; }
    if (stable[i] != raw[i] && now - changedAt[i] >= DEBOUNCE_MS) {
      stable[i] = raw[i];
      if (!stable[i]) blocked[i] = false;          // takili kanal birakildi
    }
  }

  int count = 0, which = -1;
  for (uint8_t i = 0; i < KEY_COUNT; i++)
    if (stable[i] && !blocked[i]) { count++; which = i; }

  switch (panelState) {
    case IDLE:
      if (count == 1) { panelState = HOLD; activeKey = which; pressStart = now; }
      else if (count > 1) panelState = CANCELLED;
      break;

    case HOLD:
      if (count > 1 || (count == 1 && which != activeKey)) {
        panelState = CANCELLED;                     // iki tusa birden dokunuldu
      } else if (count == 0) {
        panelState = IDLE;                          // birakildi -> islem
        return activeKey;
      } else if (now - pressStart >= KEY_STUCK_MS) {
        blocked[activeKey] = true;                  // takili kanal
        panelState = CANCELLED;
#if DEBUG
        Serial.print(F("Takili kanal: ")); Serial.println(activeKey);
#endif
      }
      break;

    case CANCELLED:
      if (count == 0) panelState = IDLE;
      break;
  }
  return -1;
}

void setup() {
  // Cikislari ILK is olarak guvenli seviyeye cek
  initOutput(PIN_OUT_LIGHT, TRIAC_OFF);
  for (uint8_t i = 0; i < 3; i++) initOutput(PIN_OUT_SPEED[i], TRIAC_OFF);
  initOutput(PIN_LED_LIGHT, LED_OFF);
  for (uint8_t i = 0; i < 3; i++) initOutput(PIN_LED_SPEED[i], LED_OFF);

#if DEBUG
  Serial.begin(9600);
  Serial.println(F("Aspirator Pro Mini"));
#endif

  // Acilis LED testi (sadece LED'ler; bu sirada TTP224 de kalibrasyonunu bitirir)
  const uint8_t order[4] = {PIN_LED_LIGHT, PIN_LED_SPEED[0], PIN_LED_SPEED[1], PIN_LED_SPEED[2]};
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(order[i], LED_ON);
    delay(250);
    digitalWrite(order[i], LED_OFF);
  }

  unsigned long now = millis();
  for (uint8_t i = 0; i < KEY_COUNT; i++) {
    pinMode(PIN_TOUCH[i], INPUT);
    raw[i] = stable[i] = digitalRead(PIN_TOUCH[i]) == HIGH;
    blocked[i] = stable[i];                         // acilista basili -> birakilana kadar yok say
    changedAt[i] = now;
  }
}

void loop() {
  int key = readPanel();
  if (key < 0) return;

#if DEBUG
  Serial.print(F("Tus: ")); Serial.println(key);
#endif

  if (key == 0) {
    lightState = !lightState;
  } else {
    currentSpeed = (currentSpeed == key) ? 0 : key;   // ayni hiza tekrar basmak motoru kapatir
  }
  applyOutputs();
}
