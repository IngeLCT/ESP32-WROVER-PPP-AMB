# ESP32-WROVER Air Monitoring

Firmware for the LilyGO TTGO T-A7670 (ESP32 WROVER + SIMCom A7670) that collects air-quality metrics and batches them to Firebase. The original PPPoS cellular stack has been removed so you can integrate the connectivity layer of your choice (Wi-Fi, Ethernet, custom modem driver, etc.).

## Features

- Acquisition of particulate matter, VOC, NOx, temperature, humidity and CO₂ readings from the sensor stack.
- Periodic upload of aggregated samples to Firebase Realtime Database with automatic trimming of old entries.
- Optional geolocation via UnwiredLabs (call the helper functions once you provide cell data).
- SNTP time synchronisation helper (`init_sntp_and_time`) that you can keep using once network access is available.

## Prerequisites

- ESP-IDF v5.5.1 (matching the version pinned in this project).
- LilyGO TTGO T-A7670 board (ESP32-WROVER + SIMCom A7670 modem) wired as in the original design.
- Sensor suite supported by `main/sensors.h` (SPS/SGP/SCx stack from the project).
- Internet connectivity supplied externally (Wi-Fi station, Ethernet, custom modem driver, etc.).

## Getting Started

1. **Clone the repository**
   ```bash
   git clone https://github.com/IngeLCT/ESP32-WROVER-PPP-AMB.git
   cd ESP32-WROVER-PPP-AMB
   ```

2. **Create your secrets file**
   - Copy `main/Privado.example.h` to `main/Privado.h` and fill in your Firebase credentials and UnwiredLabs token. `Privado.h` is git-ignored.

3. **Provide connectivity**
   - Integrate your preferred network stack and ensure `firebase_init()` can reach the internet before starting the sensor task. The application no longer powers on or configures the cellular modem.

4. **Configure build options (optional)**
   - Run `idf.py menuconfig` to adjust sensor options or other project settings.

5. **Build and flash**
   ```bash
   idf.py build
   idf.py flash monitor
   ```

## Project Structure

- `main/` – Application entry point, sensor handling, Firebase integration, helpers for SNTP and UnwiredLabs.
- `components/` – Third-party/shared components (Firebase client, JSON handling, etc.).
- `sdkconfig` – Generated configuration (not tracked; regenerate with `idf.py menuconfig`).
- `build/` – Build artifacts (ignored).

## Notes

- Because PPP support has been removed, all logs referring to the modem will now simply remind you to provide connectivity yourself.
- The helper `unwiredlabs_geolocate` is still available; call it when you have MCC/MNC/TAC/CID data from your new transport and handle the result as before.
- If you do not need geolocation or SNTP, feel free to strip those helpers from `main/main.c`.

## License

This project is released under the MIT License. See [`LICENSE`](LICENSE).
