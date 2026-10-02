# How Bluetooth got through — and how it is connected now

This is the long version of the Bluetooth part of [README.md](README.md): what stood in the way, what was done
about each thing, and how the pieces are wired together in this firmware.

## 1. Why it was hard

The Cafe's original firmware (Peter Blasser) uses the ESP32 the way you would a bare microcontroller:

- It sets up the **I2S peripheral and the SAR ADCs by register**. The ADC's digital controller fills the I2S FIFO
  with conversions from a **pattern table** — ADC1 and ADC2 together ("double" mode). EARTH is one of those
  conversions.
- **The whole sound is one interrupt** (`attachInterrupt(2, preset, FALLING)`, once per sample). It reads the
  input from the codec over SPI3, writes the main out to the codec, ASH to the ESP32's own DAC, and YELLOW to ten
  GPIO pins, then restarts the I2S.
- It **switches the radio's clocks off** and **resets timer group 0** while it sets up its own timing.

Bluetooth needs most of what that takes away: the radio clocks, the system timer, a share of ADC2, and a lot of RAM.

## 2. What was in the way, and what was done (in the order it was met)

| # | what happened | why | what this firmware does |
|---|---|---|---|
| 1 | BLE advertised, but no phone could connect; `millis()` stood still | resetting **timer group 0** also stops the system clock (`esp_timer`), which BLE's timing runs on | only **SPI3** is reset (`CHANGNOR(DPORT_PERIP_RST_EN_REG, BIT(16))` in `initDEL`) |
| 2 | the radio didn't run at all | the original writes `DPORT_WIFI_CLK_EN_REG = 0` (radio clocks off) | skipped when Bluetooth is on (`initDIG`, `cafe_no_ble`) |
| 3 | the radio's calibration was off / unstable | it calibrates against the ADC, which the Cafe's setup then takes over | **the radio is started first** (`ble_begin()`), then the Cafe's hardware (`SETUPPERS`) |
| 4 | **EARTH read 0** in every preset | the radio's power detector takes **ADC2** all the time; in double mode the controller waits for ADC2 and every conversion stalls | the pattern table runs **ADC1 only** (single mode, `CTRLJING` in `setup.h`) |
| 5 | …so EARTH needed another way in | EARTH is wired to GPIO 4 = **ADC2 channel 0**; it can't move to ADC1 on this board | EARTH is read with the IDF driver, `adc2_get_raw`, which **arbitrates with the radio** (a reading the radio refuses is skipped and counted) |
| 6 | reading EARTH in `loop()` made YELLOW's organ crackle | `adc2_get_raw` holds a spinlock that disables interrupts on its core for each conversion — on core 1 that delayed the sound's interrupt | it is read from an **`esp_timer` callback on core 0**, every 500 µs (2000×/s, enough for audio-rate FM) (`earth_tick`) |
| 7 | (a task for it cost 1.6 KB of stack the radio needed) | | the esp_timer task already exists — the callback costs nothing extra |
| 8 | **no memory for the tape** | the tape is 2^17 samples × 12 bits ≈ 192 KB; with the radio running there is no single free block that big | the tape is **128 pieces of 1.5 KB** allocated one by one (they fit in the heap's gaps); `dread` / `dwrite` find the piece |
| 9 | still a few bytes short at boot | | the **last piece lives in RTC memory**; the BLE receive ring and a reply buffer too; `loop()`'s stack is 6 KB instead of 8 (in esp_cafe_duo) |
| 10 | IRAM ran out at link time | everything the interrupt touches wants to be in IRAM, and BLE's code uses some too | only what runs every sample is `IRAM_ATTR`; the rest runs from flash |
| 11 | SKIP stopped working after a restart | GPIO 34 / 35's analog (RTC) setting survives a software restart (an update ends in one) | `rtc_gpio_deinit` on both at every start, and `loop()` puts them back if anything muxed them to the RTC side |
| 12 | updates needed a cable | | firmware updates over Bluetooth (OTA), see §4 |

Without Bluetooth (the default: BUTTON not held at power-on) none of 1–6 apply: the radio clocks go off, the
pattern table runs ADC1 + ADC2 as the original, and EARTH comes from the I2S FIFO every sample.

## 3. How it is connected now

### Boot

```
power on
  ├─ BUTTON held for the whole 0.3 s?  ── no ──► cafe_no_ble = true  (no radio: the original behaviour)
  │                                     yes
  ├─ ble_begin(): NimBLE up, the GATT service, advertising "Cafe-XXXX"   ◄─ the radio first
  ├─ SETUPPERS → initDEL(): the tape (128 × 1.5 KB) · initDIG(): ADC1-only pattern table, I2S, SPI3, pins
  ├─ earth_tick every 500 µs (esp_timer, core 0)
  └─ PRESETTER(pool[0]): the sound's interrupt is attached (core 1)
```

### The two cores

```
            core 0                                              core 1
 ┌────────────────────────────────┐            ┌──────────────────────────────────────────┐
 │ NimBLE host task (the radio)    │            │ the sound: preset() — once per sample     │
 │   onWrite(RX)  ──► ble_rb (1 KB)│───────────►│   reads input, writes main / ASH / YELLOW │
 │   onWrite(OTA) ──► ota_rb       │            │   reads EARTHREAD, FLIP, SKIP, BUTTON      │
 │ esp_timer task                  │            │                                          │
 │   earth_tick ──► earth_now ─────│───────────►│ loop(): pc_service() takes whole lines out │
 │                                 │◄───────────│   of ble_rb → pc_line() → volatile vars    │
 │   notify(TX) ◄── ble_line()     │            │   ("G" → pc_goto → load_preset()),         │
 └────────────────────────────────┘            │   ota_service(), the BUTTON menu            │
                                               └──────────────────────────────────────────┘
```

- The BLE callbacks **only copy bytes** into a ring; nothing on core 0 touches the sound's state.
- `loop()` turns the ring into lines and runs them. Anything the sound reads is a `volatile` variable it picks up
  on its next sample.
- Changing preset stops the I2S for a moment, attaches the new interrupt and starts it again (`load_preset`) —
  the same as the BUTTON menu.

### The GATT service

| characteristic | UUID | properties | used for |
|---|---|---|---|
| (service) | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | | Nordic UART Service (advertised) |
| RX | `6E400002-…` | write, write without response | text lines to the Cafe |
| TX | `6E400003-…` | notify | the Cafe's replies |
| OTA | `6E400004-…` | write, write without response | firmware update data |

- The advertisement carries the service UUID; the name (`Cafe-` + two bytes of the MAC) is in the scan response.
- On connecting the Cafe asks for a **15 ms connection interval** (`updateConnParams(…, 12, 12, 0, 300)`) and an
  **MTU of 247**. The phone decides; `H` shows what it got.
- A reply longer than one packet is cut into notifications of MTU − 3 bytes, each line ending in `\n`.
- When a phone disconnects, the Cafe advertises again by itself (`advertiseOnDisconnect`).

### The text protocol

One command per line (`\n` or `\r`), up to ~250 bytes. Replies are lines too.

| send | reply |
|---|---|
| `P` | `HELLO cafe-ble <version> <Cafe-XXXX> ota` |
| `H` | `H heap <free> min <lowest> mtu <n> interval_ms <n> earth <0..255> fail <refused EARTH reads> preset <n>` |
| `G <0\|1>` | (none) the preset changes |
| `U …` | see §4 |

## 4. Firmware update over Bluetooth

```
phone                                   Cafe
  U <size> <crc32 hex>  ───────────►   stops the sound, frees the tape, opens the other app slot
                        ◄───────────   U OK 16384                (the receive ring's size)
  OTA: [offset LE32][data] …  ──────►  ring → flash (in loop())
                        ◄───────────   U A <bytes written>       (every 4 KB: send more)
                        ◄───────────   U R <offset>              (a piece was lost: send again from here)
                        ◄───────────   U DONE                    (CRC right: restarts into the new firmware)
                        ◄───────────   U ERR <why>               (anything wrong: restarts into the old one)
```

- Use the `.bin` from *Sketch → Export Compiled Binary* (not `.merged.bin`).
- **Partition Scheme: Default 4MB with spiffs** — it has the second app slot.
- If the link drops during an update, the Cafe restarts with the old firmware. Nothing breaks.

## 5. Known limits

- **One phone at a time** (`build_opt.h` allows 2 connections, the code serves one).
- **No WiFi.** With WiFi on, ADC2 can't be used at all — EARTH would need an ADC1 pin on a future board.
- The radio's transmissions can be heard as a faint noise on some outputs (its current pulses on the supply).
  Lowering the transmit power or lengthening the connection interval reduces it (not done here).
- esp_cafe_duo also re-advertises if advertising stopped and drops a link the phone has stopped using (`ble_watch`);
  this small version leaves that out.
