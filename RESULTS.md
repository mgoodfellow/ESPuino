# FastLED FLEX_IO experiment results

Date: 2026-08-13

## Verdict

The pinned FastLED candidate compiles and links the intended non-SPI LED engines:

- classic ESP32: `fl::Bus::FLEX_IO, 0` -> `ChannelEngineI2sEsp32Dev` -> I2S1/DMA;
- ESP32-S3: `fl::Bus::FLEX_IO, 0` -> `ChannelDriverLcdClockless` -> LCD_CAM/GDMA.

The candidate builds do not link NeoPixelBus or FastLED's `SpiStripWs2812` transport. Classic ESP32 hardware testing identified and corrected a FastLED I2S sample-format defect: the 32-bit mono transport duplicated/corrupted the WS2812 stream, while direct 16-bit samples with the corresponding FIFO and GPIO-matrix mapping display the expected animation. The fix was visually confirmed on a 12-pixel ring and reproduced from a freshly downloaded dependency through the fail-closed PlatformIO patch hook. The regenerated firmware also booted without a panic and ran alongside SPI SD, a separate-SPI MFRC522 reader, Wi-Fi, and the web server.

The classic ESP32 transport verdict is therefore **working on the tested D32 hardware**. Audio playback coexistence, runtime heap deltas, OTA, and sustained stress testing remain outstanding. ESP32-S3 remains compile/link-only and requires independent hardware validation.

## Build identity

| Build | ESPuino revision | FastLED revision | Environment | Platform / core / IDF | Result |
|---|---|---|---|---|---|
| A, classic clockless-SPI baseline | `2e45be4ea7ec86c1f12489400783f3bf8d1b4bd6` | `20667c3a6413ed46a828f78ec95fb57e58d753f8` | `lolin_d32_pro`, HAL 4 | pioarduino 55.03.39 / Arduino 3.3.9 / IDF 5.5.4 | clean build |
| B, S3 NeoPixelBus baseline | `2fb6f8dd4a8dcdae1d947b0040fcca998aa9dd66` | `20667c3a6413ed46a828f78ec95fb57e58d753f8` plus NeoPixelBus 2.8.4 | `esp32-s3-devkitc-1`, s3-proto HAL 99 | pioarduino 55.03.38-1 / Arduino 3.3.8 / IDF 5.5.4 | clean build |
| C1, classic FLEX_IO candidate | `2e45be4ea7ec86c1f12489400783f3bf8d1b4bd6` plus experiment patch | `448f6ee2e77fa22b856acc2794bca6c6811c905b` | `lolin_d32_pro`, HAL 4 | pioarduino 55.03.39 / Arduino 3.3.9 / IDF 5.5.4 | clean build; I2S1 engine linked; 16-bit fix hardware-tested |
| C2, maintainer FLEX_IO candidate | same as C1 | same as C1 | `lolin_d32_pro_sdmmc_pe`, HAL 7 | pioarduino 55.03.39 / Arduino 3.3.9 / IDF 5.5.4 | clean build; I2S1 engine linked |
| C3, S3 FLEX_IO candidate | `2fb6f8dd4a8dcdae1d947b0040fcca998aa9dd66` plus experiment patch | `448f6ee2e77fa22b856acc2794bca6c6811c905b` | `esp32-s3-devkitc-1`, s3-proto HAL 99 | pioarduino 55.03.38-1 / Arduino 3.3.8 / IDF 5.5.4 | clean build; LCD clockless engine linked |
| Guard build | same source as C1, with `NEOPIXEL_ENABLE` disabled only in the isolated worktree | candidate is resolved but LED headers/code are excluded | `lolin_d32_pro` | pioarduino 55.03.39 / Arduino 3.3.9 / IDF 5.5.4 | clean build |

The classic D32 Pro conflict build is HAL 4: onboard SD uses SPI and RFID uses the second SPI bus. The S3 profile uses SPI SD, separate SPI RFID, GPIO 21 for a 12-pixel WS2812B/GRB ring, and octal PSRAM at 40 MHz.

## Classic ESP32 hardware run

The C1 candidate was uploaded over `/dev/ttyUSB0` to an ESP32-D0WD-V3 revision 3.1 with 16 MB flash. NVS was preserved. The first `FastLED.show()` completed and emitted:

```text
LED: FastLED FLEX_IO instance 0 -> I2S/ESP32 I2S parallel SPI (engine I2S, 12 leds on GPIO 12)
```

After correcting the isolated worktree's global SD setting to match the D32 Pro onboard slot (SPI rather than unsupported SD_MMC), the same boot also established:

- SDHC mounted over SPI: 30,436 MB total, 17,653 MB free;
- MFRC522 detected on the separate SPI bus, version `0x82`;
- Wi-Fi associated and received `192.168.20.13`;
- mDNS and the HTTP server started;
- 166,223 bytes free heap after setup and 4,178,336 bytes PSRAM reported;
- no reset, watchdog, assertion, or LED-driver error during the 75-second serial capture.

An earlier run with `SD_MMC_1BIT_MODE` accidentally enabled failed to mount the D32 Pro slot and entered the configured SD boot-failure deep sleep after 20 seconds. The board profile explicitly declares that slot incompatible with SD_MMC; this was a test-configuration error, not an I2S/SD coexistence result.

The initial 32-bit transport did not render correctly. A frame with four expected white pixels produced eight lit pixels whenever physical pixel 0 was active. Per-frame CRGB logging showed the application buffer consistently contained exactly the intended four pixels, ruling out the animation, brightness, gamma, color correction, startup state, and optimized transpose input.

The defect was isolated to the classic ESP32 I2S transport format. The failing implementation used 32-bit mono samples, `tx_fifo_mod = 3`, a 16-to-32-bit expansion stage, and I2S1 `DATA_OUT0..15`. The working implementation instead:

- feeds the transpose kernel's 16-bit words directly to DMA;
- sets `tx_bits_mod = 16` and `tx_fifo_mod = 1`;
- routes the 16 lanes through `DATA_OUT8..23`.

The user visually confirmed the resulting 12-pixel animation was correct. A custom pulse-timing experiment (`0` about 469 ns, `1` about 781 ns) did not improve the failing 32-bit transport and was reverted to FastLED's standard `TIMING_WS2812_800KHZ`.

A controlled A/B test subsequently separated format from frame termination:

- upstream 32-bit packing plus a full reset-low DMA tail and explicit `tx_start = 0` at EOF retained the original corruption (two states correct, the third wrong);
- direct 16-bit packing/FIFO/routing with upstream's original one-shot termination, no added reset samples, and no explicit EOF stop was visually correct and stable.

This falsifies the stale-FIFO/reset-tail hypothesis for the observed corruption and demonstrates that the 16-bit format changes are sufficient on the tested ESP32-D0WD-V3. It does not establish why other reported hardware-loopback tests observed valid 32-bit output; that discrepancy remains the focused upstream question.

The fix is persisted by `patchFastLedI2s.py`, registered as a PlatformIO pre-script. The hook is idempotent and fails closed if the pinned FastLED source no longer matches its expected input. To verify that the result did not depend on a modified `.pio/libdeps` tree, the complete FastLED dependency directory was deleted and `pio run -e lolin_d32_pro` was run again. PlatformIO downloaded commit `448f6ee2e77fa22b856acc2794bca6c6811c905b`, the hook reported `Applied classic ESP32 FastLED I2S 16-bit format fix`, and the build succeeded. A second build succeeded with the same message, proving idempotence. Inspection of the regenerated source confirmed the 16-bit sample mode, FIFO mode, `DATA_OUT8..23` mapping, and direct wave8 output. Reset buffers and the EOF stop were removed after the controlled A/B test showed they were unnecessary for correct output.

The reduced clean-hook-generated firmware was uploaded successfully after the A/B test and booted without a Guru Meditation panic. It initialized the I2S channel, mounted SPI SD, detected the MFRC522, connected Wi-Fi, and started the HTTP server. The visually confirmed format-only experiment used the same four transport changes as this reduced hook. Audio playback, repeated RFID reads, OTA behavior, timing/heap instrumentation, and a sustained stress run remain outstanding.

## Static measurements

PlatformIO's total image size is the meaningful flash measurement below. `firmware.bin` is also listed because the requested matrix calls for the physical artifact size; its padding means it is slightly larger than the reported image size.

| Build | `firmware.bin` | Total image | `.flash.text` | `.flash.rodata` | `.dram0.data` | `.dram0.bss` | Reported internal RAM |
|---|---:|---:|---:|---:|---:|---:|---:|
| A, D32 clockless-SPI | 3,554,432 | 3,554,191 | 2,130,236 | 1,282,732 | 27,508 | 26,424 | 53,932 |
| C1, D32 FLEX_IO/I2S1, 16-bit fix | 3,766,704 | 3,766,451 | 2,180,212 | 1,445,312 | 27,364 | 27,752 | 55,116 |
| B, S3 NeoPixelBus LCD/GDMA | 3,042,480 | 3,042,323 | 1,679,932 | 1,248,044 | 23,292 | 23,784 | 47,076 |
| C3, S3 FastLED LCD/GDMA | 3,305,600 | 3,305,443 | 1,766,892 | 1,422,944 | 23,804 | 25,480 | 49,284 |

| Comparison | Total-image delta | Internal-RAM delta |
|---|---:|---:|
| C1 FLEX_IO vs A clockless-SPI, classic ESP32 | +212,260 | +1,184 |
| C3 FastLED FLEX_IO vs B NeoPixelBus, S3 | +263,120 | +2,208 |

These deltas compare identical target profiles and toolchains within each row pair. Do not compare absolute D32 and S3 sizes against each other; their features, targets, and platform revisions differ. The candidate is not smaller statically at this pinned FastLED revision.

The maintainer SDMMC candidate also built successfully with the 16-bit fix: `firmware.bin` 3,770,432 bytes, total image 3,770,191 bytes, and 55,620 bytes reported internal RAM.

## Link evidence

Candidate map files contain:

- D32 SPI-SD and SDMMC: `ChannelEngineI2sEsp32Dev` and `createI2sEsp32DevEngine`;
- S3: `ChannelDriverLcdClockless` and `createLcdClocklessEngine`.

Candidate map files contain no `NeoPixelBus` or `SpiStripWs2812` matches. Baseline maps show the expected contrast: the classic baseline links `SpiStripWs2812`, while the S3 baseline links NeoPixelBus objects.

The first successful `FastLED.show()` also emits one diagnostic with the selected bus, device, engine name, LED count, and GPIO. This was captured successfully on the classic ESP32 hardware run; it still needs capture on S3.

## Implementation notes

- FastLED is pinned to exact commit `448f6ee2e77fa22b856acc2794bca6c6811c905b`; no floating branch is used.
- `patchFastLedI2s.py` applies the classic ESP32 16-bit I2S correction after dependency resolution. Exact-match guards make a future incompatible FastLED update fail during the build rather than silently restoring the broken format.
- The old `FASTLED_ESP32_USE_CLOCKLESS_SPI` override is removed.
- The modern Channel API is used deliberately. At the pinned revision, legacy `addLeds` controllers pre-bind their platform-default driver, so an exclusive-manager selection alone is not sufficient to guarantee rerouting.
- FastLED remains responsible for the CRGB buffer, brightness, `TypicalSMD5050` correction, dithering, refresh limiting, clear, show, animations, and OTA progress.
- FLEX_IO is enabled only for classic ESP32 and ESP32-S3. Other targets retain the normal `addLeds` path.
- ESPuino currently configures `CHIPSET` as `WS2812B`. The FLEX_IO channel uses FastLED's `TIMING_WS2812_800KHZ`, with a compile-time assertion that rejects another chipset until an explicit timing mapping is added.
- FastLED and an ESPuino playback header both define `BUSY`; the FastLED include is wrapped with `push_macro`/`pop_macro` to prevent that collision.
- The candidate FastLED introduces a global `round` overload that collides with the Wi-Fi scan call, so that call is qualified as `std::round`.

## Commands

Clean builds were run with:

```text
pio run -e lolin_d32_pro -t clean
pio run -e lolin_d32_pro
pio run -e lolin_d32_pro_sdmmc_pe -t clean
pio run -e lolin_d32_pro_sdmmc_pe
pio run -e esp32-s3-devkitc-1 -t clean
pio run -e esp32-s3-devkitc-1
```

The active checkout contains ignored S3-only `platformio-override.ini` and `src/settings-override.h` files. Their deliberate `HAL=99` assertion prevents classic environments from building there, so the two classic builds were rerun in the isolated D32 worktree and the S3 build was rerun in the active checkout. All three completed successfully. The active-checkout D32 failure is profile contamination, not an LED or FastLED compile failure.

PlatformIO regenerates the project-wide `dependencies.lock` for the last target built; it therefore cannot represent the classic ESP32 and S3 component graphs simultaneously. The build-specific dependency revisions recorded above came from each environment's resolved `.pio/libdeps` tree and build output.

ELF sections were read with the matching `xtensa-esp32-elf-size -A` or `xtensa-esp32s3-elf-size -A` tool from PlatformIO's toolchain package. Linked driver names were checked in each generated `firmware.map`.

## Remaining hardware validation

Run classic ESP32 and S3 independently. A passing classic result does not establish S3 stability.

1. Capture the complete S3 boot log and confirm the one-shot diagnostic names LCD clockless/LCD_CAM. Classic ESP32 first-frame I2S initialization and basic visual behavior are complete.
2. On both targets, check the ring's full color order and brightness range, correction, clear, all normal animations, LED-count restart behavior, and OTA progress. Basic classic ESP32 animation and SD/RFID boot coexistence are complete.
3. Record heap at T0 before the CRGB allocation, T1 after it, T2 after channel creation, T3 after first show, and T4 after stress. Separate internal 8-bit, DMA-capable, largest DMA block, minimum-ever DMA, PSRAM, and task stack high-water values.
4. Stress for at least 30 minutes with continuous SD audio, normal 200 Hz-capped LED updates, repeated RFID reads, Wi-Fi/web transfer, NVS writes, and OTA/flash activity.
5. Record audio underruns or audible glitches, RFID failures/latency, SD/web throughput, watchdogs/asserts, LED corruption, show latency, and any heap degradation.
6. Perform a longer soak before treating either target as fully production-validated.

The tested classic ESP32 path has passed focused visual and subsystem-coexistence validation. The remaining checks above are required for a full production/stress verdict, and no hardware claim is made yet for ESP32-S3.
