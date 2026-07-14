# Thingino Provisioner

ESP32-C3 firmware for scanning Wi-Fi networks on a tiny OLED board and automatically provisioning Thingino cameras when their initial `THINGINO-*` portal SSID appears.

The firmware was built for the ESP32-C3 SuperMini-style board with a 0.42 inch SSD1306-compatible OLED. It scans nearby access points, shows the strongest networks on the display, and can connect to open Thingino setup portals to submit Wi-Fi and root credentials.

## Features

- ESP32-C3 Wi-Fi scanner with 72x40 OLED output.
- Detects open `THINGINO-*` portal SSIDs.
- Provisions newer Thingino portals using `/x/api.cgi?action=save` when available.
- Falls back to legacy `/x/index.cgi` portals with the required review and confirm flow.
- Avoids printing configured Wi-Fi or root passwords in serial logs.
- Keeps private provisioning values out of git via `main/provision_config.h`.

## Hardware

Default pin assumptions:

- OLED SDA: GPIO 5
- OLED SCL: GPIO 6
- OLED I2C address: `0x3c`, with `0x3d` fallback
- Target: ESP32-C3, 4 MB flash

If your board uses different OLED pins, edit the `OLED_SDA_GPIO` and `OLED_SCL_GPIO` defines in `main/main.c`.

## Configure

Create a private config file from the example:

```sh
cp main/provision_config.example.h main/provision_config.h
```

Edit `main/provision_config.h`:

```c
#define THINGINO_PROVISION_ENABLED 1
#define THINGINO_ROOT_PASSWORD "your-camera-root-password"
#define THINGINO_WIFI_SSID "your-wifi-ssid"
#define THINGINO_WIFI_PASSWORD "your-wifi-password"
#define THINGINO_HOSTNAME_PREFIX "thingino"
#define THINGINO_TIMEZONE "America/New_York"
#define THINGINO_ROOT_PUBLIC_KEY ""
```

`main/provision_config.h` is ignored by git. Do not commit real credentials.

If no private config file exists, the firmware builds with `main/provision_config.example.h`, where provisioning is disabled and the device acts as a scanner.

## Build and Flash

Install and export ESP-IDF, then build for ESP32-C3:

```sh
source ~/esp/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Adjust the serial port for your machine.

## Behavior

On boot, the ESP32-C3 scans for access points every few seconds and displays the strongest networks on the OLED.

When provisioning is enabled and an open `THINGINO-*` SSID is found, it:

1. Connects to the portal.
2. Tries the newer Thingino API endpoint.
3. Falls back to the legacy CGI form when the API is not present.
4. For legacy portals, posts `mode=review`, parses the confirmation form, then posts `mode=save`.
5. Disconnects and returns to scanning.

After a successful provision attempt, the camera should leave portal mode and join the configured Wi-Fi network.

## Secret Hygiene

The repository intentionally ignores:

- `main/provision_config.h`
- `sdkconfig`
- `build/`
- generated firmware binaries and maps

Before publishing or sharing changes, verify staged files only:

```sh
git diff --cached --name-only
git diff --cached
```

Never commit generated firmware images built with private credentials, because string literals can be embedded in the binary.
