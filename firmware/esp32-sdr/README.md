# LTEScope receive-only ESP-SDR firmware

LTEScope uses [`Kelsidavis/esp-sdr-s2-range`](https://github.com/Kelsidavis/esp-sdr-s2-range)
as its receive-only firmware base, pinned to commit
`11d6dfeb9542d26b5fd227d9ec027e00c7f1b046`. The browser speaks its protocol
6 command and framing formats directly over Web Serial.

## Target matrix

| Target | Connection | LTEScope capture mode | Validation | LoRa hardware |
| --- | --- | --- | --- | --- |
| LilyGO T-Display S3 | Native USB Serial/JTAG | Continuous IQS1, 15.625–250 kSa/s IQ8; experimental 500 kSa/s IQ4 and 2 MSa/s IQ1 | 250 kSa/s waterfall and gapless 2 MSa/s transport confirmed | External SX1262 |
| Heltec WiFi LoRa 32 V3 | Onboard CP2102 UART | Gapped 16 MS/s IQ8 snapshots | Protocol confirmed; app retest pending | Onboard SX1262 |
| CYD-2432S028 | Onboard USB-to-UART | Intended gapped snapshots | Experimental: boots, no protocol reply on tested board | External module |

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

LTEScope applies a small, checked-in patch to the pinned upstream source. It
adds a phase-aligned ×32 FIR decimator for 500 kSa/s output and exposes the
existing first-stage ×8 FIR directly for 2 MSa/s output. The modes use packed
IQ4 (one complex sample per byte) and sign-only IQ1 (four complex samples per
byte), respectively, keeping both streams near 0.5 MB/s over full-speed USB
Serial/JTAG. The original ×64 through ×1024 IQ8 modes are unchanged, and
250 kSa/s remains the default until the higher rates are validated for
RF quality on hardware. Before IQ1 packing, the firmware applies a slow
fixed-point DC estimate so receiver offset does not collapse either sign bit.

A 10-second T-Display test at 1950 MHz delivered 20,001,433 samples at an
effective 1.998 MSa/s and 0.499 MB/s, with zero CRC errors, firmware gap flags,
or missing sample indices. IQ2 at the same rate was also tested, but exceeded
the measured USB Serial/JTAG ceiling and dropped frames; it remains a protocol
experiment for a future TinyUSB bulk transport rather than an app default.

The Heltec profile starts UART0 at 921600 baud. The CYD profile currently uses
a conservative 115200 baud because high-rate transfers were corrupt through
the tested CH340/macOS path; its snapshot refresh is correspondingly slow.
The browser selects these rates from the bridge's USB vendor ID. T-Display
native USB retains the upstream 2 Mbaud setting.

The upstream basis contains a classic ESP32 snapshot backend intended for the
CYD. On the tested CYD the image boots but does not reach the command protocol,
even after isolating UART0 and reducing the CH340 rate. Treat this target as
experimental until a protocol reply and app waterfall are confirmed.

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

The protocol-6 identity replies are:

```text
T-Display S3: S3SDR 6 burst 16380
Heltec V3:    S3SDR 6 burst 16380
CYD:          ESP32SDR 6 burst 16380
```

T-Display and Heltec identities above have been verified on hardware. The CYD
line is the expected upstream identity and has not yet been observed on the
tested CYD.

In either the browser build or native macOS/Linux LTEScope, choose **ESP-SDR
board**, click **Start**, and select the desired serial device. Native USB on
T-Display enables continuous decimated IQ. Heltec
operates in gapped snapshot mode; CYD is intended to use the same mode once its
protocol issue is resolved. The browser selects 921600 baud for the Heltec
CP2102, 115200 baud for the CYD CH340, and 2 Mbaud for native Espressif USB.
During connection the app should say **Connecting ESP-SDR** and then show the
firmware identity above. If it remains at the Web Serial handshake, the serial
port or baud-rate negotiation has not completed.

Native builds retain the full LTE decoder and allow it to be started with an
ESP-SDR source selected. T-Display's 250 kS/s default and experimental 500 kS/s
stream remain undersampled. The experimental 2 MS/s IQ1 stream meets the
decoder's nominal 1.92 MS/s input rate, but its sign-only quantization and USB
continuity still make this an optimization and signal-processing experiment
rather than a claim of full-channel LTE support.
The LTE sweep also supports ESP-SDR and restarts the serial IQ stream after each
frequency change.

## Limits and safety

The firmware accepts tuning attempts from 100–6000 MHz, but this is not a
guaranteed reception range. Its experimental 5/6 LO divider is intended to
extend ESP32/S3 reception to approximately 1842–2209 MHz, which includes LTE
Band 2 downlink at 1930–1990 MHz. Performance varies by chip and board.

LTEScope uses receive commands only. The firmware uses undocumented RF test
controls and should not later be used in a certified product. The experimental
2 MS/s S3 stream is only wide enough for the smallest LTE channel and uses IQ1;
UART snapshots contain gaps and cannot feed the LTE decoder.

## License

The firmware is GPL-3.0-or-later. Each generated image directory includes the complete
license text, and the exact upstream and ESP-IDF commits are pinned in the
build script. See [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md).
