# ⚠️ Proof of Concept – Passive Wi-Fi Sniffer

> This is a **proof-of-concept** project that demonstrates how to passively detect Wi-Fi clients near ESP32-C5 using promiscuous mode.\
> Use responsibly. In many jurisdictions, **sniffing Wi-Fi traffic** may require **authorization** or **consent** from network owners. This code is for **educational purposes only**.

---

## ESP32-C5 WiFi Client Sniffer

This project scans nearby Wi-Fi access points and detects how many clients are connected to each individual BSSID using an ESP32-C5 chip.\
It operates in a loop: scans visible APs, then switches into promiscuous (sniffer) mode and captures data frames to count clients per BSSID.

---

## 🔧 Requirements

- **ESP32-C5 DevKitC-1** board
- **ESP-IDF v5.5.0+** (master branch)
- **Python 3.11+**
- A serial terminal (e.g., `idf.py monitor` or `minicom`)

---

## 📦 Setup

1. Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/get-started/index.html) with version **5.5.0 or later** (required for ESP32-C5 support).
2. Set target to `esp32c5`:

```bash
idf.py --preview set-target esp32c5
```

3. Build and flash:

```bash
idf.py fullclean build flash monitor
```

Make sure the board is in download mode and connected to the correct COM port (`idf.py -p COMx ...`).

---

## 🔍 Advanced Wi-Fi Scanning

The sniffer now supports:

- ✅ **GPS Integration (ATGM336H):** If detected, each scan result includes geographic coordinates and GPS fix status
- ✅ **Client Detection per BSSID:** Promiscuous mode captures data packets to count connected Wi-Fi clients
- ✅ **Rolling Client History:** Optionally retain client MACs across scans
- ✅ **Signal Strength (RSSI):** Each AP shows real-time signal level
- ✅ **Security Type Detection:** Displays network encryption (WPA2, WPA3, OPEN)
- ✅ **Optional Sorting:** Sort by signal strength via `#define SORT_RESULTS_BY_RSSI 1`

Feature toggles in `main.c`:

```c
#define RETAIN_CLIENTS_HISTORY 1
#define SORT_RESULTS_BY_RSSI 1
#define MAX_APS 10
#define MAX_CLIENTS 10
#define SCAN_INTERVAL_SEC 60
```

Output example:

```
| SSID                      | Band | Chan  | RSSI   | Cli  | BSSID             | Security   | Latitude     | Longitude    | GPS Fix |
|---------------------------|------|-------|--------|------|-------------------|------------|--------------|--------------|---------|
| MyWiFi                    | 2.4G | 11    | -48    | 0    | XX:XX:XX:XX:XX:XX | WPA3       | 51.09572     | 16.98904     | OK      |
```

---

## 🚦 User Experience Improvements (May 2025)

- **Step-by-step status messages:** The device now prints clear progress messages during each major initialization phase (NVS, network, event loop, GPS detection, WiFi).
- **GPS detection reporting:** On every boot, the console clearly states whether GPS was detected or not (`GPS detected: YES` / `No GPS data detected - disabling GPS support`).
- **Dynamic scan table:** The scan results table **adapts automatically**—GPS columns are shown only if GPS is detected, so the output is always clean and relevant.
- **Next scan countdown:** After each scan cycle, the program prints `Next scan in 60 seconds...` so users know the device is running and not stalled.
- **Cleaner logs:** WiFi and PHY log levels are now set to `WARN` to minimize noisy output and keep the console readable.

#### Example output (with GPS not detected):

```
==== ESP32 WiFi Scanner Startup ====
Initializing NVS storage...
Initializing network interface...
Creating event loop...
Initializing GPS UART...
Detecting GPS module...
No GPS data detected - disabling GPS support
Initializing WiFi driver...
Starting WiFi scan task...

| SSID                      | Band | Chan  | RSSI   | Cli  | BSSID             | Security   |
|---------------------------|------|-------|--------|------|-------------------|------------|
| ExampleAP                 | 5G   | 128   | -51    | 2    | XX:XX:XX:XX:XX:XX | WPA2/WPA3  |
...
Memory: used 73336 / 285440 bytes (25.7% used)
Next scan in 60 seconds...
```

#### Example output (with GPS detected and fix):

```
==== ESP32 WiFi Scanner Startup ====
...
GPS detected: YES
...
| SSID      | Band | Chan | RSSI | Cli | BSSID             | Security | Latitude  | Longitude  | GPS Fix |
|-----------|------|------|------|-----|-------------------|----------|-----------|------------|---------|
| MyWiFi    | 2.4G | 11   | -48  | 0   | XX:XX:XX:XX:XX:XX | WPA3     | 51.09572  | 16.98904   | OK      |
...
Memory: used 73336 / 285440 bytes (25.7% used)
Next scan in 60 seconds...
```

**These improvements make it easy to understand what the device is doing at all times, and ensure the output is always clear and relevant to the hardware actually connected.**

---

## 🧠 How it works

1. Performs a Wi-Fi scan (active)
2. Collects SSID, RSSI, auth mode, channel, BSSID
3. For each AP:
   - Switches to its channel
   - Enters promiscuous mode
   - Captures `data` frames to detect unique clients
4. Prints results to console with live GPS data if available

---

## 📁 Structure

- `main.c` – main loop, GPS parsing, Wi-Fi scan, sniffer callback
- Uses ESP-IDF Wi-Fi APIs and `esp_wifi_set_promiscuous_rx_cb()`
- UART communication with GPS (NMEA protocol)

---

## 🧭 Optional: GPS Integration (ATGM336H)

This project supports **GPS position logging** via the **ATGM336H** module using **UART**.\
The ESP32-C5 communicates bidirectionally with the GPS module to obtain valid time and location data from NMEA sentences.

### 🔌 Wiring (ESP32-C5 DevKitC-1)

| GPS Pin | Connect to ESP32-C5    |
| ------- | ---------------------- |
| **TX**  | GPIO23 (to ESP32 RX)   |
| **RX**  | GPIO24 (from ESP32 TX) |
| **VCC** | 3.3V                   |
| **GND** | GND                    |

> ✅ **Both UART lines (TX and RX)** are required\
> ❌ **PPS is not used** or needed

### ⚙️ Features

- Auto-detects GPS module on startup
- Sends `$PMTK314,...` command to enable **RMC** and **GGA** sentences only
- Parses:
  - `$GPGGA` → **latitude, longitude, fix validity**
  - `$GPRMC` → **UTC time/date** → sets ESP32 system RTC
- GPS fix status shown as `OK` or `NOFIX` in table
- Compatible with any NMEA-based GPS module

### 💡 Notes

- UART baud: **9600**
- Fix may take up to 90s after cold boot
- GGA fix is required for valid position data

---

## 📍 Notes

- Max APs: `#define MAX_APS` (default: 10)
- Max clients per BSSID: `#define MAX_CLIENTS` (default: 10)
- Sniff duration per AP: `#define SNIFF_TIME_MS` (default: 3000 ms)
- Scan interval (full cycle): `#define SCAN_INTERVAL_SEC` (default: 60 sec)
- Client detection requires active traffic — idle clients won't be seen

---

## 🛠️ TODO

- [ ] Show results on SPI TFT (ST7735S)
- [ ] Rotary encoder to switch views
- [ ] Add CSV export via UART or SD
- [X] GPS support (ATGM336H)

---

**License:** Apache License 2.0\
See [`LICENSE`](LICENSE) for full terms.
