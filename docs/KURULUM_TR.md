# Kurulum

Bu projeyi 2023 Ducato göstergemi (parça no **00468637450**) masada bilgisayardan sürmek için hazırladım. Sadece araçtan sökülmüş göstergeyle kullanın; buradaki kodlar hatta mesaj gönderiyor, araca bağlamayın.

## 1. Donanım

X250 projesiyle aynı: Arduino Nano ATmega328P + MCP2515 (**8 MHz kristal**) + TJA1050.

| Nano | MCP2515 + TJA1050 |
|---|---|
| D10 | CS |
| D2 | INT |
| D11 | MOSI / SI |
| D12 | MISO / SO |
| D13 | SCK |
| 5 V | VCC |
| GND | GND |

Nano, modül ve gösterge ortak GND kullanmalı. 12 V'u Nano'ya ya da CAN uçlarına vermeyin.

## 2. Gösterge bağlantısı

Kendi göstergemde (00468637450) çıkardığım pin düzeni. Pinleri konnektörün üzerindeki numaradan okuyun.

| Pin | Görev |
|---:|---|
| 1 | GND (eksi) |
| 2 | +12 V (X250'deki gibi sabit +30) |
| 3 | +12 V kontak |
| 4 | Boş |
| 5 | CAN-L, **125 kbit/s** gövde hattı |
| 6 | CAN-H, **125 kbit/s** gövde hattı |
| 7, 8 | Bilinmiyor |
| 9, 10 | Boş |
| 11, 12 | CAN-L, **500 kbit/s** |
| 13, 14 | CAN-H, **500 kbit/s** |
| 15 | 5 V çıkış (göstergeden çıkar, buraya besleme vermeyin) |
| 16 | GND |
| 17, 18 | Boş |

500 kbit/s hattı için iki çift var ve **aynı hat**: gösterge kapalıyken 11-12 ve 13-14 arası ~0 Ω ölçülüyor (içeride birbirine bağlı). **Ben modülü 12 (CAN-L) ve 13 (CAN-H) pinlerine bağladım**; testlerin hepsi bu bağlantıyla yapıldı. 125 kbit/s hattında kapı, kaput, immo (356) var.

## 3. Yazılım

1. Arduino IDE'de **Arduino AVR Boards** ve **mcp_can by Cory J. Fowler, 1.5.1** kurun.
2. `firmware/Gosterge_Bench/Gosterge_Bench.ino` dosyasını açın. Nano / ATmega328P seçip yükleyin; yüklenmezse **Old Bootloader** deneyin.
3. Seri Monitör: **115200**, **Yeni satır**.

## 4. Göstergeyi çalıştırma

Modülü 500 kbit/s hattına bağlayın ve şunları yapıştırın:

```
rate 500
rep 0FC 50 20 40 00 00 00 00 FF 00
rep 101 100 00 C8 00 00 00 00 00 00
rep 545 100 00 00 08 00 00 00 00 00
rep 0DE 100 FF FF FF FF FF FF FF FF
rep 4AF 100 00 80 00 00 00 00 00 00
rep 257 100 20 00 00 00 00 00 00 00
rep 5AE 100 00 00 00 00 00 00 00 00
rep 226 100 00 00 00 00 00 00 00 00
```

Gösterge 2000 devir, ~100 km/s, 6/9 yakıt ve uyarısız görünür.

Değerleri canlı değiştirmek:

| Ne | Komut |
|---|---|
| Devir 3000 | `byte 0FC 0 30` (devir ≈ değer × 64) |
| Hız 50 km/s | `byte 101 1 64` (değer = km/s × 2) |
| Yakıt full | `byte 545 2 0C` |
| Gece modu (kısık) | `byte 545 4 FF` |

**Seri monitör her açıldığında Nano resetlenir ve 125k ile başlar; önce `rate 500` yazın.**

## 5. ETS2 / BeamNG ile oynama

`firmware/Ducato2023_Game/Ducato2023_Game.ino`, X250 projesindeki köprülerin gönderdiği `game ...` paketlerini aynen anlıyor; köprüler değişmedi (`bridge/`, `ETS2.cmd`, `BeamNG.cmd`, `beamng_mod/` bu repoda da var).

1. `Ducato2023_Game.ino`'yu Nano'ya yükleyin, modülü **500 kbit/s** hattına bağlayın, Seri Monitör'ü **kapatın**.
2. PowerShell'de repo klasöründe: `py -m pip install -r requirements.txt`
3. ETS2 için Funbit Telemetry Server'ı, BeamNG için modu X250 projesindeki gibi kurun ([X250 README](https://github.com/tnsozcan/ducato-x250-can-dashboard)).
4. `ETS2.cmd COM5` ya da `BeamNG.cmd COM5` (COM numarası sizinkine göre).

Oyundan gelenler: devir, hız, yakıt, aydınlatma (park lambası / kısa far açıkken kısık), 110 °C üstünde kırmızı hararet lambası. BeamNG'de ayrıca çekiş kontrol yanıp sönmesi, kemer, airbag, motor arızası ve yakıtta su lambaları. Direksiyon, ABS/ESC, airbag ve motor beyni mesajları sürekli gönderilir, uyarı lambaları sönük kalır. Henüz yok: hararet çubuğu, el freni, sinyaller, far simgeleri, kruvaz.

Oyunda kontak açılınca X250'deki gibi ışık testi yapılır: ~5 sn ABS, fren ve çekiş kontrol lambaları (4AF kesilerek); motor sıcaklığına göre 2/3/5/7 sn kızdırma lambası; marştan 2 sn sonrasına kadar motor arıza lambası (motor çalışmıyorken 0FC b6 = 00, devir ibresi marşta uyanır). Kontak kapanınca ya da köprüden 1 sn paket gelmezse ibreler 1,5 sn içinde sıfıra iner, sonra gönderim durur ve gösterge uyur.

## 6. Bilinen eksikler ve sınırlar

- **Sadece ETS2 ile denendi.** BeamNG aynı köprü ve paketi kullanıyor ama bu göstergeyle henüz test edilmedi.
- **Hararet çubuğu** bulunamadı. El freni, sinyaller, farlar ve kruvaz da yok; büyük ihtimalle 125k gövde hattındalar ve tek modüllü bu düzenek o hatta göndermiyor (kapı, kaput, immo da orada). 4AF b0 10 el freni değil, EBD arızası.
- **Hız** X250 köprüsünün kalibrasyonundan geri çevriliyor; bazı hızlarda ibre birkaç km/s sapabilir.
- **Yakıt** 3 segmentin altına inmiyor (b2 = 00 ve 01 ikisi de 3 gösteriyor).
- **ABS/fren/çekiş ışık testi** 4AF kesilerek yapılıyor; gösterge lambaları kendi zaman aşımından sonra (~1 sn geç) yakıyor ve hepsi birlikte sönüyor. `LAMP_DELAY_MS` kızdırma ve motor lambasını bunlarla aynı ana denk getirmek için var, kendi göstergenize göre ayarlayın.
- Motor kapalıyken 0FC b6 = 00 gönderiliyor; devir ibresi motor çalışana kadar uyuyor (hız ibresi 101 ile çalışır, 0'da durur). Akü lambası bulunamadı.
- **0FC b6** için tam değer bilinmiyor; FF çalışıyor, tek bit yetmiyor.
- Gosterge_Bench her resetten sonra 125 kbit/s ile açılır (Seri Monitör'ü açmak Nano'yu resetler).
- Nano kodları sadece benim masamda derlenip çalıştırıldı.

Tüm mesajlar: [CAN_MESSAGES.md](CAN_MESSAGES.md)
