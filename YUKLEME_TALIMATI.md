# v0.4.0 Yükleme Talimatı

Bu dosya, karta yükleme yapılacağı zaman izlenecek adımlar. Kendime not olarak
yazıldı — yeni bir oturumda bunu okuyup kaldığım yerden devam edebileyim diye.

**Durum: kod hazır, üç ortam da derleniyor, HİÇBİRİ KARTA YÜKLENMEDİ.**

Son denemede (31 Ağustos 2026) kart bilgisayara bağlı değildi:
`pio device list` boş, Windows'ta `Ports` sınıfında hiç cihaz yok, USB'de CH340
görünmüyor. Sürücü sorunu değil — cihaz takılı değildi.

---

## 0. Önce kontrol: kart bağlı mı

```bash
~/.platformio/penv/Scripts/pio.exe device list
```

Boş çıkarsa Windows tarafından bak:

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
Get-PnpDevice -PresentOnly | Where-Object { $_.Class -eq 'Ports' } | Format-Table Status, FriendlyName
```

| Görünen | Anlamı | Ne yapmalı |
|---|---|---|
| `COM3` (ya da başka COM) | Hazır | Adım 1'e geç |
| Hiçbir şey | Kart takılı değil ya da kablo şarj kablosu | Kabloyu değiştir, 2-USB modelde **mikro USB** olan porta tak |
| Sarı ünlem / "USB2.0-Serial" | CH340 sürücüsü yok | https://www.wch.cn/downloads/CH341SER_EXE.html |

> Kart CH340 olarak enumerate oluyor ve daha önce **COM3**'teydi. Port numarası
> değişebilir; komutlarda sabitlemiyorum, PlatformIO kendisi buluyor.

---

## 1. Animasyon kurucusunu yükle

Bu adım `firmware_assets/anim.anm` paketini SD karta `/anim/dolphin.anm` olarak
yazıyor. Kart okuyucu olmadığı için tek yol bu.

```bash
cd "/c/Users/serve/OneDrive/Masaüstü/carthinhg/CarDisplay-OS/v0.3.15-OFFLINE-VISUALIZER-SMOOTH/CarDisplay-OS-v0.3.15-OFFLINE-VISUALIZER-SMOOTH"

PLATFORMIO_BUILD_DIR="C:/pio-build/CarDisplay-OS" \
  ~/.platformio/penv/Scripts/pio.exe run -t upload -e cyd2usb_anim_installer
```

**Beklenen seri çıktı** (`pio device monitor -b 115200`):

```
Animation installed: /anim/dolphin.anm
```

**Beklenen ekran:** `ANIM READY` + altında `/anim/dolphin.anm` +
`Now upload: cyd2usb`

### Bu adımda çıkabilecek hatalar

| Ekranda | Sebep | Çözüm |
|---|---|---|
| `SD ERROR` | Kart yok ya da FAT32 değil | Kartı tak / FAT32 formatla |
| `NO PACK` | `firmware_assets/anim.anm` yok | `python tools/make_dolphin_pack.py` |
| `BAD PACK` | Gömülü dosya ANM1 değil | Paketi yeniden üret |
| `WRITE ERROR` | Kart dolu ya da bozuk | Kartta yer aç |

> SD kart takılı değilse bu adımı **atlayabilirsin**. PIONEER modu ekranda
> "NO ANIMATION" yazar, diğer 40 mod normal çalışır.

---

## 2. Normal firmware'i yükle

```bash
PLATFORMIO_BUILD_DIR="C:/pio-build/CarDisplay-OS" \
  ~/.platformio/penv/Scripts/pio.exe run -t upload -e cyd2usb
```

**Beklenen seri çıktı:**

```
Car Display OS v0.4.0
Car Display OS v0.4.0 - display setup
Animation pack: /anim/dolphin.anm          ← adım 1 yapıldıysa
INMP441 ready: BCLK=22 WS=27 SD=35
```

`INMP441` satırı yoksa mikrofon bağlı değil demektir — görselleştiriciler
`MIC ERROR` gösterir, PIONEER modu yine de çalışır.

---

## 3. Seri monitör

```bash
~/.platformio/penv/Scripts/pio.exe device monitor -b 115200
```

Yükleme sırasında izlenecek anahtar satırlar:

```
Waiting for saved Wi-Fi...                         ← 15 sn geri sayım başladı
Wait skipped by touch; opening setup               ← ekrana dokunuldu
No saved network after the retry window; opening setup
Portal open; re-trying the saved network           ← 20 sn'de bir
Offline visualizer open; shutting the radio down   ← TELSİZ KAPANDI
Offline visualizer closed; radio back on
```

---

## Derleme notları

**OneDrive kilidi:** `.pio` klasörü OneDrive içinde, sync client ara sıra
kilitliyor ve link adımı `cannot open map file` ile patlıyor. Bu yüzden her
komutta `PLATFORMIO_BUILD_DIR` dışarı gösteriliyor.

> README'de yazan "`platformio.ini` içindeki `build_dir` satırını yorumdan
> çıkar" çözümü **çalışmıyor** — ikinci bir `[platformio]` bölümü açıyor.
> PlatformIO'nun ayrıştırıcısıyla test edildi:
> `section 'platformio' already exists`. Düzeltilmedi.

**PlatformIO PATH'te değil:** `~/.platformio/penv/Scripts/pio.exe`

---

## Yükleme sonrası — ilk 5 dakikada bakılacaklar

Tam liste `V0.4.0_OTURUM_NOTLARI.md` bölüm 12 ve 17'de. En kritik dördü:

1. **Hotspot kapalıyken aç** → 15 sn sonra kurulum ekranı kendiliğinden geliyor mu?
   (Eskiden sonsuza kadar arıyordu, asıl düzeltilen bug bu.)
2. **OFFLINE MODE'a bas** → telefondan bakınca `SpotifyDIY` ağı **kayboluyor mu?**
   (Telsizin gerçekten kapandığının tek gerçek testi.)
3. **PIONEER modundan çık** → diğer modlar canlanıyor mu?
   (Mikrofon geri gelmezse hepsi düz çizgi çizer — en riskli regresyon.)
4. **DOLPHIN modu** → yunus yunusa benziyor mu, ters mi yüzüyor?
   (Sprite'ı elle çizdim, panelde hiç görmedim.)

---

## Geri dönüş

v0.4 arabada sorun çıkarırsa:

```bash
git checkout v0.3.17
PLATFORMIO_BUILD_DIR="C:/pio-build/CarDisplay-OS" \
  ~/.platformio/penv/Scripts/pio.exe run -t upload -e cyd2usb
git checkout main
```

v0.3.17 çalışıyordu ama **çıkar-tak sorunu onda var** — offline moda ulaşmak
için yine çift güç döngüsü gerekir.

NVS'ye yazılan `vizmode` anahtarı v0.3.17'de okunmuyor, zararsız kalır.
SD'deki `/anim/` klasörü de öylece durur.

---

## Depo durumu

```
46e4305  Install animation packs from the firmware...    ← v0.4.0, HEAD
d67a0ef  Correct the mode count in the notes summary table
fa89089  Update the session notes for the animation mode...
839fcda  Play frame packs off the SD card, and give the overlay a brightness sun
01229bc  Add the v0.4 session notes, including the post-flash checklist
5ec9c55  Put the signal bars back on the connecting screen
17c67d6  Quieten the connecting screen down to one moving element
edf87dc  Keep the player palette out of the visualizer retune...
5acb2d1  Make offline mode actually offline, and redraw the screen that offers it
951d699  Make the offline fallback honest about its timing, and skippable by touch
a0729ef  Forty visualizers, one green-cyan palette, and a boot that does not dead-end
5a65f80  (v0.3.17) Visualizers use the player green; add four modes
```

Çalışma ağacı temiz. Push edilmedi (`origin/main` hâlâ `5a65f80`).

**Derleme boyutları:**

```
cyd2usb                 RAM 30.8%  Flash 43.4%
cyd2usb_anim_installer  RAM 28.2%  Flash 35.3%   (131 KB paket gömülü)
cyd2usb_font_installer  SUCCESS
```

---

## Bekleyen iş (yükleme sonrasına)

- Gerçek Pioneer kareleri. Kaynak yok, sadece video var:
  `https://www.youtube.com/watch?v=Ei7zqMWj3Aw` (DEH-P6400R)
  İş akışı: `yt-dlp` → `ffmpeg` ile kırp+kare çıkar → `make_animation.py` →
  `cyd2usb_anim_installer` ile yükle. `crop` değerlerini videoyu görünce ayarla.
- `platformio.ini` `build_dir` çözümü bozuk (yukarıda)
- SD kapak önbelleğinde temizlik yok, sınırsız büyüyor
- Kapak dosya adı 32-bit FNV hash, başlıkta URL doğrulaması yok → çakışmada
  yanlış kapak (olasılık düşük)
- `serialPrint.h` ölü kod
- `carthinhg` ağacında ~1.9 GB `.pio` çöpü, OneDrive'a senkronlanıyor
- Native testler koşturulamıyor (`g++` PATH'te yok)
