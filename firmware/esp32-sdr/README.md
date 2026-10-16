# LTEScope receive-only ESP-SDR firmware

LTEScope uses [`Kelsidavis/esp-sdr-s2-range`](https://github.com/Kelsidavis/esp-sdr-s2-range)
as its receive-only firmware base, pinned to commit
`11d6dfeb9542d26b5fd227d9ec027e00c7f1b046`. The browser speaks its protocol
6 command and framing formats directly over Web Serial.

## Target matrix

| Target | Connection | LTEScope capture mode | LoRa hardware |
| --- | --- | --- | --- |
| LilyGO T-Display S3 | Native USB Serial/JTAG | Continuous IQS1, 15.625–250 kSa/s | External SX1262 |
| Heltec WiFi LoRa 32 V3 | Onboard CP2102 UART | Gapped 16 MS/s IQ8 snapshots | Onboard SX1262 |
| CYD-2432S028 | Onboard USB-to-UART | Gapped 16 MS/s IQ8 snapshots | External module |

All three current images are headless receiver firmware. They do not initialize
the Heltec OLED, CYD TFT, or T-Display LCD; a blank or unchanged board display
does not indicate receiver failure. The spectrum and waterfall are rendered in
the LTEscope browser application. Local display support is a later, separate
board-integration milestone.

The S3 firmware requires native USB for continuous IQS1 streaming. The
T-Display S3 USB-C connector exposes native USB. Heltec V3 routes its USB-C
connector through a CP2102, so its stock connection uses snapshot mode. A
separate USB connector wired to ESP32-S3 GPIO19/20 can enable native USB on a
hardware-modified Heltec board.

The Heltec profile starts UART0 at 921600 baud, a standard divisor that is
reliable with its original CP2102 and the macOS CP210x driver. The browser
selects this rate from the bridge's USB vendor ID. T-Display native USB and
the CYD CH340 profile retain the upstream 2 Mbaud setting.

Unlike the previous Zephyr basis, this firmware also supports the classic
ESP32 in the CYD. Its snapshot captures have transfer gaps, but they cover a
wide instantaneous bandwidth and are useful for spectrum/waterfall monitoring.

The SX1262 remains an independent receiver. Its LoRa CAD/RSSI/packet telemetry
interface will be added as a second firmware/host protocol; it is not yet
exposed by this first ESP-SDR integration.

## Build

Install and activate the ESP-IDF revision recorded below, then run the desired
profiles:

```bash
# Required ESP-IDF commit:
# 25fe69f946311abdaf9ad56591f25fedbc20ac98

./firmware/esp32-sdr/build.sh t-display-s3
./firmware/esp32-sdr/build.sh heltec-v3
./firmware/esp32-sdr/build.sh cyd
./scripts/build-emscripten.sh
```

The board profiles produce merged browser-flashable images in
`web/firmware/<target>/`. Heltec and T-Display use the same ESP32-S3 receiver,
but the Heltec build has a board-specific UART baud configuration. Their
distinct manifests prevent accidental CYD flashing and leave room for later
board-specific LoRa/display builds.

## Flash and verify

Build the firmware before the browser app so CMake copies the generated images
into the web distribution. Start LTEScope with `./scripts/start-web.sh`, open
`http://localhost:8000/flash.html`, select the matching board, and follow the
ESP Web Tools prompt. The manifests install each merged image at offset zero.

For command-line recovery or development, put the board in its ROM downloader
and write the corresponding merged image:

```bash
esptool --chip esp32s3 --port PORT write-flash 0x0 \
  web/firmware/t-display-s3/ltescope-t-display-s3.bin
esptool --chip esp32s3 --port PORT write-flash 0x0 \
  web/firmware/heltec-v3/ltescope-heltec-v3.bin
esptool --chip esp32 --port PORT write-flash 0x0 \
  web/firmware/cyd/ltescope-cyd.bin
```

If a T-Display does not enter the downloader automatically, hold **BOOT**, tap
**RESET**, and release **BOOT**. After flashing, a normal reset must be made
without holding BOOT. CYD commonly enumerates as CH340 and Heltec V3 as CP2102.

The tested protocol-6 identity replies are:

```text
T-Display S3: S3SDR 6 burst 16380
Heltec V3:    S3SDR 6 burst 16380
CYD:          ESP32SDR 6 burst 16380
```

In LTEScope, choose **ESP-SDR board**, click **Start**, and select the desired
serial device. Native USB on T-Display enables continuous decimated IQ. Heltec
and CYD operate in gapped snapshot mode. The browser selects 921600 baud for
the Heltec CP2102 and 2 Mbaud for native Espressif USB and CYD CH340 devices.
During connection the app should say **Connecting ESP-SDR** and then show the
firmware identity above. If it remains at the Web Serial handshake, the serial
port or baud-rate negotiation has not completed.

## Limits and safety

The firmware accepts tuning attempts from 100–6000 MHz, but this is not a
guaranteed reception range. Its experimental 5/6 LO divider is intended to
extend ESP32/S3 reception to approximately 1842–2209 MHz, which includes LTE
Band 2 downlink at 1930–1990 MHz. Performance varies by chip and board.

LTEScope uses receive commands only. The firmware uses undocumented RF test
controls and should not later be used in a certified product. Continuous S3 IQ
is too narrow for a full LTE channel; UART snapshots contain gaps and cannot
feed the LTE decoder.

## License

The firmware is GPL-3.0-or-later. Each generated image directory includes the complete
license text, and the exact upstream and ESP-IDF commits are pinned in the
build script. See [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md).
