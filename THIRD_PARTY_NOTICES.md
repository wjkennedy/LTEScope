# Third-party notices

## esp-sdr-s2-range / ESP-SDR

LTEScope's optional ESP32 firmware is built from
[`Kelsidavis/esp-sdr-s2-range`](https://github.com/Kelsidavis/esp-sdr-s2-range),
commit `11d6dfeb9542d26b5fd227d9ec027e00c7f1b046`. That receive-only fork is
based on [ESPARGOS ESP-SDR](https://github.com/ESPARGOS/esp-sdr) and includes
work credited upstream to Florian Euchner, Zoltan Doczi, 14sea, h0m3us3r, and
other contributors.

The firmware is GPL-3.0-or-later. Generated image directories include its
complete license text, and the corresponding source is the pinned repository
plus the build definition in `firmware/esp32-sdr`. LTEScope's Web Serial host
is an independent implementation of the documented protocol 6 / IQS1 wire
formats and contains no copied firmware source.

## ESP Web Tools

The firmware page loads [ESP Web Tools](https://github.com/esphome/esp-web-tools)
10.4.0 from unpkg. ESP Web Tools is licensed under Apache-2.0 and is maintained
by the ESPHome/Open Home Foundation contributors.

## Bruce

The board split and CYD/T-Display hardware constraints were checked against
the adjacent [Bruce](https://github.com/pr3y/Bruce)
ESP32 firmware. Bruce is maintained by its contributors and licensed under
AGPL-3.0-or-later. LTEScope does not incorporate Bruce source code. If Bruce
code is later used for board display or LoRa integration, that image must
carry the AGPL notices and corresponding source.
