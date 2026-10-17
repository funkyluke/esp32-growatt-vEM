# ESP32_growatt_vEM

Virtual energy meter for a **Growatt MIC** inverter on the
[Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm)
board. It reads a Shelly 3EM over WiFi and answers the inverter's meter
queries on RS485 as an Eastron SDM120 (or Chint DDSU666), shifting the
reported power so that the inverter exports at most a configurable limit
(default 800 W) into the grid.

## How the export limit works

```
grid      = L1 + L2 + L3 from the Shelly       (+ import / - export)
reported  = grid + (limit - margin)            (sent to the inverter)
inverter export limit = 0 W                    (keeps reported >= 0)
=> grid >= -(limit - margin)                   (at most `limit` W exported)
```

- Set the inverter's own export limitation to **on, 0 W / 0 %**, meter type
  matching *Meter emulation -> Emulated meter*. All tuning then happens on
  the ESP (*Export limit* settings, live, no reboot).
- **Fail-safe:** the inverter only gets real values while the Shelly data is
  *fresh* (younger than max(5 s, 2.5 polls)). Otherwise it is told about a
  large export (5 kW) so it throttles, or, with *Stop answering*, gets no
  reply and its own meter-loss behaviour applies. Use a poll interval of 1-2 s.
- The three phases are summed into one virtual single-phase meter, so it
  does not matter which phase the MIC feeds.
- Not verified on a real MIC yet: which registers it reads and the sign
  convention. Use *Mode: Listen only* first and check `GET /api/modbus`
  (every frame is logged, nothing is transmitted). *Invert power sign*
  exists in case the inverter reacts the wrong way round.

### Wiring

ESP32 board RS485 **A/B** to the inverter's meter RS485 **A/B** (see the
MIC manual, "export limitation / meter"). Bus settings: address 1, 9600
baud, 8N1 (adjustable). Enable the board's 120 ohm termination jumper; the
ESP is at the end of the bus.

## Hardware pin map

| Function      | GPIO | Notes                          |
|---------------|------|--------------------------------|
| RS485 TX (UART) | 17 | UART TXD                       |
| RS485 RX (UART) | 18 | UART RXD                       |
| RS485 EN (DE/RE) | 21 | Driven by the UART as its RTS pin (half-duplex mode) |

The RS485 interface is electrically isolated on-board. The 120 Ω termination
resistor is **not** populated by default (enable via the on-board jumper when
it is the bus end node). The board's CAN interface (GPIO15 TX / GPIO16 RX) is
not used by this firmware.

## Prerequisites (Windows)

1. **ESP-IDF v6.1** installed via the Espressif installer / VS Code ESP-IDF
   extension. Here it lives in `D:\ESP\.espressif\v6.1\esp-idf` (see
   `.vscode/settings.json`, `idf.currentSetup`).
2. USB-C cable to the board's native USB port (enumerates as a COM port; the
   chip's USB-Serial/JTAG is used for flashing and the console).
3. Do **not** power the board from the screw terminal and USB at the same time.

## Build and flash

Open the **ESP-IDF PowerShell** (or load the environment manually) and run:

```powershell
# only needed if PowerShell blocks scripts (affects this session only):
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

$env:IDF_TOOLS_PATH = 'D:\ESP\.espressif'
. D:\ESP\.espressif\v6.1\esp-idf\export.ps1

cd D:\git\ESP32_growatt_vEM
idf.py set-target esp32s3     # first time only (regenerates sdkconfig)
idf.py build
idf.py -p COM10 flash monitor # use your COM port; exit monitor with Ctrl+]
```

If flashing fails or the port keeps disappearing, hold **BOOT**, tap **RESET**,
then release **BOOT** to enter download mode.

### IDF 6.1 notes (why the code looks the way it does)

- Components must be listed explicitly in `main/CMakeLists.txt`
  (`esp_driver_uart`, ...). The old catch-all `driver` include path is not
  enough.
- `uart_set_rs485_*` helpers no longer exist. RS485 direction control is the
  UART's RTS pin: `uart_set_pin(..., rts = GPIO21, ...)` plus
  `UART_MODE_RS485_HALF_DUPLEX`.
- If the Python/IDF output looks garbled or empty when redirected, set
  `$env:PYTHONUTF8 = '1'` first.
- After changing Python/IDF versions, delete `build/` and `sdkconfig` and run
  `idf.py set-target esp32s3` again.

## WiFi configuration

WiFi credentials are kept in `main/wifi_credentials.h`, which is **gitignored**.
To set them up:

```sh
cp main/wifi_credentials.h.example main/wifi_credentials.h
# then edit main/wifi_credentials.h and fill in WIFI_SSID / WIFI_PASSWORD
```

The device joins your LAN in STA mode and retries forever if the AP goes away.
On boot it prints the assigned IP (look for `got IP` in the monitor), then
browse to `http://<ip>`.

**Requirements:** the ESP32 only supports **2.4 GHz** WiFi (no 5 GHz), and the
SSID/password must match exactly (case-sensitive).

Troubleshooting from the monitor log (`WiFi disconnected (reason N)`):

| Reason | Meaning |
|--------|---------|
| 201 | `NO_AP_FOUND`: SSID not visible. Wrong name, 5 GHz only, hidden SSID, or out of range |
| 15, 202, 204 | Handshake / authentication failure. Usually a wrong password |

The web server exposes:

- `GET /` — live 3-phase power dashboard
- `GET /api/power` — JSON with per-phase power/current/voltage and the total
- `GET /api/status` — JSON `{"ip": "...", "wifi": "..."}`
- `GET /api/info` — firmware version, reset reason, heap, RSSI
- `GET /api/modbus` — Modbus counters and the last 24 frames (hex, decoded)
- `GET /api/history?res=s|m&since=<seq>` — power history as columns
  (`grid`, `l1`-`l3`, `rep` = reported to the inverter, `lim`, `fl` flags;
  1 min data adds `gmin`/`gmax`). `next` is the `since` for the next call.

## Live graph

The dashboard graphs grid power, the power reported to the inverter and the
export limit: 1 s resolution for the last hour (15 min / 1 h), 1 min
averages for 7 days (6 h / 24 h / 7 d). Periods without fresh meter data are
shaded. The history lives in PSRAM (about 230 KB) and starts empty after a
reboot. The chart uses [uPlot](https://github.com/leeoniya/uPlot) 1.6.31
(MIT, `main/www/vendor/`), embedded in the firmware so it works offline.
- `POST /api/ota` — firmware upload, `POST /api/reboot`

## Shelly 3EM (Gen1) power meter

The firmware polls a Shelly **3EM Gen1** (not the Pro) and shows L1/L2/L3 power.

```sh
cp main/shelly_config.h.example main/shelly_config.h
# set SHELLY_DEVICE_ID (and optionally SHELLY_FALLBACK_IP)
```

- **Discovery:** mDNS, hostname `shellyem3-<DEVICE_ID>` (the library appends
  `.local` itself, so do not include it). `SHELLY_FALLBACK_IP` is used if mDNS
  fails. Resolution can take 20-30 s after boot.
- **Polling:** one `GET /status` every 2 s (the `emeters[0..2]` array holds the
  three phases). One request instead of three `/emeter/N` calls, because the
  Shelly has very few sockets.
- **Components:** `espressif/cjson` and `espressif/mdns` are declared in
  `main/idf_component.yml`. `idf.py add-dependency` only edits the manifest;
  run `idf.py reconfigure` (or let the next build re-run CMake) to download
  them into `managed_components/`.
- **Partition table:** `partitions.csv` gives the app 4 MB; the default 1 MB
  partition was 97 % full once WiFi + HTTP + mDNS were linked in.

### Reliability notes (found the hard way)

- A pure TCP-connect timeout to the Shelly while the PC reaches it in 5 ms was
  **not** AP isolation. Contributing causes were the WiFi event handler
  sleeping inside the system event loop (stalling netif/mDNS events), modem
  power-save, and three separate HTTP connections per poll. All three are fixed
  (`esp_wifi_set_ps(WIFI_PS_NONE)`, no blocking in the handler, one request).
- This network has several APs on one SSID. The firmware scans all channels
  and joins the strongest instead of the first (`WIFI_ALL_CHANNEL_SCAN` +
  `WIFI_CONNECT_AP_BY_SIGNAL`); the weak AP caused auth/assoc failures at boot.
- One missed poll does not flip the dashboard to offline: data freshness is
  computed on the device from the age of the last good poll.

### Settings page

`http://<ip>/settings` edits the WiFi and Shelly settings. They are stored in
NVS and override the compile-time defaults in `wifi_credentials.h` /
`shelly_config.h`, which are only used on the first boot.

- Settings are grouped into collapsible categories (WiFi, Shelly 3EM).
- The WiFi password is never sent back to the browser; leave it empty to keep it.
- Changing the Shelly settings re-discovers the meter immediately.
- Changing WiFi switches network. The new credentials are saved only after the
  device gets an IP with them; if that fails within 30 s the old ones are
  restored, so a typo cannot lock you out.
- API: `GET /api/settings` (field list + values), `POST /api/settings` (JSON with
  any subset of the keys).

To add a setting: add a member to `settings_t` in `settings.h` and one row to the
`SETTINGS[]` table in `settings.c` (category, key, label, limits, what to re-apply). The page,
validation and storage pick it up from the table.

### Freshness and uptime

`GET /api/power` includes `state`, `age_ms` and `uptime_s`:

| `state` | Age of last good poll | Dashboard |
|---------|-----------------------|-----------|
| `fresh` | under 5 s | green **FRESH** badge |
| `aging` | 5 to 15 s | amber **AGING** badge (one or two missed polls) |
| `stale` | over 15 s | red **STALE** badge, values dimmed |
| `none`  | no poll succeeded yet | grey **NO DATA** badge |

If the ESP32 itself stops answering, the page shows a red **UNREACHABLE** badge
and dims the values; it recovers on its own. `ok` is `true` for `fresh` and
`aging`. `uptime_s` is the ESP32 time since boot, shown as `2m 22s` / `1d 3h`.

## Firmware updates (OTA)

The partition table has two 4 MB app slots (`ota_0`/`ota_1`). After **one**
USB flash with this table (`idf.py -p COMx flash`), later updates can be
uploaded on the settings page under *Firmware & system*
(`build/ESP32_growatt_vEM.bin`), or with
`curl -u admin:<pw> -H "Content-Type: application/octet-stream" --data-binary @build/ESP32_growatt_vEM.bin http://<ip>/api/ota`.

A new image boots in "pending verify" state and confirms itself after 60 s.
If it crashes or is reset before that, the bootloader rolls back to the
previous image. NVS (settings) is at the same offset as before, so settings
survive the switch to the OTA table.

## Web password

*Settings -> System -> Web password* protects the settings page, the
settings API, reboot and OTA with HTTP Basic auth (user `admin`). The
dashboard and `/api/power` stay public. POST endpoints require a JSON (or
octet-stream) content type, which blocks cross-site form posts.

## Debugging helpers

`tools/serial_capture.py` resets the chip and writes N seconds of console
output to a UTF-8 file. Close the ESP-IDF Monitor first (it holds the port):

```powershell
python tools\serial_capture.py COM10 60 serial.log
```

## Structure

```
.
├── CMakeLists.txt          # top-level project
├── partitions.csv          # NVS + 2 x 4 MB OTA app slots
├── sdkconfig.defaults      # ESP32-S3 / PSRAM / flash defaults
├── tools/serial_capture.py # console capture helper
├── test/host/              # host unit tests (make -C test/host, run in CI)
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml   # managed components (cjson, mdns)
    ├── main.c              # startup
    ├── settings.c/.h       # table-driven settings (NVS, JSON, validation)
    ├── wifi.c/.h           # STA connection, safe WiFi switching
    ├── shelly.c/.h         # Shelly 3EM polling, freshness state
    ├── web.c/.h            # HTTP server, API, auth
    ├── ota.c/.h            # firmware upload, rollback confirmation
    ├── modbus.c/.h         # RS485 UART task, meter emulation, frame log
    ├── mb_proto.c/.h       # Modbus RTU + SDM120/DDSU666 register maps (pure C)
    ├── limiter.c/.h        # export limit offset + fail-safe (pure C)
    ├── vem.c/.h            # combines Shelly data, limiter, meter values
    ├── history.c/.h        # 1 s / 1 min power history for the graph
    ├── www/                # embedded HTML pages
    ├── wifi_credentials.h          # (gitignored) your LAN credentials
    ├── wifi_credentials.h.example  # committed template
    ├── shelly_config.h             # (gitignored) Shelly device ID / fallback IP
    └── shelly_config.h.example     # committed template
```