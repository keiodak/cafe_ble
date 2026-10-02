# cafe_ble

A small, plain firmware for the ESP32 inside the **Ciat-Lonbarde Cafe / Cafeteria** — a starting point for anyone
who wants to build their own presets or talk to the Cafe over **Bluetooth LE**.

- Two presets: **COCO_MOD** (a looper) and **ECHO** (a four-tap echo with an organ on YELLOW).
- **Bluetooth only when you ask for it**: hold BUTTON while the Cafe powers on. Otherwise it behaves exactly like
  the original firmware (radio off).
- About 1200 lines in five files. Everything the presets do not use has been taken out.

日本語の概要は[下](#日本語)にあります。

---

## Build and upload

| | |
|---|---|
| IDE | Arduino IDE 2.x |
| Board | **ESP32 Dev Module** (Espressif ESP32 core **2.0.9** — 3.x changed the low-level APIs this uses) |
| Partition Scheme | **Default 4MB with spiffs** (needed for updates over Bluetooth) |
| Library | **NimBLE-Arduino** (h2zero), tested 2.3.x |

1. Put this folder where Arduino IDE finds sketches (the folder must stay named `cafe_ble`).
2. Open `cafe_ble.ino`. All five files open as tabs. Keep `build_opt.h`: it trims NimBLE to what the Cafe needs.
3. Upload over USB (hold the ESP32's BOOT button if it can't connect).
4. Serial Monitor at 115200 shows the boot and whether Bluetooth is on.

## Playing it

| | COCO_MOD (1) | ECHO (2) |
|---|---|---|
| main out / ASH | the tape | the wet echo |
| YELLOW | a clock from the loop | an organ (5 octaves of squares) |
| EARTH | record on / off (as a switch) | the organ's pitch (FLIP: PITCH → RING → OFF) |
| SKIP | back to the loop point | wobble on / off |
| FLIP | backwards | (see EARTH) |
| BUTTON (short) | freeze | freeze |

**Changing preset:** long-press BUTTON (the lamp flickers), tap it N times, long-press again. The lamp blinks the
number.

## Bluetooth

Hold **BUTTON** while powering on (about 0.3 s). The Cafe then advertises as **Cafe-XXXX** (the last bytes of its
MAC) with the **Nordic UART Service**:

| UUID | |
|---|---|
| `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | the service |
| `6E400002-…` (write) | text lines to the Cafe |
| `6E400003-…` (notify) | the Cafe's replies |
| `6E400004-…` (write without response) | firmware update data |

Any BLE UART app works for trying it (nRF Connect, LightBlue, Bluefruit Connect …). One command per line:

| send | reply / effect |
|---|---|
| `P` | `HELLO cafe-ble <version> <name> ota` |
| `H` | free heap, MTU, connection interval, EARTH, the preset |
| `G 0` / `G 1` | go to COCO_MOD / ECHO |
| `U <size> <crc32 hex>` | start a firmware update (see `ota_cmd`) |

**Adding a command:** a new `case` in `pc_line()` (`cafe_ble.ino`). It runs in `loop()`, not in the audio
interrupt. If it changes something the sound reads, make that a `volatile` variable.

**Adding a preset:** write a function like `coco_mod()` in `synths.h` and put it in `pool[]` (`cafe_ble.ino`). A
preset is the audio interrupt itself: it is called once per sample, reads the input (`ADCREADER`), writes the main
out (`DACWRITER`), ASH (`ASHWRITER`) and YELLOW, and must restart the I2S at its end (copy the "HEARTBEAT" lines).
Keep it short. It runs on core 1; Bluetooth runs on core 0.

## What it took to fit Bluetooth around the Cafe's sound

The original firmware owns the ESP32 completely: it programs the I2S, the SAR ADCs and the timers by register,
makes the whole sound in one interrupt, and turns the radio's clocks off. With Bluetooth on (`cafe_no_ble = false`)
this firmware does the following. Without it, it does exactly what the original does.

- **The radio starts first**, then the Cafe's own hardware setup. The radio calibrates against a clean ADC.
- **The radio clocks stay on.** The original writes `DPORT_WIFI_CLK_EN_REG = 0`.
- **Only SPI3 is reset, never timer group 0.** Resetting timer group 0 also stops the system clock (`esp_timer`,
  `millis()`). BLE then advertises but can't connect.
- **EARTH.** The original reads EARTH (GPIO 4 = ADC2 channel 0) through the SAR's DMA pattern table, ADC1 and ADC2
  converted together. The radio's power detector takes ADC2 all the time, so in that mode every conversion stalled
  and EARTH read 0. Instead:
  - the pattern table runs ADC1 only (single mode);
  - EARTH is read with the IDF driver (`adc2_get_raw`, which shares ADC2 with the radio) from an `esp_timer`
    callback on core 0, 2000 times a second (`earth_tick`).
- **Memory.** With the radio running there is no single free block for the tape (2^17 samples of 12 bits). It is
  allocated as 128 pieces of 1.5 KB, the last one in RTC memory; `dread` / `dwrite` find the piece.
- **SKIP and FLIP** (GPIO 34 / 35) are handed back to the digital side at every start. Their analog setting survives
  a software restart, and an update ends in one.
- **Updates over Bluetooth:** `U <size> <crc32>` stops the sound and frees the tape's memory for a receive ring. The
  binary arrives on `6E400004` in pieces of `[offset: 4 bytes LE][data]`, is written to the other app slot, and is
  CRC-checked. The Cafe then restarts into it. Anything wrong and it restarts into the old one.
- **WiFi** is not used. With WiFi on, ADC2 cannot be used at all, so EARTH would need an ADC1 pin.

## Files

| file | |
|---|---|
| `cafe_ble.ino` | Bluetooth, the commands, the firmware update, `setup()`, `loop()` and the BUTTON menu |
| `synths.h` | the two presets |
| `stuff.h` | inputs (BUTTON, FLIP, SKIP, EARTH), the lamp, the outputs (main, ASH, YELLOW), BUTTON's handler, the tape |
| `setup.h` | the ESP32's registers and the ADC / I2S setup (the original firmware's) |
| `build_opt.h` | NimBLE options (2 connections, peripheral only) |

## Credits

- The original Cafe firmware and hardware: **Peter Blasser, Ciat-Lonbarde** — the hardware setup (`setup.h`, most
  of `initDEL`) and ECHO's four taps are his.
- **Apple π** by ieat31415 (alternative Cafe firmware): COCO_MOD, the BUTTON menu, the ASH and YELLOW writers.
- Cut down, Bluetooth, the firmware update and ECHO's organ: k.odk.

The parts above come from those projects, which do not state a license. If you build on this, credit them as above.

---

## 日本語

Ciat-Lonbarde Cafe の ESP32 用の小さなファームウェアです。自分でプリセットを作ったり、Bluetooth LE で Cafe
と話したりするための出発点として作りました。

- プリセットは **COCO_MOD**（ルーパー）と **ECHO**（4タップのエコー、YELLOW にオルガン）の2つ
- **BUTTON を押したまま電源を入れたときだけ Bluetooth が起動**します。普通に起動するとオリジナルと同じです
- 全部で約1200行。使わない部分は取り除いてあります

**書き込み**：Arduino IDE、ボード「ESP32 Dev Module」、ESP32 core 2.0.9、Partition Scheme「Default 4MB with
spiffs」、ライブラリ NimBLE-Arduino。`cafe_ble.ino` を開いて USB で書き込みます。

**Bluetooth**：Nordic UART Service（`6E400001-…`）。`P`（バージョン）、`H`（状態）、`G 0` / `G 1`（プリセット）、
`U …`（ファーム更新）。コマンドは `pc_line()` に、プリセットは `synths.h` と `pool[]` に足します。

**Bluetooth を入れるために変えた点**：無線を先に起動する、無線のクロックを切らない、timer group 0 をリセット
しない、変換表を ADC1 だけにして EARTH（ADC2）はドライバ経由で core 0 から読む、テープを 1.5KB × 128 に分けて
確保する。詳しくは上の英語の説明を見てください。

**クレジット**：オリジナル（Peter Blasser / Ciat-Lonbarde）、Apple π（ieat31415）、k.odk。
