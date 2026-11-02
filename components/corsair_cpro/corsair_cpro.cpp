#include "corsair_cpro.h"

#include "esp_mac.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include <algorithm>
#include <cinttypes>

namespace esphome {
namespace corsair_cpro {

static const char *const TAG = "corsair_cpro";

// Singleton instance pointer for TinyUSB C callbacks
static CorsairCPro *g_instance = nullptr;

// ────────────────────────────────────────────────────────────────
// USB Descriptors
// ────────────────────────────────────────────────────────────────

// Device Descriptor
static const tusb_desc_device_t DEVICE_DESCRIPTOR = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,    // Defined at interface level
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CORSAIR_VID,  // 0x1B1C (Corsair)
    .idProduct          = CORSAIR_PID,  // 0x0C10 (Commander Pro)
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// HID Report Descriptor
// Vendor Page 0xFF00, Usage 0x01
// Input Report: 16 bytes (Report Count 16, Report Size 8)
// Output Report: 64 bytes (Report Count 64, Report Size 8)
// No Report IDs
static const uint8_t HID_REPORT_DESCRIPTOR[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (Vendor Usage 0x01)
    0xA1, 0x01,        // Collection (Application)

    // ── Input Report (16 bytes) ──
    0x09, 0x02,        //   Usage (Vendor Usage 0x02)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8 bits)
    0x95, 0x10,        //   Report Count (16 bytes)
    0x81, 0x02,        //   Input (Data, Var, Abs)

    // ── Output Report (64 bytes) ──
    0x09, 0x03,        //   Usage (Vendor Usage 0x03)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8 bits)
    0x95, 0x40,        //   Report Count (64 bytes)
    0x91, 0x02,        //   Output (Data, Var, Abs)

    0xC0               // End Collection
};

// Configuration Descriptor Length (Config Header + HID Interface with IN & OUT endpoints)
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

// Configuration Descriptor
static const uint8_t CONFIGURATION_DESCRIPTOR[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface number, string index, protocol, report descriptor len,
    // EP Out address, EP In address, EP size, polling interval (ms)
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE,
                             sizeof(HID_REPORT_DESCRIPTOR),
                             0x01, 0x81, 64, 1),
};

// String descriptors: [0]=Language, [1]=Manufacturer, [2]=Product, [3]=Serial
static const char *STRING_DESCRIPTORS[] = {
    "\x09\x04",              // 0: Supported language (English - 0x0409)
    "Corsair",               // 1: Manufacturer
    "Commander PRO",         // 2: Product
    "000000000001",          // 3: Serial (populated in setup)
};

// ────────────────────────────────────────────────────────────────
// TinyUSB HID Callbacks
// ────────────────────────────────────────────────────────────────

extern "C" {

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void) instance;
  return HID_REPORT_DESCRIPTOR;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) reqlen;
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
  (void) instance;
  (void) report_id;
  if (g_instance != nullptr) {
    g_instance->handle_output_report(report_type, buffer, bufsize);
  }
}

}  // extern "C"

// ────────────────────────────────────────────────────────────────
// Component Lifecycle & Command Handling
// ────────────────────────────────────────────────────────────────

void CorsairCPro::setup() {
  g_instance = this;

  // Initialize hardware-unique serial number from STA MAC if not overridden
  if (strcmp(this->serial_str_, "000000000001") == 0) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
      snprintf(this->serial_str_, sizeof(this->serial_str_), "%02X%02X%02X%02X%02X%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
  }
  STRING_DESCRIPTORS[3] = this->serial_str_;

  ESP_LOGI(TAG, "Initializing Corsair Commander Pro USB emulation (Serial: %s)...",
           this->serial_str_);

  tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
  tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
  tusb_cfg.phy.skip_setup = false;
  tusb_cfg.descriptor.device = &DEVICE_DESCRIPTOR;
  tusb_cfg.descriptor.full_speed_config = CONFIGURATION_DESCRIPTOR;
  tusb_cfg.descriptor.string = STRING_DESCRIPTORS;
  tusb_cfg.descriptor.string_count = sizeof(STRING_DESCRIPTORS) / sizeof(STRING_DESCRIPTORS[0]);

  esp_err_t result = tinyusb_driver_install(&tusb_cfg);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(result));
    this->mark_failed();
    return;
  }

  this->hid_ready_ = true;
  ESP_LOGI(TAG, "Corsair Commander Pro ready (VID=0x%04X, PID=0x%04X, Serial=%s)",
           CORSAIR_VID, CORSAIR_PID, this->serial_str_);
}

void CorsairCPro::loop() {
  if (this->has_pending_resp_ && tud_mounted() && tud_hid_ready()) {
    std::lock_guard<std::mutex> lock(this->lock_);
    if (tud_hid_report(0, this->pending_resp_, sizeof(this->pending_resp_))) {
      this->has_pending_resp_ = false;
    }
  }
}

void CorsairCPro::handle_output_report(hid_report_type_t report_type, const uint8_t *buffer, uint16_t bufsize) {
  if (bufsize == 0 || buffer == nullptr)
    return;

  std::lock_guard<std::mutex> lock(this->lock_);

  uint8_t cmd = buffer[0];
  uint8_t resp[16] = {0};

  switch (cmd) {
    case CTL_GET_TMP_CNCT: {  // 0x10: Connection status for 4 temp channels
      resp[0] = 0x00;         // Success
      for (size_t i = 0; i < 4; i++) {
        resp[i + 1] = this->temp_connected_[i] ? 0x01 : 0x00;
      }
      ESP_LOGD(TAG, "CTL_GET_TMP_CNCT -> [%u, %u, %u, %u]",
               resp[1], resp[2], resp[3], resp[4]);
      break;
    }

    case CTL_GET_TMP: {  // 0x11: Read temp in centi-degrees Celsius (int16 big-endian)
      uint8_t channel = (bufsize > 1) ? buffer[1] : 0;
      if (channel < 4 && this->temp_connected_[channel] && this->has_temperature_[channel]) {
        resp[0] = 0x00;  // Success
        float val_c = this->temperature_c_[channel];
        int16_t centi_c = (int16_t)std::round(val_c * 100.0f);
        resp[1] = (uint8_t)((centi_c >> 8) & 0xFF);
        resp[2] = (uint8_t)(centi_c & 0xFF);
        ESP_LOGV(TAG, "CTL_GET_TMP ch %u -> %.2f °C (%d cC)", channel, val_c, centi_c);
      } else {
        resp[0] = 0x11;  // Error: disconnected / no data
        ESP_LOGV(TAG, "CTL_GET_TMP ch %u -> disconnected / no data", channel);
      }
      break;
    }

    case CTL_GET_VOLT: {  // 0x12: Read rail voltage in millivolts (uint16 big-endian)
      uint8_t rail = (bufsize > 1) ? buffer[1] : 0;
      resp[0] = 0x00;
      uint16_t mv = 0;
      if (rail == 0) {
        mv = this->volt_12v_mv_;
      } else if (rail == 1) {
        mv = this->volt_5v_mv_;
      } else if (rail == 2) {
        mv = this->volt_3v3_mv_;
      } else {
        resp[0] = 0x10;  // Invalid argument
      }
      resp[1] = (uint8_t)((mv >> 8) & 0xFF);
      resp[2] = (uint8_t)(mv & 0xFF);
      ESP_LOGV(TAG, "CTL_GET_VOLT rail %u -> %u mV", rail, mv);
      break;
    }

    case CTL_GET_FAN_CNCT: {  // 0x20: Fan connection status (0=none, 1=3pin, 2=4pin)
      resp[0] = 0x00;
      for (size_t i = 0; i < 6; i++) {
        resp[i + 1] = this->fan_connected_[i];
      }
      ESP_LOGD(TAG, "CTL_GET_FAN_CNCT -> [%u, %u, %u, %u, %u, %u]",
               resp[1], resp[2], resp[3], resp[4], resp[5], resp[6]);
      break;
    }

    case CTL_GET_FAN_RPM: {  // 0x21: Read fan RPM (uint16 big-endian)
      uint8_t channel = (bufsize > 1) ? buffer[1] : 0;
      resp[0] = 0x00;
      uint16_t rpm = (channel < 6) ? this->fan_rpm_[channel] : 0;
      resp[1] = (uint8_t)((rpm >> 8) & 0xFF);
      resp[2] = (uint8_t)(rpm & 0xFF);
      ESP_LOGV(TAG, "CTL_GET_FAN_RPM ch %u -> %u RPM", channel, rpm);
      break;
    }

    case CTL_GET_FAN_PWM: {  // 0x22: Read fan PWM percentage (0..100)
      uint8_t channel = (bufsize > 1) ? buffer[1] : 0;
      resp[0] = 0x00;
      resp[1] = (channel < 6) ? this->fan_pwm_percent_[channel] : 0;
      ESP_LOGV(TAG, "CTL_GET_FAN_PWM ch %u -> %u%%", channel, resp[1]);
      break;
    }

    case CTL_SET_FAN_FPWM: {  // 0x23: Set fixed PWM percentage (0..100)
      uint8_t channel = (bufsize > 1) ? buffer[1] : 0;
      uint8_t val = (bufsize > 2) ? buffer[2] : 0;
      resp[0] = 0x00;
      if (channel < 6) {
        this->fan_pwm_percent_[channel] = std::min(val, (uint8_t)100);
      }
      ESP_LOGI(TAG, "Host set fan %u fixed PWM to %u%%", channel + 1, val);
      break;
    }

    case CTL_SET_FAN_TARGET: {  // 0x24: Set fan target RPM
      uint8_t channel = (bufsize > 1) ? buffer[1] : 0;
      uint16_t target = (bufsize > 3) ? (((uint16_t)buffer[2] << 8) | buffer[3]) : 0;
      resp[0] = 0x00;
      if (channel < 6) {
        this->fan_target_[channel] = target;
      }
      ESP_LOGI(TAG, "Host set fan %u target to %u RPM", channel + 1, target);
      break;
    }

    case CTL_GET_FW_VER: {  // 0x02: Firmware version (bytes 1-3)
      resp[0] = 0x00;
      resp[1] = 0;    // Major
      resp[2] = 9;    // Minor
      resp[3] = 212;  // Patch (0.9.212)
      ESP_LOGD(TAG, "CTL_GET_FW_VER -> 0.9.212");
      break;
    }

    case CTL_GET_BL_VER: {  // 0x06: Bootloader version (bytes 1-2)
      resp[0] = 0x00;
      resp[1] = 0;    // Major
      resp[2] = 1;    // Minor (0.1)
      ESP_LOGD(TAG, "CTL_GET_BL_VER -> 0.1");
      break;
    }

    default:
      ESP_LOGW(TAG, "Unknown command 0x%02X (len=%u)", cmd, bufsize);
      resp[0] = 0x01;  // -EOPNOTSUPP
      break;
  }

  // Transmit 16-byte response packet to the host via Interrupt IN
  if (tud_hid_ready()) {
    tud_hid_report(0, resp, sizeof(resp));
  } else {
    memcpy(this->pending_resp_, resp, sizeof(resp));
    this->has_pending_resp_ = true;
  }
}

}  // namespace corsair_cpro
}  // namespace esphome
