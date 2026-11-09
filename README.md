# button-boi

Prototype firmware for a battery-powered ESP32-S3 "timestamp button". Press it
when you take your medication, go to bed, etc.: the device wakes, writes a
timestamp to flash, flashes its LED, and goes back to deep sleep. Hold the
button for a few seconds to open a short Bluetooth LE window, then download
the log from a web page. No phone app needed.

Target board: **Unexpected Maker TinyS3**. Framework: **ESP-IDF 6.1** through
PlatformIO (`espressif32@8.0.0`).

## Usage

| Action | Result | LED (WS2812) |
|---|---|---|
| First power-on / reset | New *session* starts, `BOOT` record logged | white blip |
| Short press | `PRESS` record logged | green flash |
| Hold ≥ 3 s | Sync mode: advertise over BLE for 60 s | solid blue → let go → blinking blue |
| Client connected | Window stays open while connected (+5 s after) | cyan |
| Short press during sync | Still logged | green flash |
| Hold ≥ 3 s during sync | Leave sync mode early | off |

### Downloading the log

1. Open <https://haydenmc.github.io/button-boi/>, or serve the page locally
   with `cd web && python3 -m http.server 8000` and open
   <http://localhost:8000>. Use **Chrome or Edge** (desktop or Android). Web
   Bluetooth needs a secure context; `localhost` qualifies, and so does any
   HTTPS host such as GitHub Pages. On iOS, use the Bluefy browser. Firefox
   and Safari don't support Web Bluetooth.
   - **Linux:** Chromium ships Web Bluetooth behind a flag. Enable
     `chrome://flags/#enable-experimental-web-platform-features` and relaunch,
     then check that `chrome://bluetooth-internals` lists your adapter. A Flatpak
     Chromium also needs BlueZ access:
     `flatpak override --user --system-talk-name=org.bluez org.chromium.Chromium`.
2. Hold the button until the LED turns blue, then let go.
3. Click **Connect** and pick `button-boi-XXXX`. The page sets the device
   clock from your computer automatically.
4. Click **Download log**, then **Save CSV** if you want a file. Tick *only new
   since last download* to fetch just the records added since your last dump.

## Build, flash, test

```sh
pio run -e tinys3                 # build
pio run -e tinys3 -t upload       # flash (TinyS3 native USB)
pio device monitor                # serial log (drops when the chip deep sleeps)
pio test -e native                # host unit tests for the flash ring log
pio run -e tinys3 -t menuconfig   # change build-time options ("button-boi" menu)
```

If an upload doesn't connect, hold BOOT, tap RESET, release BOOT, then retry.
The chip spends most of its time in deep sleep, which drops the USB port.

## Build-time configuration

Every option is in `src/Kconfig.projbuild`, under the **button-boi** menu in
menuconfig. To keep changes in version control, add the `CONFIG_BB_*` lines to
`sdkconfig.defaults` and delete `sdkconfig.tinys3` so it gets regenerated.

| Option | Default | Notes |
|---|---|---|
| `BB_BUTTON_GPIO` | 0 | Must be an RTC GPIO (0–21). 0 is the TinyS3 BOOT button |
| `BB_BUTTON_ACTIVE_LOW` | y | Button to GND. Disable for a button to 3V3 |
| `BB_BUTTON_INTERNAL_PULL` | y | Disable if you fit an external pull resistor (saves sleep current) |
| `BB_LED_NONE` / `BB_LED_GPIO` / `BB_LED_WS2812` | WS2812 | LED type |
| `BB_LED_GPIO_NUM`, `BB_LED_ACTIVE_HIGH` | 21, y | Plain LED |
| `BB_WS2812_GPIO`, `BB_WS2812_PWR_GPIO` | 18, 17 | TinyS3 RGB LED and its LDO2 enable (−1 = none) |
| `BB_WS2812_BRIGHTNESS` | 40 | 1–255 |
| `BB_LONG_PRESS_MS` | 3000 | Hold time to enter sync mode |
| `BB_SYNC_WINDOW_S` | 60 | Advertising window |
| `BB_DEVICE_NAME` | `button-boi` | The last 2 bytes of the BT address are appended |
| `BB_BATTERY_SENSE` | y | Report battery voltage in the sync page |
| `BB_VBAT_GPIO`, `BB_VBAT_R_TOP_KOHM`, `BB_VBAT_R_BOTTOM_KOHM` | 10, 442, 160 | TinyS3 VBAT divider |
| `BB_VBUS_GPIO` | 33 | USB-power sense (−1 = none) |
| `BB_DEBUG_NO_SLEEP` | n | Stays awake so the serial console keeps working |

## How it works

```
src/main.c             wake-cause state machine (power-on / button / timer)
src/event_log_core.c   flash ring log, pure C, unit-tested on the host
src/event_log.c        esp_partition backend + RTC-RAM state cache
src/timekeep.c         wall clock validity + session id
src/button.c           GPIO input + EXT1 deep-sleep wake
src/led.c              none / GPIO / WS2812 indicator
src/ble_sync.c         NimBLE GATT server
web/index.html         Web Bluetooth client (single file, no dependencies)
```

**Storage.** Records go to a 64 KB raw `evlog` partition (`partitions.csv`),
which holds at least 3,840 records. When it fills, the oldest 4 KB sector is
erased and reused. Each 16-byte record has a sequence number and a CRC-8, so
after power loss the firmware recovers the write position by scanning, and
half-written records are skipped. On a normal wake the scan is skipped,
because the write position is cached in RTC RAM and checked against flash.

**Time.** The ESP32-S3 has no battery-backed clock. The RTC timer keeps time
through deep sleep but resets when power is lost. Each power-on starts a new
*session*. Until a client sets the clock, records store seconds since
power-on. When a client syncs, the firmware logs a `TIME_SYNC` record holding
the old and new clock values. The web page uses it to:
- convert earlier records from the same session to wall-clock time (shown as
  *estimated*), and
- spread the RTC drift measured at each sync across the records since the
  previous sync (shown as *drift-corrected*).

Records from a session that never got a sync, for example because the battery
died first, can only be shown as "+N s after power-on".

**Battery.** Each time sync mode starts, before the radio comes on, the
firmware averages 16 ADC samples of the TinyS3's VBAT divider (442 k / 160 k
on GPIO10). The sync page shows the voltage and an approximate LiPo charge
percentage. When USB power is detected (GPIO33), the page says so instead,
because the charger holds VBAT near 4.2 V. The divider is fixed hardware and
draws about 7 µA all the time. The red power LED and the orange charge LED are
both fed from USB, so on battery they stay dark. The charge LED can also be
disabled permanently by cutting solder jumper A3.

**Power.** A press wakes the chip, writes 16 bytes and flashes the LED. Expected
awake time is about 200–300 ms (an estimate, not yet measured). The WS2812's LDO is switched off and the pin is held
low through sleep. During sync mode the radio advertises every 100–200 ms. The
firmware never stays connectable outside that window.

### BLE GATT protocol

Service `6b1f0001-3c5e-4a7d-9b2e-8f1c0d2a7e51`. All integers are
little-endian.

| Char | UUID | Ops | Payload |
|---|---|---|---|
| Info | `6b1f0002-…` | read | `u8 proto=1, u8 flags(bit0=time valid), u16 session, u64 device_ms, u32 first_seq, u32 last_seq, u32 count, u32 capacity, u16 battery_mv(0=unknown), u8 power_flags(bit0=USB)` |
| Time | `6b1f0003-…` | read / write | read: `u64 device_ms, u8 valid` · write: `u64 epoch_ms` |
| Control | `6b1f0004-…` | write | `01 <u32 from_seq>` = dump · `02 "WIPE"` = erase |
| Data | `6b1f0005-…` | notify | N × 16-byte records, then `FF <u32 count>` |

Record: `u32 seq, u32 t, u32 aux, u16 session, u8 type|flags, u8 crc8`.
Types: 1 boot (aux = reset reason), 2 press (aux = ms), 3 time sync
(t = new epoch, aux = previous device clock), 4 erase. Flag `0x80` means `t` is
Unix epoch seconds.

You can also use a generic BLE app such as nRF Connect for poking around.

## Known limitations / next steps

- **GPIO0 is a strapping pin.** It's the default only because the TinyS3 BOOT
  button is wired to it. For real use, wire a button to a free RTC GPIO such as
  1 or 2.
- **Clock drift.** The TinyS3 has no 32 kHz crystal. The firmware uses the
  more stable RC_FAST/256 slow clock, which costs about 5 µA, and the web page
  corrects drift at each sync. For accurate time without syncing, add a 32 kHz
  crystal or an RTC chip.
- **No BLE security.** Anyone in range during the 60 s window can read or
  erase the log. Pairing/bonding is the obvious next step.
- A held or stuck button doesn't drain the battery: the device re-checks it
  every 30 s on a timer instead of waking continuously.
- The battery reading is only as accurate as the ADC calibration and divider
  tolerance. Expect tens of mV of error, and adjust the resistor values in
  menuconfig if a multimeter disagrees.
- Not done yet: light sleep during sync, OTA updates.
- **Not measured yet:** sleep current and energy per press. Measure them on
  battery with a power profiler and record them here.
