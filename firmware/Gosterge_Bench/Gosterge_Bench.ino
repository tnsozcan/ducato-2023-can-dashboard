// Gosterge_Bench - masa ustu gosterge CAN araci (Ducato 2023 / Transit / X250)
// SADECE aractan sokulmus gosterge ile kullan!
// Nano + MCP2515 (8 MHz) + TJA1050, mcp_can 1.5.1. CS D10, INT D2. Serial 115200, "Yeni satir".
//
// Komutlar (hex, bosluklu):
//   send 3A0 11 22 33          -> tek mesaj gonder
//   rep 3A0 100 11 22 33       -> 100 ms'de bir surekli gonder (8 slot; ayni ID guncellenir)
//   byte 3A0 2 FF              -> tekrar eden 3A0'in 2. baytini FF yap (0'dan sayilir)
//   unrep 3A0                  -> o ID'nin tekrarini durdur
//   stop                       -> tum tekrarlari durdur
//   ids                        -> gostergenin gonderdigi ID'ler: adet, siklik, son veri
//   clear                      -> ID tablosunu sifirla
//   mon                        -> gelen her mesaji canli bas (ac/kapat)
//   rate 125                   -> hiz: 500 / 250 / 125 / 100 / 50
//   stat                       -> hata sayaclari ve aktif tekrarlar
//   scan 000 7FF 200 FF FF FF FF FF FF FF FF
//                              -> 000'dan 7FF'e her ID'yi 200 ms gonderir; gostergenin bir mesaji
//                                 degisirse ya da yeni ID cikarsa ">>>" ile yazar (veri vermezsen 8x FF)
//   qscan 380 3BF 1000 FF ...  -> ayni tarama, 5 sn bekleme yok (onceki ogrenilen ID'leri kullanir)
//   xscan 1E340000 1E3400FF 1 1000 00 1C
//                              -> 29 bitlik tarama: baslangic, bitis, ADIM (hex), sure, veri
//   xscan 00004000 1FFF4000 10000 200 FF FF ...  -> gonderen 4000 sabit, KONU taramasi
//   bits 380 1000              -> 380'in 64 bitini tek tek yakar, her adim 1 sn (100 ms'de bir gonderir)
//   bits 380 1000 FF FF FF FF FF FF FF FF -> taban FF iken bitleri tek tek SONDURUR
//   bytes 356 0 1000           -> 356'nin B0 baytina 00..FF tum degerleri sirayla dener (her biri 1 sn)
//                                 (taban verirsen diger baytlar o olur; sure 0 = elle n/p)
//   m kaput                    -> tarama sirasinda (scan/qscan/xscan/bits/bytes) o anki ID/adimi not ile isaretle
//   marks                      -> isaretleri tekrar listele (tarama bitince/scanstop'ta kendisi basar)
//   bits 356 0                 -> ELLE bit taramasi: 'n' sonraki bit, 'p' onceki bit (sen ilerletirsin)
//   bits 1E340000 1000 00 00  -> 2 baytlik taban verirsen sadece 16 bit tarar (uzunluk = verdigin bayt sayisi)
//   scanstop                   -> taramayi / bit taramasini durdur
// 7FF'ten buyuk ID yazarsan extended (29 bit) gonderilir.

#include <SPI.h>
#include <mcp_can.h>

const uint8_t CS_PIN = 10, INT_PIN = 2;
MCP_CAN can(CS_PIN);
uint16_t rateK = 125;

// ---- tekrar slotlari ----
struct Slot { bool on; uint32_t id; uint8_t len, d[8]; uint16_t period; uint32_t last; };
const uint8_t NSLOTS = 8;
Slot slots[NSLOTS];

// ---- gorulen ID tablosu ----
const uint8_t MAX_IDS = 28;   // Nano RAM icin
struct Seen { uint32_t id; uint32_t count; uint8_t len, d[8]; bool noisy; };
Seen seen[MAX_IDS];
uint8_t seenN = 0;
uint32_t seenSince = 0, overflowIds = 0;

// ---- otomatik tarama ----
bool scanning = false;
uint32_t scanId, scanEnd, scanStepV = 1, prevScanId = 0xFFFFFFFF, scanCount = 0;
uint16_t scanPer;
uint8_t idDigits(uint32_t id) { return id > 0x7FF ? 8 : 3; }
uint8_t scanLen, scanD[8];
uint32_t scanLast = 0, baselineUntil = 0, scanHits = 0;
bool inBaseline() { return scanning && millis() < baselineUntil; }

// ---- bit taramasi ----
bool bitting = false;
uint32_t bitId;
uint16_t bitStep;
uint8_t bitBase[8], bitCur[8], bitLen = 8;
bool byteMode = false;      // true: tek bir baytin 00..FF tum degerlerini dener
uint8_t byteSel = 0;
uint16_t bitPer;
uint32_t bitStepStart = 0, bitLastTx = 0;
uint16_t totalSteps() { return byteMode ? 256 : bitLen * 8; }

// ---- isaretler (tarama sirasinda 'm not' ile o anki adimi kaydet) ----
const uint8_t MAX_MARKS = 16;
struct Mark { uint32_t step; char note[11]; };   // bit/bayt taramasinda adim, ID taramasinda ID
bool marksAreIds = false;
Mark marks[MAX_MARKS];
uint8_t markN = 0;

bool monitor = false;
uint32_t txOk = 0, txFail = 0;

uint8_t rateCode(uint16_t k) {
  switch (k) {
    case 500: return CAN_500KBPS; case 250: return CAN_250KBPS;
    case 100: return CAN_100KBPS; case 50: return CAN_50KBPS;
    default:  return CAN_125KBPS;
  }
}

void startBus() {
  while (can.begin(MCP_ANY, rateCode(rateK), MCP_8MHZ) != CAN_OK) {
    Serial.println(F("MCP2515 baslatilamadi, tekrar..."));
    delay(500);
  }
  can.setMode(MCP_NORMAL);
  can.disOneShotTX();
  Serial.print(F("NORMAL mod, ")); Serial.print(rateK); Serial.println(F(" kbit/s"));
}

void printHex(uint32_t v, uint8_t digits) {
  for (int8_t i = digits - 1; i >= 0; i--) Serial.print((v >> (i * 4)) & 0xF, HEX);
}

void printFrame(uint32_t id, bool ext, uint8_t len, const uint8_t *d) {
  printHex(id, ext ? 8 : 3);
  Serial.print(F(" [")); Serial.print(len); Serial.print(F("] "));
  for (uint8_t i = 0; i < len; i++) { printHex(d[i], 2); Serial.print(' '); }
}

bool sendFrame(uint32_t id, uint8_t len, uint8_t *d) {
  bool ext = id > 0x7FF;
  if (can.sendMsgBuf(id, ext ? 1 : 0, len, d) == CAN_OK) { txOk++; return true; }
  txFail++;
  can.abortTX();
  return false;
}

void reportHit(const __FlashStringHelper *what, uint32_t id, uint8_t len, const uint8_t *d, const uint8_t *old) {
  scanHits++;
  Serial.print(F(">>> ")); Serial.print(what); Serial.print(F(" (gonderilen: "));
  printHex(scanId, idDigits(scanId));
  if (prevScanId != 0xFFFFFFFF) { Serial.print(F(", onceki: ")); printHex(prevScanId, idDigits(prevScanId)); }
  Serial.print(F(") ")); printFrame(id & 0x1FFFFFFF, id & 0x80000000, len, d);
  if (old) { Serial.print(F(" <- eskisi: ")); for (uint8_t i = 0; i < len; i++) { printHex(old[i], 2); Serial.print(' '); } }
  Serial.println();
}

void rememberRx(uint32_t id, uint8_t len, const uint8_t *d) {
  for (uint8_t i = 0; i < seenN; i++) {
    if (seen[i].id == id) {
      bool changed = seen[i].len != len || memcmp(seen[i].d, d, len);
      if (changed) {
        if (inBaseline()) seen[i].noisy = true;              // kendi kendine degisen mesaj
        else if (scanning && !seen[i].noisy) reportHit(F("DEGISTI"), id, len, d, seen[i].d);
      }
      seen[i].count++; seen[i].len = len; memcpy(seen[i].d, d, len);
      return;
    }
  }
  if (scanning && !inBaseline()) reportHit(F("YENI ID"), id, len, d, NULL);
  if (seenN < MAX_IDS) {
    seen[seenN].id = id; seen[seenN].count = 1; seen[seenN].len = len;
    seen[seenN].noisy = inBaseline();
    memcpy(seen[seenN].d, d, len); seenN++;
  } else overflowIds++;
}

bool isClusterId(uint32_t id) {
  for (uint8_t i = 0; i < seenN; i++) if ((seen[i].id & 0x1FFFFFFF) == id) return true;
  return false;
}

void bitPrintStep() {
  if (byteMode) {
    memcpy(bitCur, bitBase, 8);
    bitCur[byteSel] = bitStep;
    Serial.print(F("DEGER ")); Serial.print(bitStep + 1); Serial.print(F("/256  B"));
    Serial.print(byteSel); Serial.print(F(" = 0x")); printHex(bitStep, 2); Serial.print(F("  "));
    printFrame(bitId, bitId > 0x7FF, bitLen, bitCur); Serial.println();
    return;
  }
  uint8_t byteIdx = bitStep / 8, bitIdx = 7 - (bitStep % 8);
  memcpy(bitCur, bitBase, 8);
  bitCur[byteIdx] ^= (1 << bitIdx);               // tek biti cevir (taban 00 ise yakar)
  Serial.print(F("ADIM ")); Serial.print(bitStep + 1); Serial.print(F("/")); Serial.print(bitLen * 8); Serial.print(F("  B"));
  Serial.print(byteIdx); Serial.print(F(" bit")); Serial.print(bitIdx);
  Serial.print(F(" (0x")); printHex(1 << bitIdx, 2); Serial.print(F(")  "));
  printFrame(bitId, bitId > 0x7FF, bitLen, bitCur); Serial.println();
}

void printMarkLine(uint32_t step, const char *note) {
  if (marksAreIds) {
    Serial.print(F("  ID ")); printHex(step, idDigits(step));
    if (note[0]) { Serial.print(F("  -> ")); Serial.print(note); }
    Serial.println();
    return;
  }
  if (byteMode) {
    Serial.print(F("  B")); Serial.print(byteSel); Serial.print(F(" = 0x")); printHex(step, 2);
    if (note[0]) { Serial.print(F("  -> ")); Serial.print(note); }
    Serial.println();
    return;
  }
  uint8_t byteIdx = step / 8, bitIdx = 7 - (step % 8);
  Serial.print(F("  ADIM ")); Serial.print(step + 1);
  Serial.print(F("  B")); Serial.print(byteIdx); Serial.print(F(" bit")); Serial.print(bitIdx);
  Serial.print(F(" (0x")); printHex(1 << bitIdx, 2); Serial.print(F(")"));
  if (note[0]) { Serial.print(F("  -> ")); Serial.print(note); }
  Serial.println();
}

void printMarks() {
  if (!markN) { Serial.println(F("Isaret yok.")); return; }
  if (marksAreIds) {
    Serial.println(F("=== TARAMA ISARETLERI (o an gonderilen ID) ==="));
    for (uint8_t i = 0; i < markN; i++) printMarkLine(marks[i].step, marks[i].note);
    Serial.println(F("(Tepki gecikebilir: isaretli ID'nin 1-3 oncesini de dene)"));
    return;
  }
  Serial.print(F("=== ISARETLER (")); printHex(bitId, idDigits(bitId)); Serial.print(F(", taban "));
  for (uint8_t i = 0; i < bitLen; i++) { printHex(bitBase[i], 2); Serial.print(' '); }
  Serial.println(F(") ==="));
  for (uint8_t i = 0; i < markN; i++) printMarkLine(marks[i].step, marks[i].note);
  Serial.println(F("(Tepki bazen 1 adim gec gelir: bir onceki adimi da kontrol et)"));
}

void bitStepRun() {
  if (!bitting) return;
  if (bitPer > 0 && millis() - bitStepStart >= bitPer) {   // bitPer 0 = elle ilerletme (n / p)
    bitStepStart = millis();
    if (++bitStep >= totalSteps()) {
      bitting = false;
      Serial.println(F("BIT TARAMASI BITTI"));
      printMarks();
      return;
    }
    bitPrintStep();
  }
  if (millis() - bitLastTx >= 100) {             // adim boyunca 100 ms'de bir gonder
    bitLastTx = millis();
    sendFrame(bitId, bitLen, bitCur);
  }
}

// Her ID kendi suresi (scanPer) boyunca 50 ms'de bir gonderilir; gostergeler
// "kontak acik" gibi mesajlari tek sefer degil, surekli gormek ister.
bool scanCurOk = false, scanStarted = false;
uint32_t scanTx = 0;

void scanStep() {
  if (!scanning || inBaseline()) return;
  if (!scanStarted || millis() - scanLast >= scanPer) {
    if (scanStarted) {
      if (scanCurOk) prevScanId = scanId;
      scanId += scanStepV;
    }
    scanStarted = true;
    scanLast = millis();
    if (scanId > scanEnd) {
      scanning = false; scanStarted = false;
      Serial.print(F("TARAMA BITTI. Yakalanan olay: ")); Serial.println(scanHits);
      printMarks();
      return;
    }
    scanCurOk = !isClusterId(scanId);         // gostergenin kendi ID'lerine karisma
    for (uint8_t i = 0; i < NSLOTS; i++) if (slots[i].on && slots[i].id == scanId) scanCurOk = false;  // rep'teki ID'yi ezme
    if ((scanCount++ & 0x3F) == 0) { Serial.print(F("... ")); printHex(scanId, idDigits(scanId)); Serial.println(); }
    scanTx = 0;
  }
  if (scanCurOk && (scanTx == 0 || millis() - scanTx >= 50)) {
    scanTx = millis();
    sendFrame(scanId, scanLen, scanD);
  }
}

void printIds() {
  float secs = (millis() - seenSince) / 1000.0;
  Serial.print(F("--- ")); Serial.print(seenN); Serial.print(F(" ID, "));
  Serial.print(secs, 1); Serial.println(F(" sn ---"));
  for (uint8_t i = 0; i < seenN; i++) {
    printFrame(seen[i].id & 0x1FFFFFFF, seen[i].id & 0x80000000, seen[i].len, seen[i].d);
    Serial.print(F(" | adet=")); Serial.print(seen[i].count);
    if (secs > 0.5) { Serial.print(F(" ~")); Serial.print(seen[i].count / secs, 1); Serial.print(F(" Hz")); }
    Serial.println();
  }
  if (overflowIds) { Serial.print(F("Tabloya sigmayan mesaj: ")); Serial.println(overflowIds); }
}

void printStat() {
  Serial.print(rateK); Serial.print(F("k | TX ok=")); Serial.print(txOk);
  Serial.print(F(" fail=")); Serial.print(txFail);
  Serial.print(F(" | TEC=")); Serial.print(can.errorCountTX());
  Serial.print(F(" REC=")); Serial.print(can.errorCountRX());
  Serial.print(F(" EFLG=0x")); Serial.println(can.getError(), HEX);
  for (uint8_t i = 0; i < NSLOTS; i++) if (slots[i].on) {
    Serial.print(F("  rep ")); printFrame(slots[i].id, slots[i].id > 0x7FF, slots[i].len, slots[i].d);
    Serial.print(F("her ")); Serial.print(slots[i].period); Serial.println(F(" ms"));
  }
}

// ---- komut satiri ----
char line[72]; uint8_t lineLen = 0;

bool parseData(char *&p, uint8_t *d, uint8_t &len) {
  len = 0;
  char *tok;
  while ((tok = strtok(NULL, " ")) && len < 8) d[len++] = strtoul(tok, NULL, 16);
  return true;
}

void runCommand() {
  char *cmd = strtok(line, " ");
  if (!cmd) return;
  for (char *c = cmd; *c; c++) *c = tolower(*c);
  char *dummy = NULL;

  if (!strcmp(cmd, "send")) {
    char *t = strtok(NULL, " "); if (!t) { Serial.println(F("ornek: send 3A0 11 22")); return; }
    uint32_t id = strtoul(t, NULL, 16); uint8_t d[8], len; parseData(dummy, d, len);
    Serial.print(sendFrame(id, len, d) ? F("GONDERILDI ") : F("GONDERILEMEDI "));
    printFrame(id, id > 0x7FF, len, d); Serial.println();

  } else if (!strcmp(cmd, "rep")) {
    char *t = strtok(NULL, " "); char *p = strtok(NULL, " ");
    if (!t || !p) { Serial.println(F("ornek: rep 3A0 100 11 22")); return; }
    uint32_t id = strtoul(t, NULL, 16); uint16_t per = atoi(p);
    if (per < 10) per = 10;
    int8_t s = -1;
    for (uint8_t i = 0; i < NSLOTS; i++) if (slots[i].on && slots[i].id == id) s = i;
    if (s < 0) for (uint8_t i = 0; i < NSLOTS; i++) if (!slots[i].on) { s = i; break; }
    if (s < 0) { Serial.println(F("8 slot dolu, once unrep/stop")); return; }
    slots[s].id = id; slots[s].period = per; parseData(dummy, slots[s].d, slots[s].len);
    slots[s].last = 0; slots[s].on = true;
    Serial.print(F("TEKRAR ")); printFrame(id, id > 0x7FF, slots[s].len, slots[s].d);
    Serial.print(F("her ")); Serial.print(per); Serial.println(F(" ms"));

  } else if (!strcmp(cmd, "byte")) {
    char *t = strtok(NULL, " "), *i = strtok(NULL, " "), *v = strtok(NULL, " ");
    if (!t || !i || !v) { Serial.println(F("ornek: byte 3A0 2 FF")); return; }
    uint32_t id = strtoul(t, NULL, 16); uint8_t idx = atoi(i);
    for (uint8_t k = 0; k < NSLOTS; k++) if (slots[k].on && slots[k].id == id && idx < 8) {
      slots[k].d[idx] = strtoul(v, NULL, 16);
      if (idx >= slots[k].len) slots[k].len = idx + 1;
      Serial.print(F("GUNCEL ")); printFrame(id, id > 0x7FF, slots[k].len, slots[k].d); Serial.println();
      return;
    }
    Serial.println(F("Bu ID tekrar etmiyor, once rep"));

  } else if (!strcmp(cmd, "unrep")) {
    char *t = strtok(NULL, " "); if (!t) return;
    uint32_t id = strtoul(t, NULL, 16);
    for (uint8_t i = 0; i < NSLOTS; i++) if (slots[i].id == id) slots[i].on = false;
    Serial.println(F("Durduruldu"));

  } else if (!strcmp(cmd, "stop")) {
    for (uint8_t i = 0; i < NSLOTS; i++) slots[i].on = false;
    Serial.println(F("Tum tekrarlar durdu"));

  } else if (!strcmp(cmd, "scan") || !strcmp(cmd, "qscan") || !strcmp(cmd, "xscan")) {
    bool ext = cmd[0] == 'x';
    bool quick = cmd[0] == 'q' || ext;
    char *a = strtok(NULL, " "), *b = strtok(NULL, " ");
    char *s = ext ? strtok(NULL, " ") : NULL;
    char *p = strtok(NULL, " ");
    if (!a || !b || !p || (ext && !s)) {
      Serial.println(ext ? F("ornek: xscan 1E340000 1E3400FF 1 1000 00 1C") :
                           F("ornek: scan 000 7FF 200 FF FF FF FF FF FF FF FF"));
      return;
    }
    scanId = strtoul(a, NULL, 16); scanEnd = strtoul(b, NULL, 16); scanPer = atoi(p);
    scanStepV = ext ? strtoul(s, NULL, 16) : 1;
    if (scanStepV == 0) scanStepV = 1;
    scanCount = 0;
    if (ext) { if (scanEnd > 0x1FFFFFFF) scanEnd = 0x1FFFFFFF; }
    else if (scanEnd > 0x7FF) scanEnd = 0x7FF;
    if (scanPer < 20) scanPer = 20;
    parseData(dummy, scanD, scanLen);
    if (scanLen == 0) { scanLen = 8; memset(scanD, 0xFF, 8); }
    // rep'ler tarama sirasinda da calismaya devam eder (arka planda uyandirma adaylari icin)
    if (!quick) { seenN = 0; overflowIds = 0; seenSince = millis(); }   // qscan: ogrenilen ID'leri koru
    scanHits = 0; prevScanId = 0xFFFFFFFF; markN = 0; marksAreIds = true;
    scanning = true; baselineUntil = quick ? millis() : millis() + 5000;
    scanLast = 0; scanStarted = false;
    Serial.print(F("TARAMA: ")); printHex(scanId, idDigits(scanId)); Serial.print(F("-")); printHex(scanEnd, idDigits(scanEnd));
    if (ext) { Serial.print(F(" adim ")); Serial.print(scanStepV, HEX); }
    Serial.print(F(" her ")); Serial.print(scanPer); Serial.print(F(" ms, veri: "));
    for (uint8_t i = 0; i < scanLen; i++) { printHex(scanD[i], 2); Serial.print(' '); }
    Serial.println(quick ? F("\nHemen basliyor.") :
                           F("\n5 sn sadece dinliyorum (kendi kendine degisenleri ayikliyorum)..."));

  } else if (!strcmp(cmd, "bits")) {
    char *t = strtok(NULL, " "), *p = strtok(NULL, " ");
    if (!t || !p) { Serial.println(F("ornek: bits 380 1000   (taban veri vermezsen 8x 00)")); return; }
    bitId = strtoul(t, NULL, 16); bitPer = atoi(p); if (bitPer != 0 && bitPer < 100) bitPer = 100;
    uint8_t len; memset(bitBase, 0, 8); parseData(dummy, bitBase, len); bitLen = len ? len : 8;
    byteMode = false;
    scanning = false;
    for (uint8_t i = 0; i < NSLOTS; i++) if (slots[i].on && slots[i].id == bitId) slots[i].on = false;
    bitStep = 0; bitStepStart = millis(); bitLastTx = 0; bitting = true; markN = 0;
    Serial.print(F("BIT TARAMASI: ")); printHex(bitId, idDigits(bitId));
    if (bitPer == 0) Serial.println(F(", ELLE: n = sonraki bit, p = onceki bit, scanstop = bitir"));
    else { Serial.print(F(", adim ")); Serial.print(bitPer); Serial.println(F(" ms. Tepki gorunce 'scanstop' yaz.")); }
    bitPrintStep();

  } else if (!strcmp(cmd, "bytes")) {
    char *t = strtok(NULL, " "), *b = strtok(NULL, " "), *p = strtok(NULL, " ");
    if (!t || !b || !p) { Serial.println(F("ornek: bytes 356 0 1000   (ID, bayt no 0-7, sure; taban istege bagli)")); return; }
    bitId = strtoul(t, NULL, 16); byteSel = atoi(b); bitPer = atoi(p);
    if (bitPer != 0 && bitPer < 100) bitPer = 100;
    uint8_t len; memset(bitBase, 0, 8); parseData(dummy, bitBase, len); bitLen = len ? len : 8;
    if (byteSel >= bitLen) { Serial.println(F("Bayt no veri uzunlugundan buyuk")); return; }
    byteMode = true; scanning = false;
    for (uint8_t i = 0; i < NSLOTS; i++) if (slots[i].on && slots[i].id == bitId) slots[i].on = false;
    bitStep = 0; bitStepStart = millis(); bitLastTx = 0; bitting = true; markN = 0;
    Serial.print(F("BAYT TARAMASI: ")); printHex(bitId, idDigits(bitId));
    Serial.print(F(" B")); Serial.print(byteSel);
    if (bitPer == 0) Serial.println(F(", ELLE: n / p / m / scanstop"));
    else { Serial.print(F(", adim ")); Serial.print(bitPer); Serial.println(F(" ms. Tepki gorunce 'm not' yaz.")); }
    bitPrintStep();

  } else if (!strcmp(cmd, "m")) {
    if (!bitting && !scanning) { Serial.println(F("Calisan tarama yok.")); return; }
    if (markN >= MAX_MARKS) { Serial.println(F("Isaret dolu (16).")); return; }
    char *note = strtok(NULL, "");             // satirin geri kalani not
    marksAreIds = scanning && !bitting;
    marks[markN].step = marksAreIds ? scanId : bitStep;
    marks[markN].note[0] = 0;
    if (note) { strncpy(marks[markN].note, note, 10); marks[markN].note[10] = 0; }
    Serial.print(F("ISARETLENDI #")); Serial.print(markN + 1); Serial.print(':');
    printMarkLine(marks[markN].step, marks[markN].note);
    markN++;

  } else if (!strcmp(cmd, "marks")) {
    printMarks();

  } else if (bitting && (!strcmp(cmd, "n") || !strcmp(cmd, "p"))) {
    if (cmd[0] == 'n') {
      if (bitStep + 1 >= totalSteps()) { Serial.println(F("Sondayiz. scanstop ile bitir.")); return; }
      bitStep++;
    } else {
      if (bitStep == 0) { Serial.println(F("Ilk bitteyiz.")); return; }
      bitStep--;
    }
    bitStepStart = millis();
    bitPrintStep();

  } else if (!strcmp(cmd, "scanstop")) {
    if (bitting) {
      bitting = false;
      Serial.print(F("Bit taramasi durdu, son adim: ")); Serial.print(bitStep + 1);
      Serial.print(F("  ")); printFrame(bitId, bitId > 0x7FF, bitLen, bitCur); Serial.println();
      printMarks();
      return;
    }
    scanning = false;
    scanStarted = false;
    Serial.print(F("Tarama durdu, su an gonderilen: ")); printHex(scanId, idDigits(scanId));
    if (prevScanId != 0xFFFFFFFF) { Serial.print(F(", onceki: ")); printHex(prevScanId, idDigits(prevScanId)); }
    Serial.println();
    printMarks();

  } else if (!strcmp(cmd, "ids")) printIds();
  else if (!strcmp(cmd, "clear")) { seenN = 0; overflowIds = 0; seenSince = millis(); Serial.println(F("ID tablosu sifirlandi")); }
  else if (!strcmp(cmd, "mon")) { monitor = !monitor; Serial.println(monitor ? F("Canli izleme ACIK") : F("Canli izleme KAPALI")); }
  else if (!strcmp(cmd, "rate")) {
    char *t = strtok(NULL, " "); if (!t) return;
    rateK = atoi(t); if (rateK != 500 && rateK != 250 && rateK != 100 && rateK != 50) rateK = 125;
    startBus(); seenN = 0; seenSince = millis();
  }
  else if (!strcmp(cmd, "stat")) printStat();
  else Serial.println(F("Komutlar: send rep byte unrep stop ids clear mon rate stat scan qscan xscan bits bytes m marks scanstop"));
}

void setup() {
  Serial.begin(115200);
  pinMode(INT_PIN, INPUT);
  startBus();
  seenSince = millis();
  Serial.println(F("Komutlar: send rep byte unrep stop ids clear mon rate stat scan qscan xscan bits bytes m marks scanstop"));
}

void loop() {
  // seri komut
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen) { line[lineLen] = 0; runCommand(); lineLen = 0; }
    } else if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
  }

  // gelenleri oku
  while (!digitalRead(INT_PIN)) {
    unsigned long id; uint8_t len, d[8];
    if (can.readMsgBuf(&id, &len, d) != CAN_OK) break;
    if (len > 8) len = 8;
    rememberRx(id, len, d);
    if (monitor) {
      Serial.print(F("RX ")); printFrame(id & 0x1FFFFFFF, id & 0x80000000, len, d); Serial.println();
    }
  }

  // tekrarlar
  for (uint8_t i = 0; i < NSLOTS; i++) {
    if (slots[i].on && millis() - slots[i].last >= slots[i].period) {
      slots[i].last = millis();
      sendFrame(slots[i].id, slots[i].len, slots[i].d);
    }
  }

  scanStep();
  bitStepRun();

  // bus-off olursa toparla
  if (can.getError() & 0x20) { Serial.println(F("BUS-OFF, yeniden baslatiliyor")); startBus(); }
}
