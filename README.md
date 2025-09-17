# ESP32-WROVER PPP Air Monitoring

Firmware for the LilyGO TTGO T-A7670 (ESP32 WROVER + SIMCom A7670) that collects air-quality metrics and pushes batches to Firebase over a PPP data link. The application also obtains coarse location via UnwiredLabs to tag measurements and synchronises time using SNTP once the cellular link is up.

## Features

- Power-on sequence and PPPoS session management for the SIMCom A7670 GSM/LTE modem.
- Batch acquisition of particulate matter, VOC, NOx, temperature, humidity and CO₂ readings from the connected sensor stack.
- Periodic upload of aggregated samples to Firebase Realtime Database with automatic retention trimming.
- Optional geolocation via UnwiredLabs to enrich telemetry metadata.
- SNTP time synchronisation once PPP is established to keep timestamps consistent.

## Prerequisites

- ESP-IDF v5.5.1 (matching the version pinned in the project).
- LilyGO TTGO T-A7670 board (ESP32-WROVER + SIMCom A7670 modem) wired as defined in `main/ppp_gsm_config.h`.
- Sensors supported by `main/sensors.h` (SPS/SGP/SCx stack from the original design).
- An active SIM with data plan and APN credentials. The default configuration targets Telcel (Mexico):
  - APN: `internet.itelcel.com`
  - User: `webgprs`
  - Password: `webgprs2002`

## Getting Started

1. **Clone the repository**
   ```bash
   git clone https://github.com/IngeLCT/ESP32-WROVER-PPP-AMB.git
   cd ESP32-WROVER-PPP-AMB
   ```

2. **Create your secrets file**
   - Copy `main/Privado.example.h` to `main/Privado.h` and fill in your Firebase credentials and UnwiredLabs token. The real file is ignored by Git to protect secrets.

3. **Configure the project (optional)**
   - Run `idf.py menuconfig` if you need to tweak GPIO mappings, UART baud rate, PPP settings or sensor options.
   - Remember that the modem control pins and APN defaults live in `main/ppp_gsm_config.h`.

4. **Build and flash**
   ```bash
   idf.py build
   idf.py flash monitor
   ```
   The monitor will display the PPP negotiation log. If the link does not reach the `RUNNING` phase, enable the PPP debug output under `Component config → LWIP` for deeper diagnostics.

5. **Deploy**
   - Once the firmware is stable, use `idf.py flash` on any machine with the correct toolchain and a populated `Privado.h`.

## Project Structure

- `main/` – Application entry point, PPP modem driver (`ppp_gsm.c`), sensor handling and Firebase integration.
- `components/` – Third-party or shared components (Firebase client, JSON handling, etc.).
- `sdkconfig` – Generated configuration file (not tracked; regenerate with `idf.py menuconfig`).
- `build/` – Build artifacts (ignored).

## Contributing & Maintenance Tips

- Secrets: never commit `main/Privado.h`. Share only via secure channels and regenerate API keys if they leak.
- When updating ESP-IDF, review PPP API changes (e.g., callback names) and retest the modem bring-up sequence.
- Use `idf.py fullclean` if you switch ESP-IDF versions or see inconsistent build behaviour.
- For troubleshooting PPP, capture monitor logs with `PPP_NOTIFY_PHASE` and `PPP debug log output` enabled; include those when reporting issues.

## License

This project is released under the MIT License. See [`LICENSE`](LICENSE).
