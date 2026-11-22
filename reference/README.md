# Hardware reference (local only)

Downloaded 2026-11-21 for the Waveshare ESP32-S3-RLCD-4.2. Not committed (`.gitignore`):
the ST7305 datasheet is marked Sitronix Confidential, and the rest are third-party files.
Re-download from the sources below if this folder is missing.

| Path | What | Source |
|---|---|---|
| `datasheets/ST_7305_V0_2.pdf` (+ `.txt`, `pdftotext -layout`) | ST7305 controller datasheet v0.2 (B8h PNLSET p. 81) | https://files.waveshare.com/wiki/common/ST_7305_V0_2.pdf |
| `datasheets/ES8311.DS.pdf` | ES8311 audio codec | https://files.waveshare.com/wiki/common/ES8311.DS.pdf |
| `datasheets/Pcf85063atl1118-*.pdf` | PCF85063 RTC | https://files.waveshare.com/wiki/common/Pcf85063atl1118-NdPQpTGE-loeW7GbZ7.pdf |
| `datasheets/SHTC3_Datasheet.pdf` | SHTC3 humidity sensor | https://files.waveshare.com/wiki/common/SHTC3_Datasheet.pdf |
| `datasheets/esp32-s3_*.pdf` | ESP32-S3 datasheet, technical reference manual | https://documentation.espressif.com/ |
| `schematics/ESP32-S3-RLCD-4.2-schematic.pdf` (+ `.txt`) | Board schematic | https://files.waveshare.com/wiki/ESP32-S3-RLCD-4.2/ESP32-S3-RLCD-4.2-schematic.pdf |
| `schematics/ESP32-S3-RLCD-4.2_20261010.zip` | V2 mechanical: PDF drawing, DXF, STEP | https://files.waveshare.com/wiki/ESP32-S3-RLCD-4.2/ESP32-S3-RLCD-4.2_20261010.zip |
| `waveshare-ESP32-S3-RLCD-4.2/` | Waveshare examples (ESP-IDF, Arduino, ESPHome), shallow clone at 9e754fd (2026-11-16); ST7305 init in `examples/esp-idf/*/components/port_bsp/display_bsp.cpp` | https://github.com/waveshareteam/ESP32-S3-RLCD-4.2 |

Index pages: https://docs.waveshare.com/ESP32-S3-RLCD-4.2/Resources-And-Documents.
Not linked there: an ES7210 datasheet and the V1 mechanical RAR (`ESP32-S3-RLCD-4.2-3dFile.rar`).
