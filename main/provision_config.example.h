#pragma once

// Copy this file to provision_config.h and set this to 1 to enable provisioning.
#define THINGINO_PROVISION_ENABLED 0
#define THINGINO_ROOT_PASSWORD "change-me"
#define THINGINO_WIFI_SSID "YourWifiSsid"
#define THINGINO_WIFI_PASSWORD "YourWifiPassword"
// Set to 1 to override the camera's hostname during provisioning. When 0 (the
// default) the provisioner does not submit a hostname, so the firmware keeps its
// own descriptive auto-generated name (e.g. "ing-wyze-cam3-acb2") instead of a
// generic "<prefix>-<ssid-suffix>". THINGINO_HOSTNAME_PREFIX applies only when 1.
#define THINGINO_SET_HOSTNAME 0
#define THINGINO_HOSTNAME_PREFIX "thingino"
#define THINGINO_TIMEZONE "America/New_York"
#define THINGINO_ROOT_PUBLIC_KEY ""
