# Car Display OS v0.4.0

Spotify album art, a full-screen clock and forty-one visualizers on a
2-USB Cheap Yellow Display (ESP32-2432S028R). Online it follows Spotify; offline
it runs the INMP441 visualizers, so the unit is useful before the phone hotspot
comes up and while the setup portal is still open.

The version number lives in exactly one place: `SpotifyDiyThing/version.h`.

## What it does

- **Player** - 160x160 album art, track/artist/album with Japanese glyph
  support, a thin progress line, and previous / play-pause / next controls.
  Taps get optimistic feedback and are sent by a dedicated high-priority task,
  so the screen never waits on the network.
- **Clock** - tap the header clock for a full-screen clock. Spotify polling and
  cover downloads keep running behind it.
- **Visualizers** - tap the Wi-Fi bars for forty-one modes, including a
  Pioneer-style leaping `DOLPHIN` and a `PIONEER` mode that plays frame packs
  off the SD card. A sun in the chrome row cycles brightness. Tap the right
  half of the screen for the next mode and the left half for the previous one;
  the top-right X closes. The chosen mode is stored in NVS, so the unit comes
  back on the same one after the ignition goes off. Buffered into an off-screen
  frame and pushed at ~24 FPS, so there is no visible clear pass.
- **Visualizer palette** - every mode draws from a single green-to-cyan ramp in
  `cydTheme.h`. Depth and energy are shown by moving along the hue axis, never
  by darkening, so no mode ever draws a muddy dark green. The player palette is
  a separate set of constants and is unchanged since v0.3.
- **Offline mode** - a boot with no reachable network opens the setup screen by
  itself after 15 seconds, and that screen carries the `OFFLINE VISUALIZER`
  button. Touching the connecting screen skips the wait entirely. No double
  power cycle is needed for any of it. The saved network keeps being re-tried
  behind the open portal, so a phone hotspot that appears late is picked up
  without a reboot.
- **Radio off means radio off** - opening the offline visualizer tears the AP,
  the DNS responder and the web server down and calls `WiFi.mode(WIFI_OFF)`.
  Closing it brings all three back. Nothing is transmitting while the mode is
  open, and the Wi-Fi stack stops competing with the microphone task for core 0.
- **Brightness** - tap the Spotify badge to cycle 100/75/50/25/10/5%, stored in
  NVS. At sunset the backlight dims once to 25%; the first manual press
  releases that cap for the rest of the night.
- **Album cache** - covers are cached to SD as pre-decoded RGB565, and a
  rolling window of the next eight queued covers is prefetched, so track
  changes usually draw with no network round trip at all.
- **Recovery** - a saved network reconnects without rebooting. Power-cycle twice
  quickly to force the setup portal open immediately instead of waiting for it.

## First-run setup

Connect to the `SpotifyDIY` hotspot (password `thing123`) and open the address
shown on screen. Besides the Spotify credentials the portal now configures:

| Field | Default | Why it matters |
| --- | --- | --- |
| Spotify market | `TR` | A wrong market makes Spotify report tracks as unavailable |
| POSIX timezone | `<+03>-3` | Drives the header and full-screen clocks |
| Latitude / longitude | Istanbul | Drives the automatic sunset dim |

These were compile-time constants pinned to Ireland and Sofia until v0.3.16.
Settings are stored next to the credentials in `/spotify_diy_config.json`; a
config written by an older build still loads and picks up the defaults above.


## Animations (the PIONEER mode)

Forty of the modes are procedural: they read the FFT and draw geometry. The
forty-first plays a pre-drawn frame sequence off the SD card, because that is
what the Pioneer head units this keeps being compared to actually did, and no
amount of trigonometry produces it.

Packs are 1 bit per pixel, matching the monochrome OEL panels the originals
used. That is not a compromise: a 320x200 frame is 8 KB against 128 KB for
RGB565, which is the difference between 192 KB/s off the card at 24 FPS and
3 MB/s. The single bit picks background or palette colour, so the animation
inherits the green-cyan rather than fighting it.

The packs that ship are the **original Pioneer animations**, not imitations of
them. Pioneer let owners load `.lkd` files onto units like the DEH-P7600MP over
CD or PC link; the [carozerra](https://github.com/youxufkhan/carozerra) project
reverse-engineered that format, and `tools/lkd_to_anm.py` converts one straight
to a pack:

```
python tools/lkd_to_anm.py movie8_f.lkd -o firmware_assets/dolphins.anm --dither --stretch
```

Use `--dither` (Bayer 4x4, the same trick the panel itself used to fake grey).
The source has only four grey levels and a hard threshold flattens the artwork
into noise. `--stretch` pulls those four levels out to the full range first.

Eight of them are installed by default: `dolphins`, `carrozzeria`, `reef`,
`flowers`, `canyon`, `city`, `roadster`, `mecha` - all 256x64, 60 frames, 17 fps.

For artwork that did not come off a head unit, `make_animation.py` still builds
a pack from a PNG sequence or a GIF:

```
python tools/make_animation.py frames/ -o dolphin.anm --fps 12
python tools/make_animation.py clip.gif -o dolphin.anm --fps 12
```

Copy the result to `/anim/dolphin.anm` on the same card the album cache uses.
Several packs in `/anim` play one after another. The mode says what is missing
if the directory is empty rather than showing a black screen.

To get frames out of a video:

```
ffmpeg -i clip.mp4 -vf fps=12,scale=320:-1 frames/%04d.png
```

The converter is standard library only - PNG through `zlib`, GIF through an LZW
decoder in the file - because PlatformIO ships a Python but no Pillow, and
needing a `pip install` into that virtualenv to convert an image is a poor
trade. Use `--invert` for artwork on a white background and `--threshold` to
move the cut between lit and unlit.

**Playback stops the microphone.** The I2S capture task and the FFT sit on core
0, which is where the SD reads land too, and a frame sequence needs neither. The
mode opens paused, showing frame zero behind a play button with the spectrum
still live underneath; pressing play switches the microphone off and gives the
frames the core. Pressing it again hands the microphone back.

## Building

Everything the project needs is vendored under `lib/`, so PlatformIO never has
to reach a library mirror while configuring.

```
pio run -e cyd2usb        # normal build (inverted panel, 2-USB CYD)
pio run -e cyd            # same board, non-inverted panel
pio run -t upload -e cyd2usb
```

> **If the link step fails with `cannot open map file`:** the `.pio` build
> directory is inside a OneDrive-synced folder and the sync client intermittently
> locks it. Uncomment the `build_dir` line in `platformio.ini`, or move the
> project out of OneDrive. Syncing tens of thousands of object files is not
> doing anything useful either way.

### Tests

The modules with no Arduino dependency - the millis() rollover arithmetic, the
bounded string copy, the microphone DSP and the sunrise/sunset model - are
covered by a Unity suite in `test/test_native/`.

```
pio pkg install -g -p native   # once; also needs g++ or clang on PATH
pio test -e native

pio test -e esp32_test         # or run the same suite on the board
```
### Installing a pack without a card reader

The card lives inside the unit, and not everybody has a reader. Same trick the
Japanese font uses: the pack is baked into a one-time firmware that writes it to
the card.

The installer carries a *list* of packs and writes them all in one pass, so
this is one upload however many animations are in it.

1. In PlatformIO Project Tasks open `cyd2usb_anim_installer` and click Upload.
2. Wait until the CYD shows `ANIM READY` (the screen counts `3/8` as it goes).
3. Open `cyd2usb` and click Upload.

The set it ships with is the eight original Pioneer animations above. To change
it, edit **both** `board_build.embed_files` in `platformio.ini` and the
`ANIM_INSTALLER_PACKS` table in `SpotifyDiyThing/animationInstaller.h`. They
have to agree: the linker only emits symbols for files it actually embedded, so
a name in one and not the other is a link error rather than a silent skip.

Keep the list at eight or fewer. `AnimationPlayer::MAX_PACKS` is 8, so a ninth
pack would be written to the card and then never played.

A pack already on the card at the right size is left alone, so re-running the
installer after adding one does not rewrite the others.

`tools/make_dolphin_pack.py` reads the sprite out of `visualizerModes.cpp`
rather than keeping its own copy, so editing the dolphin art changes both the
procedural `DOLPHIN` mode and the pack.


## Japanese font (no card reader needed)

1. Leave the FAT32 microSD card in the CYD.
2. In PlatformIO Project Tasks, open `cyd2usb_font_installer` and click Upload.
3. Wait until the CYD shows `FONT READY`.
4. Open `cyd2usb` and click Upload.

Only needed once, or again after replacing or reformatting the card.

## Layout

```
SpotifyDiyThing/
  SpotifyDiyThing.ino      setup() and loop() only
  version.h                the single version string
  deviceConfig.*           persisted settings, credentials + region
  WifiManagerHandler.*     connect / captive setup portal
  refreshToken.*           one-page Spotify authorisation helper
  spotifyLogic.*           polling, playback commands, queue prefetch
  touchScreen.*            touch sampling task, publishes one TouchAction
  cheapYellowLCD.*         the player screen and the overlays
    cydTheme.h               palette, layout, the single TFT instance
    albumArtCache.*          cover download, SD/SPIFFS caching, prefetch
    backlightController.*    manual levels, NVS, automatic sunset dim
    visualizerRenderer.*     mode dispatch, frame buffer, the v0.3 modes
    visualizerModes.*        the twenty modes added in v0.4
    visualizerGeometry.h     shared integer direction tables
  audioVisualizer.*        I2S capture task
  audioSpectrum.*          FFT and band mapping     (Arduino-free, tested)
  solarTime.*              sunrise/sunset            (Arduino-free, tested)
  timeMath.h               rollover-safe ticks       (Arduino-free, tested)
  stringUtil.h             bounded copy              (Arduino-free, tested)
```

Everything that touches the TFT runs on the Arduino loop task. The background
tasks - album download, playback commands, touch sampling, microphone capture -
do network, SD and I2S work only.

## Wiring

INMP441 microphone, for the visualizers:

| INMP441 | CYD |
| --- | --- |
| VDD | CN1 3V3 |
| GND | CN1 / P3 GND |
| SCK | GPIO22 |
| WS | GPIO27 |
| SD | GPIO35 (input-only) |
| L/R | GND (left channel) |

GPIO21 is deliberately unused: it drives the TFT backlight.

Bluetooth and the Android companion are not part of this build. NFC is compiled
out by default; re-enable `NFC_ENABLED` only with a PN532 attached.

---

# Spotify-Diy-Thing

Something similar to the [Spotify Car Thing](https://carthing.spotify.com/), built with a cheap ESP32 Screen. Connects to your Spotify account and displays your currently playing song with its album art

![image](https://user-images.githubusercontent.com/1562562/221344692-7dd359d3-2e64-4a09-850b-b619477c5043.png)

This project is a Work in Progress!

## Help Support what I do!

[If you enjoy my work, please consider becoming a Github sponsor!](https://github.com/sponsors/witnessmenow/)

## Hardware Required

This project is designed to make use of basically ready to go hardware, so is very easy to get up and running

Currently this project runs on two types of hardware:

### "Cheap Yellow Display" (CYD)

An ESP32 With Built in 320x240 LCD with Touch Screen (ESP32-2432S028R), buy from wherever works out cheapest for you:

- [Aliexpress\*](https://s.click.aliexpress.com/e/_DkSpIjB)
- [Aliexpress\*](https://s.click.aliexpress.com/e/_DkcmuCh)
- [Aliexpress](https://www.aliexpress.com/item/1005004502250619.htm)
- [Makerfabs](https://www.makerfabs.com/sunton-esp32-2-8-inch-tft-with-touch.html)

### Matrix panel (ESP32 Trinity)

It's built to work with the [ESP32 Trinity](https://github.com/witnessmenow/ESP32-Trinity), an open source board I created for controlling Hub75 Matrix panels, but it will does work with any ESP32 that breaks out enough pins.

The display it uses is a 64x64 HUB75 Matrix Panel.

All the parts can be purchased from Makerfabs.com:

- [ESP32 Trinity](https://www.makerfabs.com/esp32-trinity.html)
- [64 x 64 Matrix Panel](https://www.makerfabs.com/64x64-rgb-led-matrix-3mm-pitch.html)
- Optional: [5V Power Supply](https://www.makerfabs.com/5v-6a-ac-dc-power-adapter-with-cable.html) - You can alternatively use a USB-C power supply

\* = Affilate Link

### BYOD (Bring your own display)

I've tried to design this project to be modular and have abstracted the display code behind an interface, so it should be pretty easy to get it up and running with a different type of display.

## NFC Tags (Optional)

One of the coolest parts about this project, in my opinion at least, is the ability to connect an NFC reader to control what songs/albums/playlists are being played.

You can write the spotify URI or URL of the song, album or playlist to an NFC tag and when you swipe it off the reader, the device will tell your spotify account to play what it reads.

If you aren't interested in this feature, you can just not connect it and the device will work without it.

### PN532 NFC reader and Tags

This code has been tested with these red PN532 NFC readers and cheap NFC stickers.

To use the PN532 as an SPI device, you need to configure the toggle switches so switch 1 is down and 2 is up. (You may need to remove the sticker on top of it)

#### Links

- [PN532 NFC reader - Aliexpress\*](https://s.click.aliexpress.com/e/_DCanbAB)
- [NFC Stickers - Aliexpress\*](https://s.click.aliexpress.com/e/_DkX2F5z)

### Hardware support

#### "Cheap Yellow Display" (CYD)

The CYD does not have enough free pins to use an SPI device by default, and the NFC reader is quite slow over i2c, so we need to get creative.

With the help of an "SD Card Sniffer", we can make use of the Micro SD slot of the CYD to connect the NFC reader to.

##### Connections

| Sniffer Board Label | ESP32 Pin | PN532 NFC |
| ------------------- | --------- | --------- |
| DAT2                | -         | -         |
| CD                  | IO5       | CS        |
| CMD                 | IO23      | DI / MOSI |
| GND                 | GND       | GND       |
| VCC                 | 3.3V      | VCC       |
| CLK                 | IO18      | SCLK      |
| DAT0                | IO19      | DO / MISO |
| DAT1                | -         | -         |

#### Links

- [Micro SD Card Sniffer - Aliexpress\*](https://s.click.aliexpress.com/e/_Ddwcy9h)

#### Matrix Panel (ESP32 Trinity)

Again, it is designed for the ESP32 Trinity, but can work with any ESP32 that breaks out the required pins.

The Trinity has some pins broken out that can be used for this purpose

#### Connections

| ESP32 Pin  | PN532 NFC |
| ---------- | --------- |
| IO22 (SCL) | CS        |
| IO21 (SDA) | DI / MOSI |
| GND        | GND       |
| 3.3V       | VCC       |
| IO33       | SCLK      |
| IO32       | DO / MISO |

## Project Setup

These steps only need to be run once.

### Step 1 - Spotify Dev Account

In order to use this project, you will need to create an application on the Spotify Developer Dashboard

- Sign into the [Spotify Developer page](https://developer.spotify.com/dashboard/login)
- Create a new application. (name it whatever you want). There is a section fo "callback URI" you can just make this "locahost" for now.
- You will need the "client ID" and "client secret" from this page later in the steps

You will need to add a callback URI for authentication process by clicking "Edit Settings", what URI to use will be displayed on screen in a later step.

### Step 2 - Flash the Project

Flash the project directly from your browser [here](https://witnessmenow.github.io/Spotify-Diy-Thing) (Chrome & Edge only)

or

Jump to the "code" section below

### Step 3 - Adding your Wifi and Spotify Details

The device is now flashed with the code, but it doesn't know what your Wifi or Spotify details are.

In order to enter your wifi details, the project will host it's own wifi network. Connect to it using your phone.

- SSID: SpotifyDiy
- Password: thing123

You should be automatically redirected to the config page.

- Click Config
- Enter your WIfi details (2.4Ghz only)
- Enter the client Id and client secret from the earlier step
- You can leave refresh token blank
- Click save

Note: If you ever need to get back into this config section, click reset button (the button closest to the side) twice.

### Step 4 - Authenticating the device with your Spotify account

The final step is to connect this device to your Spotify account. When the Wifi is configured correctly, it will enter "Refresh Token Mode".

- Your device will now be connected to the Wifi details you gave it.
- Go to the address displayed on screen using your phone or PC that is connected to the same network as your device.
- On the webpage that loads there will be an address displayed in bold, add this to the callback URI section as mentioned in the Spotify API section
- Click the `Spotify Auth` URL
- You will need to give permission to the app you created to have access to your spotify account

Your project should now be setup and will start displaying your currently playing music!

## Code

If you want to program this project manually, there are two options

### PlatformIO

PlatformIO is the easiest way to code this project.

In the [platformio.ini](platformio.ini), there are several environments defined for the different boards

| Environment | Description                                                                                                                  |
| ----------- | ---------------------------------------------------------------------------------------------------------------------------- |
| env:cyd     | For the [Cheap Yellow Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)                                   |
| env:cyd2usb | For the Cheap Yellow Display with two USB ports                                                                              |
| env:trinity | For the [ESP32 Trinity](https://github.com/witnessmenow/ESP32-Trinity) (or generic ESP32 wired to the matrix panel the same) |

When you select the environment, it will automatically install the right libraries and set the configurations in the code.

### Arduino IDE

If you want to use the Arduino IDE, you will need to do the following to get it working

The following libraries need to be installed for this project to work:

| Library Name/Link                                                                | Purpose                                    | Library manager                 |
| -------------------------------------------------------------------------------- | ------------------------------------------ | ------------------------------- |
| [SpotifyArduino](https://github.com/witnessmenow/spotify-api-arduino)            | For interacting with Spotify API           | No                              |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson)                          | Dependancy of the Spotify API              | Yes ("Arduino Json")            |
| [JPEGDEC](https://github.com/bitbank2/JPEGDEC)                                   | For decoding the album art images          | Yes ("JPEGDEC")                 |
| [WifiManager - By Tzapu](https://github.com/tzapu/WiFiManager)                   | Captive portal for configuring the WiFi    | Yes ("WifiManager")             |
| [ESP_DoubleResetDetector](https://github.com/khoih-prog/ESP_DoubleResetDetector) | Detecting double pressing the reset button | Yes ("ESP_DoubleResetDetector") |
| [Seeed_Arduino_NFC](https://github.com/witnessmenow/Seeed_Arduino_NFC)           | For the NFC reader                         | No (it's a modified version)    |

#### Cheap Yellow Display Specific libraries

| Library Name/Link                              | Purpose                         | Library manager  |
| ---------------------------------------------- | ------------------------------- | ---------------- |
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | For controlling the LCD Display | Yes ("tft_espi") |

#### Matrix Panel Specific libraries

| Library Name/Link                                                                                 | Purpose                          | Library manager          |
| ------------------------------------------------------------------------------------------------- | -------------------------------- | ------------------------ |
| [ESP32-HUB75-MatrixPanel-I2S-DMA](https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA) | For controlling the LED Matrix   | Yes ("ESP32 MATRIX DMA") |
| [Adafruit GFX library](https://github.com/adafruit/Adafruit-GFX-Library)                          | Dependency of the Matrix library | Yes ("Adafruit GFX")     |

#### Cheap Yellow Display Display Config

The CYD version of the project makes use of [TFT_eSPI library by Bodmer](https://github.com/Bodmer/TFT_eSPI).

TFT_eSPI is configured using a "User_Setup.h" file in the library folder, you will need to replace this file with the one in the `DisplayConfig` folder of this repo.

#### Display Selection

At the top of the `SpotifyDiyThing.ino` file, there is a section labeled "Display Type", follow the instructions there for how to enable the different displays.

Note: By default it will use the Cheap Yellow Display
