# ESP32_growatt_vEM

Virtual energy meter for a **Growatt MIC** inverter on the
[Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/esp32-s3-rs485-can.htm)
board. It reads a **Shelly 3EM (Gen1)** over WiFi and answers the inverter's
meter queries on RS485 as an **Eastron SDM120** (or Chint DDSU666), shifting
the reported power so that the inverter exports **at most a configurable
limit (default 800 W)** into the grid.

Also: live dashboard with history graph, MQTT with Home Assistant discovery,
Telegram alerts and a daily report, OTA updates with rollback.

- [How the export limit works](#how-the-export-limit-works)
- [Setup](#setup): hardware, build and flash, first start
- [Web interface and API](#web-interface-and-api)
- [MQTT / Home Assistant](#mqtt--home-assistant)
- [Telegram](#telegram)
- [Reliability](#reliability)
- [Development](#development)

## How the export limit works

```
grid      = L1 + L2 + L3 from the Shelly       (+ import / - export)
reported  = grid + (limit - margin)            (sent to the inverter)
inverter export limit = 0 W                    (keeps reported >= 0)
=> grid >= -(limit - margin)                   (at most `limit` W exported)
```

- Set the inverter's own export limitation to **on, 0 W / 0 %**, with the
  meter type matching *Meter emulation -> Emulated meter*. All tuning then
  happens on the ESP (*Export limit* settings or the Home Assistant number
  entity), live, no reboot.
- **Fail-safe:** the inverter only gets real values while the Shelly data is
  *fresh* (younger than max(5 s, 2.5 polls)). Otherwise it is told about a
  large export (5 kW) so it throttles, or, with *Stop answering*, gets no
  reply and its own meter-loss behaviour applies. Use a Shelly poll interval
  of 1-2 s.
- The three phases are summed into one virtual single-phase meter, so it does
  not matter which phase the MIC feeds.
- *Limit export: off* reports the real grid power (the inverter's own limit
  then applies).

> **Not verified on a real MIC yet:** which registers it reads and the sign
> convention it expects. For the first connection use *Mode: Listen only*
> and watch `/diag` (every frame is logged, nothing is transmitted). Then
> switch to *Emulate meter* and check that the export settles below the
> limit. *Invert power sign* exists in case the inverter reacts the wrong way
> round; *Chint DDSU666* in case the SDM120 is not accepted.

## Setup

### Hardware

| Function         | GPIO | Notes |
|------------------|------|-------|
| RS485 TX (UART1) | 17   | |
| RS485 RX (UART1) | 18   | |
| RS485 EN (DE/RE) | 21   | driven by the UART as its RTS pin (half-duplex mode) |

- Board RS485 **A/B** to the inverter's meter RS485 **A/B** (see the MIC
  manual, "export limitation / meter"). Bus: address 1, 9600 baud, 8N1
  (adjustable).
- The RS485 port is isolated on the board. Its 120 ohm termination is **not**
  fitted by default; enable the jumper, the ESP is at the end of the bus.
- The CAN interface (GPIO15/16) is not used.
- Do **not** power the board from the screw terminal and USB at the same time.

### Build and flash (Windows, ESP-IDF v6.1)

Copy the two local config templates (both gitignored) and fill them in. They
are only factory defaults: everything can be changed later on the settings
page.

```sh
cp main/wifi_credentials.h.example main/wifi_credentials.h   # WIFI_SSID / WIFI_PASSWORD
cp main/shelly_config.h.example main/shelly_config.h         # SHELLY_DEVICE_ID
```

In the **ESP-IDF PowerShell** (or load the environment manually):

```powershell
# only if PowerShell blocks scripts (this session only):
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

$env:IDF_TOOLS_PATH = 'D:\ESP\.espressif'
. D:\ESP\.espressif\v6.1\esp-idf\export.ps1

cd D:\git\ESP32_growatt_vEM
idf.py set-target esp32s3     # first time only (regenerates sdkconfig)
idf.py build
idf.py -p COM10 flash monitor # your COM port; exit the monitor with Ctrl+]
```

If flashing fails or the port keeps disappearing, hold **BOOT**, tap
**RESET**, release **BOOT** (download mode).

After the first USB flash, updates go over WiFi (see [OTA](#firmware-updates-ota)).

### First start

- The device joins the WiFi (2.4 GHz only) and is reachable as
  **http://growatt-vem.local/** (mDNS) or by its IP (`got IP` in the log).
- If it gets no IP for 2 minutes it opens a **fallback access point**
  `GrowattVEM-XXXX` (password `growattvem`) with the web UI at
  **http://192.168.4.1/**, while it keeps retrying the configured network.
  The AP closes 1 minute after the network is back.
- WiFi disconnect reasons in the log: `201` = SSID not found (name, 5 GHz,
  range), `15` / `202` / `204` = authentication failed (password).
- Several APs on one SSID: the strongest is joined. `/diag` shows the current
  AP (BSSID, channel, RSSI); *WiFi -> Lock to access point* pins one if the
  automatic choice is a bad one (e.g. a repeater with a weak uplink).

## Web interface and API

| Page | |
|------|--|
| `/` | dashboard: L1-L3, grid power, energy counters, power reported to the inverter, limiter state, inverter polling, **live graph** |
| `/settings` | all settings, Telegram test, firmware upload, reboot |
| `/diag` | firmware, reset reason, memory, WiFi, time, MQTT/Telegram status, Modbus counters and the last 24 frames |

### Settings

Settings are stored in NVS; the header files above are only used on the very
first boot. Categories: WiFi, Shelly 3EM, Meter emulation, Export limit,
MQTT, Telegram, System.

- Secrets (passwords, bot token) are never sent back; leave empty to keep,
  tick *clear* to remove.
- Changing WiFi switches network; the new credentials are saved only once the
  device gets an IP with them, otherwise the old ones return after 30 s.
- Changing the Shelly ID/IP re-discovers the meter immediately.
- *System -> Web password* protects `/settings`, the settings API, reboot,
  OTA and the Telegram endpoints with HTTP Basic auth (user `admin`). The
  dashboard, `/diag` and the read-only APIs stay open.

To add a setting: a member in `settings_t` (`settings.h`) and one row in the
`SETTINGS[]` table (`settings.c`); page, validation and storage follow from
the table.

### Freshness

| `state` | Age of the last good Shelly poll | Dashboard | Inverter gets |
|---------|----------------------------------|-----------|---------------|
| `fresh` | < max(5 s, 2.5 polls)  | green **FRESH** | real values |
| `aging` | < max(15 s, 7.5 polls) | amber **AGING** | fail-safe |
| `stale` | older | red **STALE**, dimmed | fail-safe |
| `none`  | no poll succeeded yet | grey **NO DATA** | fail-safe |

### Live graph

Grid power, power reported to the inverter and the export limit (L1-L3 via
the legend): 1 s resolution for the last hour (15 min / 1 h), 1 min averages
for 7 days (6 h / 24 h / 7 d). Periods without fresh data are shaded. The
history lives in PSRAM (about 230 KB) and starts empty after a restart. The
chart is [uPlot](https://github.com/leeoniya/uPlot) 1.6.31 (MIT,
`main/www/vendor/`), embedded so it works without internet.

### API

| Endpoint | |
|----------|--|
| `GET /api/power` | Shelly values, freshness, energy counters, `limiter` (state, limit, offset, reported power), `inverter` (last poll) |
| `GET /api/history?res=s\|m&since=<seq>` | history as columns (`grid`, `l1`-`l3`, `rep`, `lim`, `fl`; 1 min data adds `gmin`/`gmax`); pass `next` as `since` for increments |
| `GET /api/info` | firmware, app slot, reset reason, memory, WiFi, time, MQTT/Telegram status |
| `GET /api/modbus` | Modbus counters and the last 24 frames |
| `GET /api/status` | IP and SSID |
| `GET /api/settings`, `POST /api/settings` | field list with values; JSON with any subset of the keys (`null` clears a secret) |
| `GET /api/telegram`, `POST /api/telegram/test` | last send result; queue a test message |
| `POST /api/ota`, `POST /api/reboot` | firmware upload (`application/octet-stream`), restart |

POST endpoints require a JSON (or octet-stream) content type, which blocks
cross-site form posts.

## MQTT / Home Assistant

*Settings -> MQTT*: broker URI (`mqtt://host:1883`, or `mqtts://` with a
public CA), user, password, base topic (default `growatt_vem`), publish
interval (default 10 s).

| Topic | Content |
|-------|---------|
| `<base>/status` | `online` / `offline` (retained, last will) |
| `<base>/state` | JSON: `grid_power`, `l1_power`..`l3_power`, `l1_voltage`..`l3_voltage`, `reported_power`, `import_energy`, `export_energy` (kWh, Shelly counters), `limiter`, `meter_data`, `inverter_polling`, `export_limit`, `limiter_enabled`, `rssi`, `uptime` |
| `<base>/limit/set` | command: export limit in W (0-5000), saved like a setting |
| `<base>/limiter/set` | command: `ON` / `OFF` |

With *Home Assistant discovery* on, a device "Growatt vEM" appears with all
sensors (the energy sensors work in the Energy dashboard), a **number**
entity for the export limit and a **switch** for the limiter. Discovery is
re-sent when HA restarts; turning discovery off removes the entities.
Commands are confirmed in the state within a second.

## Telegram

1. Create a bot with [@BotFather](https://t.me/BotFather) and copy the token.
2. Send your bot any message (bots cannot start a chat), then find your chat
   ID, e.g. with @userinfobot.
3. *Settings -> Telegram*: enable, token, chat ID, save, *Send test message*.
   The status line shows the result of the last send (`HTTP 401` = wrong
   token, `HTTP 400: chat not found` = wrong chat ID or no message sent to
   the bot yet).

Alerts (each with a recovery message; the delay is a setting, default 5 min):

| Alert | Condition | Default |
|-------|-----------|---------|
| Shelly data lost | no fresh meter data for longer than the delay | on |
| Export above the limit | export > limit + 50 W for 60 s while limiting | on |
| Unexpected restart | after a crash, watchdog reset or brownout | on |
| WiFi outage | sent after reconnecting, if longer than the delay | on |
| Inverter not polling | only inside a daytime window (default 10-16 h): the inverter switches off without PV | **off** |

The **daily report** (default 21:30 local time) covers the 24 h since the
previous one: grid import/export in kWh (difference of the Shelly energy
counters, baseline kept in NVS), peak import/export, time at the export
limit, time without fresh meter data, when the inverter was active, number
of alerts, uptime and WiFi signal. Peaks and times start over after a restart
(the report says so).

Time comes from SNTP (*System -> NTP server*); the time zone is a POSIX TZ
string (*System -> Time zone*, default Central Europe).

## Reliability

### Firmware updates (OTA)

Two 4 MB app slots (`ota_0`/`ota_1`). Upload `build/ESP32_growatt_vEM.bin`
on the settings page (*Firmware & system*) or with

```sh
curl -u admin:<pw> -H "Content-Type: application/octet-stream" \
     --data-binary @build/ESP32_growatt_vEM.bin http://growatt-vem.local/api/ota
```

A new image boots as "pending verify" and confirms itself after 60 s. If it
crashes or is reset before that, the bootloader rolls back to the previous
image (`/diag` shows the slot and whether it is confirmed).

### Watchdogs and self-healing

- Task watchdog (30 s, panics and restarts) on the Modbus and Shelly tasks.
- No Shelly data for 30 min while WiFi is up: restart (only if data was
  received since boot, so an offline Shelly cannot cause a restart loop).
- After 5 failed polls the Shelly address is resolved again (new DHCP
  lease); while on the fallback IP, mDNS is retried every 5 min.
- The fallback AP (see [First start](#first-start)) keeps the device
  reachable without WiFi.

### Lessons learned

- A TCP-connect timeout to the Shelly while the PC reached it in 5 ms was
  **not** AP isolation: the WiFi event handler slept inside the system event
  loop (stalling netif/mDNS), modem power-save was on, and three HTTP
  connections per poll exhausted the Shelly's sockets. Fixed by never
  blocking in the handler, `WIFI_PS_NONE`, and one `GET /status` per poll.
- Several APs share the SSID: the firmware scans all channels and joins the
  strongest (`WIFI_ALL_CHANNEL_SCAN` + `WIFI_CONNECT_AP_BY_SIGNAL`).
- One missed poll does not flip anything to offline: freshness is the age of
  the last good poll.
- esp_http_client returns `ESP_ERR_NOT_SUPPORTED` for an HTTP 401; check the
  status code before the error code.

## Development

### IDF 6.1 notes

- Components must be listed explicitly in `main/CMakeLists.txt`; esp-mqtt
  now comes from the component manager (`espressif/mqtt`). Resolved versions
  are pinned in the committed `dependencies.lock`.
- RS485 direction control is the UART's RTS pin (`uart_set_pin(..., rts)` +
  `UART_MODE_RS485_HALF_DUPLEX`); the old `uart_set_rs485_*` helpers are gone.
- If IDF output looks garbled when redirected, set `$env:PYTHONUTF8 = '1'`.
- `sdkconfig.defaults` only applies to options missing from `sdkconfig`:
  after changing it (or the IDF/Python version) delete `sdkconfig` (and
  `build/`) and run `idf.py set-target esp32s3` again.

### Tests and CI

The protocol and limiter code is plain C and has host unit tests with
independently computed frames:

```sh
make -C test/host        # needs gcc/clang; runs with ASan/UBSan
```

GitHub Actions runs them and builds the firmware on every push
(`.github/workflows/ci.yml`; the binary is attached as an artifact).

### Serial console

`tools/serial_capture.py` writes N seconds of console output to a UTF-8
file, optionally resetting the chip first (close the IDF monitor first):

```powershell
python tools\serial_capture.py COM10 60 serial.log --reset
```

### Structure

```
.
├── CMakeLists.txt, partitions.csv (NVS + 2 x 4 MB OTA slots), sdkconfig.defaults
├── dependencies.lock        # pinned component versions
├── .github/workflows/ci.yml # host tests + firmware build
├── test/host/               # unit tests for mb_proto + limiter
├── tools/serial_capture.py  # console capture helper
└── main/
    ├── main.c               # startup order
    ├── settings.c/.h        # table-driven settings (NVS, JSON, validation, change callbacks)
    ├── wifi.c/.h            # STA, safe WiFi switching, fallback AP, hostname
    ├── shelly.c/.h          # Shelly 3EM polling, freshness, mDNS
    ├── vem.c/.h             # one view: Shelly data + limiter + meter values
    ├── limiter.c/.h         # export limit offset + fail-safe (pure C)
    ├── mb_proto.c/.h        # Modbus RTU + SDM120/DDSU666 register maps (pure C)
    ├── modbus.c/.h          # RS485 task, meter emulation, frame log
    ├── history.c/.h         # 1 s / 1 min history for the graph
    ├── mqtt_ha.c/.h         # MQTT state/commands, HA discovery
    ├── telegram.c/.h        # Telegram send queue
    ├── monitor.c/.h         # alerts, daily report, self-restart
    ├── clock.c/.h           # SNTP + time zone
    ├── web.c/.h             # HTTP server, API, auth
    ├── ota.c/.h             # firmware upload, rollback confirmation
    ├── www/                 # embedded pages (+ vendor/uPlot)
    └── *.h.example          # templates for the gitignored local config
```
