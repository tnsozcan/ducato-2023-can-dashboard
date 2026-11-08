// Ducato 2023 (00468637450) - ETS2 / BeamNG oyun firmware'i
// SADECE aractan sokulmus gosterge ile, MASADA kullanin.
// Nano ATmega328P + MCP2515 (8 MHz) + TJA1050, mcp_can 1.5.1. CS D10, INT D2. Serial 115200.
// Modul gostergenin 500 kbit/s hattina baglanir.
//
// X250 projesindeki PC koprulerinin (ets2_bridge.py / beamng_bridge.py) paketini aynen anlar:
//   game RPM SPEED_RAW FUEL_RAW TEMP_RAW LIGHTS TURNS PARK [IGNITION CRUISE] [ENGINE WARNINGS]
// Degerler X250 olceginde gelir, burada 2023 gostergesinin mesajlarina cevrilir.
// Komutlar: game, stop, status.
//
// Durumlar:
//   OFF      -> hicbir sey gonderilmez, gosterge uyur
//   RUN      -> oyun degerleri (kontak acik)
//   SHUTDOWN -> kontak kapaninca / oyun durunca ibreleri 0'a indirip sonra susar
// Kontak acilisi (X250 gibi):
//   - 3 sn isik testi: ABS, fren, cekis kontrol lambalari (4AF kesilerek) + motor ariza lambasi
//     (5AE b4 0x10). Kirmizi hararet otuyordu, cikarildi.
//   - ibre supurmesi YOK (hiz ibresi tavana cikinca gosterge asiri hiz diye otuyordu)
//   - kizdirma lambasi motor sicakligina gore 2/3/5/7 sn, mars ile soner
//   - marsa kadar 0FC b6 = 00 (devir ibresi uyur, marsta uyanir); motor ariza lambasi marstan 2 sn sonra soner (5AE b4 0x10)

#include <SPI.h>
#include <mcp_can.h>

const uint8_t CS_PIN = 10;
MCP_CAN can(CS_PIN);

const uint16_t SHUTDOWN_MS = 1500;   // kapanista ibreleri indirme suresi
const uint16_t LAMP_CHECK_MS = 3000; // kontak acilinca isik testi (motor lambasi)
const uint16_t ABS_CHECK_MS = 5000;  // ABS/fren/cekis seti: 4AF bu kadar kesilir (lambalar ~1 sn gec yanar)
const uint16_t LAMP_DELAY_MS = 1200; // kizdirma + motor lambasi bu kadar gec baslar ki ABS setiyle ayni anda yansin
                                     // (gosterge ABS lambalarini 4AF zaman asimindan sonra yakiyor)

enum State : uint8_t { OFF, RUN, SHUTDOWN };
State state = OFF;
uint32_t stateStart = 0;

// ---- oyundan gelen son durum ----
uint32_t lastPacket = 0;
bool havePacket = false;
uint16_t gRpm = 0, gSpeedRaw = 0, gFuelRaw = 0, gTempRaw = 40;
uint8_t gLights = 0;
uint16_t gWarn = 0;            // koprunun WARNINGS alani (BeamNG bitleri asagida)
bool gPark = false;            // el freni
bool gIgnition = false, gEngine = false;
uint32_t ignOnTime = 0;        // kontak acildigi an
uint32_t engineStartTime = 0;  // marsin tuttugu an
const uint16_t MIL_AFTER_START_MS = 2000;  // motor ariza lambasi marstan 2 sn sonra soner
uint16_t glowMs = 0;           // kizdirma suresi
bool glowDone = true;          // mars basildi ya da sure doldu

uint32_t t50 = 0, t100 = 0;
uint32_t txOk = 0, txFail = 0;

void setState(State s) { state = s; stateStart = millis(); }

void send(uint32_t id, const uint8_t *d) {
  if (can.sendMsgBuf(id, 0, 8, (uint8_t *)d) == CAN_OK) txOk++;
  else { txFail++; can.abortTX(); }
}

// X250 hiz olcegi (koprudeki interpolasyon) -> km/s. Noktalar: 0->0, 560->40, 720->50, 1504->100, 2800->180
uint16_t rawToKmh(uint16_t raw) {
  const uint16_t r[] = {0, 560, 720, 1504, 2800};
  const uint8_t  k[] = {0, 40, 50, 100, 180};
  if (raw >= r[4]) return 180;
  for (uint8_t i = 0; i < 4; i++)
    if (raw <= r[i + 1])
      return k[i] + (uint32_t)(raw - r[i]) * (k[i + 1] - k[i]) / (r[i + 1] - r[i]);
  return 180;
}

bool lampDelay() { return state == RUN && millis() - ignOnTime < LAMP_DELAY_MS; }
bool lampCheck() { return state == RUN && !lampDelay() && millis() - ignOnTime < LAMP_DELAY_MS + LAMP_CHECK_MS; }
bool absCheck()  { return state == RUN && millis() - ignOnTime < ABS_CHECK_MS; }
bool glowOn() {
  if (state != RUN || glowDone) return false;
  if (lampDelay()) return false;                          // ABS setiyle ayni anda baslasin
  if (millis() - ignOnTime >= LAMP_DELAY_MS + glowMs) { glowDone = true; return false; }
  return true;
}

// O anki durumda gosterilecek devir ve hiz
void currentNeedles(uint16_t &rpm, uint16_t &kmh) {
  if (state == RUN) {
    rpm = gRpm;
    kmh = rawToKmh(gSpeedRaw);
  } else {
    rpm = 0; kmh = 0;      // SHUTDOWN: ibreleri indir
  }
}

void sendRpm(uint16_t rpm) {
  // 0FC: b0 = devir/64, b1 = 40 (gecerli), b6 = FF motor calisiyor (sadece devir ibresini uyandirir; hiz ibresi 101 ile kalkar)
  //      motor calismiyorken b6 = 00 -> devir ibresi uyur (aku lambasi bununla YANMIYOR, bulunamadi)
  bool running = state == RUN && gEngine;
  uint8_t d[8] = {0, 0x40, 0, 0, 0, 0, (uint8_t)(running ? 0xFF : 0x00), 0};
  uint16_t v = (rpm + 32) / 64;
  d[0] = v > 0xFF ? 0xFF : v;
  send(0x0FC, d);
}

void sendAll100(uint16_t kmh) {
  uint8_t d[8];

  // 101 hiz: b0:b1 = km/s x 2
  uint16_t sv = kmh * 2;
  memset(d, 0, 8); d[0] = sv >> 8; d[1] = sv & 0xFF;
  send(0x101, d);

  // 545 yakit + aydinlatma: X250 yakit 0..96 (0x60 = dolu) -> 2023 b2 0..0C, b3 80 = yarim adim
  uint16_t f = gFuelRaw > 96 ? 96 : gFuelRaw;
  uint16_t halfSteps = (uint32_t)f * 24 / 96;          // 0..24 yarim adim
  memset(d, 0, 8);
  d[2] = halfSteps / 2;
  d[3] = (halfSteps & 1) ? 0x80 : 0x00;
  if (d[2] == 0 && d[3] == 0) d[2] = 0x01;              // 00 gecersiz gibi davraniyor, en dusuk degeri kullan
  bool dim = gLights & (0x20 | 0x08);                    // park lambasi ya da kisa far -> gece modu
  d[4] = dim ? 0xFF : 0x00;
  send(0x545, d);

  // Modul "var" mesajlari: uyari lambalari sonuk kalsin
  memset(d, 0xFF, 8);                     send(0x0DE, d);   // direksiyon (EPS)
  bool check = lampCheck();
  bool run = state == RUN;
  // WARNINGS bitleri (BeamNG koprusu; ETS2 sadece 1 ve 2 gonderir):
  //   4 ABS/patinaj aktif | 32 kemer | 64 motor arizasi | 256 yakitta su | 8192 airbag yanip soner
  // Isik testinde 4AF gonderilmez -> ABS, fren, cekis kontrol, cekis OFF lambalari birlikte yanar
  if (!absCheck()) {
    memset(d, 0, 8); d[1] = 0x80;                          // ABS / ESC normal
    if (run && (gWarn & 4)) d[0] |= 0x08;                  // cekis kontrol lambasi yanip soner (kayma)
    // Not: b0 0x10 = EBD arizasi (kirmizi fren lambasi + "fren sistemi arizali"), el freni DEGIL.
    // Gercek el freni bulunamadi (muhtemelen 125k govde hatti); gPark su an kullanilmiyor.
    send(0x4AF, d);
  }
  memset(d, 0, 8); d[0] = 0x20;                            // airbag + kemer normal
  if (run && (gWarn & 8192)) d[0] = 0x40;                  // airbag yanip soner
  if (run && (gWarn & 32)) d[4] |= 0x04;                   // LCD kemer (sofor)
  send(0x257, d);
  memset(d, 0, 8); send(0x226, d);                         // motor beyni var (yoksa gosterge motor lambasi yakar)

  // 5AE: b3 = kizdirma, b4 0x10 = motor ariza lambasi, b4 0x20 = kirmizi hararet, b4 0x80 = yakit filtresi
  memset(d, 0, 8);
  if (glowOn()) d[3] = 0xFF;
  if (state == RUN && gTempRaw >= 40 + 110) d[4] = 0x20;  // kirmizi hararet sadece oyunda 110 C ustu (isik testinde otuyordu)
  bool milStart = run && !lampDelay() && (!gEngine || millis() - engineStartTime < MIL_AFTER_START_MS);
  if (check || milStart || (run && (gWarn & 64))) d[4] |= 0x10;
                                                           // motor ariza lambasi: kontak acik-motor durgun, marstan 2 sn sonra soner,
                                                           // isik testi ya da oyunda motor arizasi
  if (state == RUN && (gWarn & 256)) d[4] |= 0x80;      // yakit filtresi / yakitta su
  send(0x5AE, d);
}

// ---- seri komutlar ----
char line[80]; uint8_t len = 0;

void handleLine() {
  char *tok[13]; uint8_t n = 0;
  char *p = strtok(line, " \t");
  while (p && n < 13) { tok[n++] = p; p = strtok(NULL, " \t"); }
  if (!n) return;

  if (!strcmp(tok[0], "game") && n >= 8) {
    gRpm      = atoi(tok[1]);
    gSpeedRaw = atoi(tok[2]);
    gFuelRaw  = atoi(tok[3]);
    gTempRaw  = atoi(tok[4]);
    gLights   = atoi(tok[5]);
    bool ign  = n >= 9 ? atoi(tok[8]) != 0 : true;
    bool eng  = n >= 11 ? atoi(tok[10]) != 0 : gRpm > 300;
    if (eng && !gEngine) engineStartTime = millis();      // mars tuttu
    gEngine   = eng;
    gWarn     = n >= 12 ? (uint16_t)atol(tok[11]) : 0;
    gPark     = atoi(tok[7]) != 0;
    lastPacket = millis(); havePacket = true;

    if (ign && (state == OFF || state == SHUTDOWN)) {
      Serial.println(F("KONTAK ACIK: isik testi"));
      setState(RUN);
      ignOnTime = millis();
      // Kizdirma suresi motor sicakligina gore (X250 ile ayni yaklasim, gercek ECU hesabi degil)
      // TEMP_RAW = sicaklik + 40: <0 C 7 sn, 0-20 C 5 sn, 20-60 C 3 sn, 60 C ustu 2 sn
      glowMs = gTempRaw < 40 ? 7000 : (gTempRaw < 60 ? 5000 : (gTempRaw < 100 ? 3000 : 2000));
      glowDone = gEngine;                     // motor zaten calisiyorsa kizdirma yok
    } else if (!ign && state == RUN) {
      Serial.println(F("KONTAK KAPALI: ibreler iniyor"));
      setState(SHUTDOWN);
    }
    gIgnition = ign;
    return;
  }
  if (!strcmp(tok[0], "stop") || !strcmp(tok[0], "allstop")) {
    if (state == RUN) setState(SHUTDOWN);
    Serial.println(F("STOP: ibreler iniyor, sonra TX durur")); return;
  }
  if (!strcmp(tok[0], "status")) {
    const char *names[] = {"OFF", "RUN", "SHUTDOWN"};
    Serial.print(names[state]);
    Serial.print(F(" rpm=")); Serial.print(gRpm);
    Serial.print(F(" kmh=")); Serial.print(rawToKmh(gSpeedRaw));
    Serial.print(F(" fuel=")); Serial.print(gFuelRaw);
    Serial.print(F(" temp=")); Serial.print((int)gTempRaw - 40);
    Serial.print(F(" lights=")); Serial.print(gLights);
    Serial.print(F(" ign=")); Serial.print(gIgnition);
    Serial.print(F(" eng=")); Serial.print(gEngine);
    Serial.print(F(" | TX ok=")); Serial.print(txOk); Serial.print(F(" fail=")); Serial.print(txFail);
    Serial.print(F(" TEC=")); Serial.print(can.errorCountTX());
    Serial.print(F(" EFLG=0x")); Serial.println(can.getError(), HEX);
    return;
  }
  Serial.println(F("HATA: komutlar: game, stop, status"));
}

void setup() {
  Serial.begin(115200);
  while (can.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) != CAN_OK) {
    Serial.println(F("MCP2515 baslatilamadi, tekrar..."));
    delay(500);
  }
  can.setMode(MCP_NORMAL);
  Serial.println(F("Ducato 2023 GAME | MCP2515 8 MHz | 500 kbit/s | CS D10"));
  Serial.println(F("Hazir: PC koprusunu baslatin. Acilista TX yok. Komutlar: game / stop / status."));
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (len) { line[len] = 0; handleLine(); len = 0; } }
    else if (len < sizeof(line) - 1) line[len++] = c;
  }

  uint32_t now = millis();

  // Kopru 1 sn sessiz kalirsa (oyun kapandi / kopru durdu) ibreleri indir
  if (state == RUN && havePacket && now - lastPacket > 1000) {
    Serial.println(F("Paket gelmiyor: ibreler iniyor"));
    setState(SHUTDOWN);
  }
  // Durum gecisleri
  if (state == RUN && gEngine) glowDone = true;   // mars: kizdirma soner
  if (state == SHUTDOWN && now - stateStart >= SHUTDOWN_MS) {
    setState(OFF);
    Serial.println(F("TX durdu, gosterge uyuyor"));
  }
  if (state == OFF) return;

  uint16_t rpm, kmh;
  currentNeedles(rpm, kmh);
  if (now - t50 >= 50)   { t50 = now;  sendRpm(rpm); }
  if (now - t100 >= 100) { t100 = now; sendAll100(kmh); }

  if (can.getError() & 0x20) {        // bus-off olursa toparla
    can.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ);
    can.setMode(MCP_NORMAL);
  }
}
