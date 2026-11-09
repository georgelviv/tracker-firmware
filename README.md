# Stezhka firmware

ESP-IDF v6.0 firmware for the **LilyGO T-A7670G R2 with L76K GPS** (ESP32 + SIMCom
A7670G-LLSE modem + Quectel L76K GNSS module).

The A7670G modem has no GNSS of its own. Position comes from the L76K, which is
wired straight to the ESP32 (UART2: RX GPIO22, TX GPIO21, 9600 baud; WAKEUP GPIO19).
Every _N_ seconds the firmware takes the latest NMEA fix and writes it to Firebase
Realtime Database over Wi-Fi. The modem is powered up but not used yet; it is kept for LTE.

## Layout

| File | What it does |
|---|---|
| `main/board.h` | Pin map for the T-A7670G + L76K board |
| `main/modem.c` | Modem UART link, power-on sequence, `modem_at()` helper (for future LTE) |
| `main/gnss.c` | L76K reader: NMEA `RMC`/`GGA` parsing with checksum validation |
| `main/wifi.c` | Wi-Fi station with auto-reconnect |
| `main/firebase.c` | Firebase Auth sign-in (e-mail/password) and a REST `PUT` to RTDB |
| `main/app_main.c` | Main loop |

## Data written

Each fix overwrites the device's latest position. No history is stored.

```
/devices/<deviceId>/latest = { lat, lon, alt, speed, course, hdop, sats, ts, receivedAt, via }
```

`ts` is the GNSS UTC time in epoch ms. `receivedAt` is the Firebase server timestamp.

## Build and flash

```bash
source ~/.espressif/tools/activate_idf_v6.0.sh
idf.py menuconfig        # Tracker configuration -> Wi-Fi, Firebase, device ID
idf.py build flash monitor
```

Credentials live in `sdkconfig`, which is git-ignored. Never commit it.

Put the GPS antenna on the L76K's connector, not the modem's, and give it sky view.
The first fix usually takes 30 s to a few minutes.

`sdkconfig.defaults` enables `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`.
Google serves GTS Root R1/R4 cross-signed by the retired "GlobalSign Root CA", which is no
longer in the bundle. Without this option, TLS to Firebase fails with "No matching trusted root certificate".

## Firebase setup

Positions show up on the map at **https://stezhka-tracker.web.app/**.

Firebase setup is described in the [tracker-ui README](https://github.com/georgelviv/tracker-ui#one-time-firebase-setup).
The device needs its own Firebase Auth user, and an entry in `/deviceWriters/<deviceId>` holding that user's UID.

## Roadmap

- [ ] Position history (track)
- [ ] Buffer fixes in NVS/SD while offline and flush later
- [ ] LTE data via PPP (`esp_modem`): `firebase.c` stays as is because it only needs an IP link
- [ ] SMS fallback
- [ ] Battery voltage (GPIO35) and light sleep between fixes
