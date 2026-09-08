# Kaldığımız yer — 2 Eylül 2026

Oturum yarıda kesildi (PC kapatıldı). Bu dosya, yeni bir oturumda kaldığın
yerden devam edebilmen için yazıldı.

**Durum: her şey derleniyor, HİÇBİRİ KARTA YÜKLENMEDİ, HİÇBİRİ EKRANDA
GÖRÜLMEDİ. Hiçbir değişiklik commit'lenmedi.**

---

## 1. Bu oturumda ne oldu

Üç iş yapıldı:

1. **Orijinal Pioneer animasyonları bulundu ve karta hazırlandı.** (bitti)
2. **Elle çizilmiş yunus ve DOLPHIN modu söküldü.** (bitti, derleniyor)
3. **PIONEER moduna animasyon seçme menüsü eklendi.** (kod bitti, derleniyor,
   **ekranda hiç denenmedi**)

---

## 2. Önemli düzeltme — animasyonların kaynağı

Önceki oturum "internette çıkarılmış Pioneer animasyonu yok" diye karar verip
YouTube videosundan kare kazımıştı. **Bu yanlıştı.** Kullanıcı doğru kaynağı
buldu:

> https://github.com/youxufkhan/carozerra

Bu proje `.lkd` formatını çözmüş ve **83 orijinal klip** içeriyor. Videodan
kazınan sürüm çöpe atıldı, bir daha o yola sapma.

`.lkd` formatı: 20 baytlık başlık (`zLKD`, sürüm, 2 alan, kare sayısı), sonra
gzip → tar → tek bir 24-bit alttan-üste BMP. Kareler dikey üst üste dizili.
Pillow gerekmiyor, standart kütüphane yetiyor.

| sürüm | kare boyutu | kare | ton |
|---|---|---|---|
| 3 | 256x64 | 60 | 4 gri (0,114,143,178) |
| 4 | 128x33 | ~150 | 4 gri (0,112,173,241) |
| 5 | 192x48 | 150 | renkli |

**Yunus = `movie8_f.lkd`** (v3, en iyi çözünürlük). `alt_diverdolphins.lkd` de
yunus ama 128x33 (daha kaba, 149 kare).

`movie*` dosya adları içerik hakkında hiçbir şey söylemiyor — kareleri çözüp
bakmak gerekti. Tespit edilenler:

| dosya | içerik | pakete girdi mi |
|---|---|---|
| movie1 | kanyon / dağ manzarası | ✅ `canyon` |
| movie2 | şehir sokağı | ✅ `city` |
| movie3 | çiçekler | ✅ `flowers` |
| movie4 | spor araba | ✅ `roadster` |
| movie5 | carrozzeria logosu | ✅ `carrozzeria` |
| movie6 | mercan / su altı | ✅ `reef` |
| movie7 | "FIGHT!! MOVIE" robotlar | ✅ `mecha` |
| movie8_f | **yunuslar** | ✅ `dolphins` |
| movie9_f | türbin / makine | ❌ elendi |
| movie10_f | soyut çizgiler | ❌ elendi |

İkisi elendi çünkü `AnimationPlayer::MAX_PACKS = 8`; dokuzuncu karta yazılır
ama hiç oynatılmazdı.

### Dönüştürme

`tools/lkd_to_anm.py` (yeni dosya, bu oturumda yazıldı):

```
python tools/lkd_to_anm.py movie8_f.lkd -o firmware_assets/dolphins.anm --dither --stretch
```

**`--dither` şart.** Kaynak sadece 4 gri tonlu; sert eşikle 1-bit'e çevirince
resim parazite dönüşüyor. Bayer 4x4 taraması panelin kendi yaptığı işin aynısı.
`--stretch` o 4 seviyeyi tam aralığa yayıyor.

---

## 3. Kurucu artık çoklu paket yazıyor

Eskiden tek paket gömülüyordu (`firmware_assets/anim.anm`, sabit linker
sembolü). Sekiz animasyon için sekiz kez flash'lamak gerekecekti; onun yerine
kurucu bir **liste** taşıyacak hale getirildi.

İki yer birbiriyle uyuşmak zorunda:

- `platformio.ini` → `board_build.embed_files` (8 satır)
- `SpotifyDiyThing/animationInstaller.h` → `ANIM_INSTALLER_PACKS` tablosu

Linker sadece gerçekten gömdüğü dosyalar için sembol üretir, yani birinde olup
diğerinde olmayan bir isim **link hatası** verir, sessizce atlanmaz.

Kurucu ekranda `3/8` diye sayıyor. Karta doğru boyutta zaten duran paketi
atlıyor, yani yeniden çalıştırmak diğerlerini tekrar yazmıyor.

---

## 4. Sökülenler

- `DOLPHIN_ART[]` sprite'ı ve `DOLPHIN_*` sabitleri (`visualizerModes.cpp`)
- `drawDolphin()` fonksiyonu
- `dolphinPhase / dolphinDirection / dolphinArc / dolphinSplash` state'i
- `MODES[]` içindeki `{"DOLPHIN", ...}` satırı
- `tools/make_dolphin_pack.py` (silindi)
- `firmware_assets/anim.anm` (silindi)

**`MODE_COUNT` 41 → 40.** `visualizerRenderer.cpp`'deki `static_assert` bunu
tablodaki satır sayısıyla karşılaştırıyor, derleme geçtiğine göre tutuyor.

Not: `styleIndex` kalıcı olarak saklanıyor (Preferences). 41'den 40'a inince
eski kayıtlı indeks hâlâ geçerli aralıkta kalıyor, sorun yok — ama kart
açıldığında **beklediğinden farklı bir mod** gelebilir.

---

## 5. Seçici menü — YAZILDI AMA HİÇ DENENMEDİ

### Nasıl çalışması gerekiyor

- PIONEER modu duraklatılmışken: ortada oynat düğmesi, **sol altta yeni bir
  liste düğmesi** (üç çizgi, 40x36 px, x=10 y=198)
- Liste düğmesine basınca sahne bir **menüye dönüşüyor**: 8 paket alt alta,
  seçili olanın yanında ok işareti ve koyu şerit
- Bir satıra basınca o paket açılıyor, menü kapanıyor, o paketin ilk karesi
  poster olarak duruyor
- Sağ üstteki X menüden çıkıyor (görselleştiriciyi kapatmıyor)
- Menü açıkken sol/sağ mod değiştirme **kapalı** — yoksa soldaki bir satıra
  basmak aynı anda bir önceki moda da geçerdi

### Dokunulan dosyalar

| dosya | ne eklendi |
|---|---|
| `animationPlayer.h/.cpp` | `packNameAt()`, `currentPackIndex()`, `selectPack()` |
| `touchScreen.h/.cpp` | `OpenAnimationMenu`, `CloseAnimationMenu`, `PickAnimation` eylemleri; `setTouchAnimationMenu()`; `lastTouchY()` |
| `visualizerRenderer.h` | menü state'i + `MENU_TOP/ROW_HEIGHT/LEFT/RIGHT` sabitleri |
| `visualizerRenderer.cpp` | `openAnimationMenu()`, `closeAnimationMenu()`, `pickAnimationAt()`, `drawFrame()` içinde menü dalı |
| `visualizerModes.cpp` | `drawAnimationMenu()`, `drawAnimationListButton()` |
| `cheapYellowLCD.cpp` | üç yeni eylemin dispatch'i |

### Dikkat edilecek eşleşmeler

Bunlar iki yerde yazılı ve **elle uyumlu tutulmak zorunda**:

- Liste düğmesi: `drawAnimationListButton()` (visualizerModes.cpp) ↔
  `ANIMATION_LIST_ZONE` (touchScreen.cpp)
- Menü satır geometrisi: `drawAnimationMenu()` ↔ `pickAnimationAt()` — ikisi de
  `MENU_TOP` ve `MENU_ROW_HEIGHT`'tan türetiyor, o yüzden şimdilik güvenli

`TouchAction` enum'ı veri taşıyamıyor, o yüzden basılan satırın Y'si
`lastTouchY()` ile ayrıca okunuyor. İkisi dokunma görevinde birlikte
mandallanıyor, ayrışamazlar.

---

## 6. Derleme durumu (doğrulandı)

```
cyd2usb                 RAM 30.8%  Flash 43.4%  (1364421 / 3145728)
cyd2usb_anim_installer  RAM 28.2%  Flash 62.3%  (1960925 / 3145728)
```

Sekiz paketin de baytları `firmware.bin` içinde birebir bulundu
(offset 1576'dan başlayarak, 122896 bayt aralıklarla).

---

## 7. SIRADAKİ ADIM — yükleme

Kart **COM3**'te (CH340). Yüklemeden önce mutlaka sor.

```bash
# 1. Kurucu (8 paketi karta yazar)
PLATFORMIO_BUILD_DIR="C:/pio-build/CarDisplay-OS" \
  ~/.platformio/penv/Scripts/pio.exe run -t upload -e cyd2usb_anim_installer

# 2. Ekranda "ANIM READY" yazmasını bekle (3/8 diye sayacak)

# 3. Normal firmware
PLATFORMIO_BUILD_DIR="C:/pio-build/CarDisplay-OS" \
  ~/.platformio/penv/Scripts/pio.exe run -t upload -e cyd2usb
```

`pio` PATH'te değil, tam yol gerekiyor. OneDrive `.pio`'yu kilitlediği için
build dizini OneDrive dışında olmalı.

---

## 8. Flash sonrası kontrol listesi

Menü hiç denenmedi, en çok buraya bakılacak:

- [ ] PIONEER moduna gel — sol altta üç çizgili liste düğmesi görünüyor mu?
- [ ] Basınca menü açılıyor mu, 8 isim alt alta duruyor mu?
- [ ] Seçili olanın yanında ok ve koyu şerit var mı?
- [ ] Bir satıra basınca **o** animasyon geliyor mu? (satır/paket kayması var mı)
- [ ] En alttaki satır (`mecha`) ekrana sığıyor mu, basılabiliyor mu?
- [ ] Sağ üstteki X menüyü kapatıyor mu (görselleştiriciyi değil)?
- [ ] Menü açıkken sağa/sola basmak mod değiştiriyor mu? **Değiştirmemeli**
- [ ] Menüden çıkıp başka moda geçince mikrofon geri geliyor mu?
- [ ] Menü açıkken görselleştiriciyi kapatıp açınca menü kapalı geliyor mu?
- [ ] DOLPHIN modu listeden gerçekten kalktı mı, mod sayısı 40 mı?
- [ ] Eski kayıtlı `styleIndex` yüzünden tuhaf bir modda açılıyor mu?

---

## 9. Yapılmayanlar

- **Hiçbir şey commit'lenmedi.** `git status` 12 değişik + 9 yeni dosya
  gösteriyor.
- `V0.4.0_OTURUM_NOTLARI.md` hâlâ eski: 41 mod ve DOLPHIN modundan bahsediyor,
  animasyon kaynağı için video yöntemini anlatıyor. **Güncellenmesi lazım.**
- `YUKLEME_TALIMATI.md` tek paketli eski akışı anlatıyor olabilir, kontrol et.
- `README.md` güncellendi ama seçici menüden hiç bahsetmiyor.
- `tools/make_animation.py` içindeki örnekler hâlâ `dolphin.anm` diyor
  (zararsız, sadece dokümantasyon).
- `alt_*` (10 animasyon daha, 128x33 ama 150 kare) ve renkli `color_*` seti
  (192x48) hiç değerlendirilmedi. İstenirse oradan da seçilebilir.
- Sürüm numarası hâlâ `0.4.0` (`SpotifyDiyThing/version.h`). Bu iş bir sürüm
  atlaması sayılır, muhtemelen `0.5.0` olmalı.

---

## 10. Ortam notları

- `carozerra` reposu geçici klasöre klonlandı, **PC kapanınca silinecek**.
  Gerekirse tekrar klonla: `git clone --depth 1 https://github.com/youxufkhan/carozerra`
- `yt-dlp` ve `ffmpeg` bu oturumda winget ile kuruldu, PATH'e eklendi ama
  kabuk yeniden başlatılmadan görünmüyor olabilir. Tam yollar:
  - `~/AppData/Local/Microsoft/WinGet/Packages/Gyan.FFmpeg_*/ffmpeg-9.0.1-full_build/bin/`
  - `~/AppData/Local/Microsoft/WinGet/Packages/yt-dlp.yt-dlp_*/yt-dlp.exe`
- Sistemde çalışan Python **yok**; `python` komutu Microsoft Store kısayoluna
  düşüyor. PlatformIO'nunkini kullan:
  `~/.platformio/penv/Scripts/python.exe`
- Dosyaları Python ile yeniden yazarken **satır sonlarına dikkat**: bu oturumda
  bir kez `README.md` ve `platformio.ini` LF'den CRLF'e çevrildi ve diff tüm
  dosyayı değişmiş gösterdi. `io.open(..., newline='')` kullan.
