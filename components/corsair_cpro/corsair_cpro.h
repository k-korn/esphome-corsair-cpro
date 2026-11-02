#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include "tinyusb.h"
#include "class/hid/hid_device.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace esphome {
namespace corsair_cpro {

// Corsair Commander Pro USB Identifiers
static constexpr uint16_t CORSAIR_VID = 0x1B1C;
static constexpr uint16_t CORSAIR_PID = 0x0C10;

// Corsair CPro Command Identifiers (from drivers/hwmon/corsair-cpro.c)
static constexpr uint8_t CTL_GET_FW_VER     = 0x02;
static constexpr uint8_t CTL_GET_BL_VER     = 0x06;
static constexpr uint8_t CTL_GET_TMP_CNCT   = 0x10;
static constexpr uint8_t CTL_GET_TMP        = 0x11;
static constexpr uint8_t CTL_GET_VOLT       = 0x12;
static constexpr uint8_t CTL_GET_FAN_CNCT   = 0x20;
static constexpr uint8_t CTL_GET_FAN_RPM    = 0x21;
static constexpr uint8_t CTL_GET_FAN_PWM    = 0x22;
static constexpr uint8_t CTL_SET_FAN_FPWM   = 0x23;
static constexpr uint8_t CTL_SET_FAN_TARGET = 0x24;

class CorsairCPro : public Component {
 public:
  float get_setup_priority() const override {
    return setup_priority::BUS;
  }

  void setup() override;
  void loop() override;

  /// Custom USB Serial Number (defaults to hardware MAC if omitted)
  void set_serial_number(const std::string &serial) {
    std::lock_guard<std::mutex> lock(this->lock_);
    strncpy(this->serial_str_, serial.c_str(), sizeof(this->serial_str_) - 1);
    this->serial_str_[sizeof(this->serial_str_) - 1] = '\0';
  }

  /// Mark whether a temperature channel (0..3) is advertised as connected
  void set_temp_connected(uint8_t channel, bool connected) {
    if (channel < 4) {
      std::lock_guard<std::mutex> lock(this->lock_);
      this->temp_connected_[channel] = connected;
    }
  }

  /// Mark fan connection mode (0=disconnected, 1=3pin, 2=4pin)
  void set_fan_connected(uint8_t channel, uint8_t mode) {
    if (channel < 6) {
      std::lock_guard<std::mutex> lock(this->lock_);
      this->fan_connected_[channel] = mode;
    }
  }

  /// Update temperature for channel 0 (default) in °C
  void set_temperature(float temp_c) {
    this->set_temperature(0, temp_c);
  }

  /// Update temperature for specific channel (0..3) in °C
  void set_temperature(uint8_t channel, float temp_c) {
    if (channel < 4) {
      std::lock_guard<std::mutex> lock(this->lock_);
      if (std::isnan(temp_c)) {
        this->has_temperature_[channel] = false;
      } else {
        this->temperature_c_[channel] = temp_c;
        this->has_temperature_[channel] = true;
        this->temp_connected_[channel] = true;
      }
    }
  }

  /// Update rail voltage in Volts (e.g. 12.0f, 5.0f, 3.3f) for rail 0=12V, 1=5V, 2=3.3V
  void set_voltage(uint8_t rail, float volts) {
    if (!std::isnan(volts) && volts >= 0.0f) {
      this->set_voltage_mv(rail, (uint16_t)std::round(volts * 1000.0f));
    }
  }

  /// Update rail voltage in millivolts directly (e.g. 12000, 5000, 3300)
  void set_voltage_mv(uint8_t rail, uint16_t mv) {
    std::lock_guard<std::mutex> lock(this->lock_);
    if (rail == 0) this->volt_12v_mv_ = mv;
    else if (rail == 1) this->volt_5v_mv_ = mv;
    else if (rail == 2) this->volt_3v3_mv_ = mv;
  }

  /// Update fan RPM for a specific channel (0..5)
  void set_fan_rpm(uint8_t channel, uint16_t rpm) {
    if (channel < 6) {
      std::lock_guard<std::mutex> lock(this->lock_);
      this->fan_rpm_[channel] = rpm;
      if (this->fan_connected_[channel] == 0) {
        this->fan_connected_[channel] = 2;  // 4-pin PWM mode
      }
    }
  }

  /// Read the current host-commanded PWM % for a fan channel (0..5)
  uint8_t get_fan_pwm(uint8_t channel) const {
    if (channel < 6) return this->fan_pwm_percent_[channel];
    return 0;
  }

  /// Read the current host-commanded target RPM for a fan channel (0..5)
  uint16_t get_fan_target(uint8_t channel) const {
    if (channel < 6) return this->fan_target_[channel];
    return 0;
  }

  /// Internal handler called from tud_hid_set_report_cb
  void handle_output_report(hid_report_type_t report_type, const uint8_t *buffer, uint16_t bufsize);

 protected:
  mutable std::mutex lock_;
  bool hid_ready_{false};

  char serial_str_[32]{"000000000001"};

  // Temperature sensors (channels 0..3)
  float temperature_c_[4]{0.0f, 0.0f, 0.0f, 0.0f};
  bool has_temperature_[4]{false, false, false, false};
  bool temp_connected_[4]{false, false, false, false};

  // Voltage rails (millivolts)
  uint16_t volt_12v_mv_{12000};  // Nominal 12.00 V
  uint16_t volt_5v_mv_{5000};    // Nominal 5.00 V
  uint16_t volt_3v3_mv_{3300};   // Nominal 3.30 V

  // Fan emulation (6 fan headers)
  uint8_t fan_connected_[6]{0, 0, 0, 0, 0, 0};  // Advertised if mapped or set
  uint16_t fan_rpm_[6]{0, 0, 0, 0, 0, 0};
  uint8_t fan_pwm_percent_[6]{50, 50, 50, 50, 50, 50};
  uint16_t fan_target_[6]{0, 0, 0, 0, 0, 0};

  // Pending response buffer if endpoint is temporarily busy
  uint8_t pending_resp_[16]{0};
  bool has_pending_resp_{false};
};

}  // namespace corsair_cpro
}  // namespace esphome
