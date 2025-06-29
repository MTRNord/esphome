#pragma once

#include "esphome/components/climate_ir/climate_ir.h"
#include "esphome/components/remote_base/nec_protocol.h"
#include "esphome/core/automation.h"
#include <cinttypes>

namespace esphome {
namespace emerio_pac_125152 {

// Temperature
const uint8_t TEMP_MIN = 15;  // Celsius
const uint8_t TEMP_MAX = 31;  // Celsius

const uint16_t ADDRESS = 0xFF00;

// The known NEC commands for the Emerio PAC 125152
const uint16_t CMD_POWER = 0xBA45;
const uint16_t CMD_FAN_TOGGLE = 0xA15E;
const uint16_t CMD_TEMP_UP = 0xF807;
const uint16_t CMD_TEMP_DOWN = 0xF30C;
const uint16_t CMD_DEHUMIDIFY_TOGGLE = 0xB847;
const uint16_t CMD_TIMER = 0xBD42;
const uint16_t CMD_MODE = 0xF609;
const uint16_t CMD_SLEEPING = 0xB54A;

// Internal cycling order for modes (excluding OFF and DRY/DEHUMIDIFY)
enum InternalMode { MODE_AUTO = 0, MODE_COOL = 1, MODE_FAN = 2, MODE_COUNT = 3 };

// Internal cycling order for fan speeds
enum InternalFan { FAN_LOW = 0, FAN_MEDIUM = 1, FAN_HIGH = 2, FAN_COUNT = 3 };

// Map climate::ClimateMode to InternalMode
inline InternalMode to_internal_mode(climate::ClimateMode mode) {
  switch (mode) {
    case climate::CLIMATE_MODE_AUTO:
      return MODE_AUTO;
    case climate::CLIMATE_MODE_COOL:
      return MODE_COOL;
    case climate::CLIMATE_MODE_FAN_ONLY:
      return MODE_FAN;
    default:
      return MODE_AUTO;
  }
}

// Map climate::ClimateFanMode to InternalFan
inline InternalFan to_internal_fan(climate::ClimateFanMode fan) {
  switch (fan) {
    case climate::CLIMATE_FAN_LOW:
      return FAN_LOW;
    case climate::CLIMATE_FAN_MEDIUM:
      return FAN_MEDIUM;
    case climate::CLIMATE_FAN_HIGH:
      return FAN_HIGH;
    default:
      return FAN_LOW;
  }
}

// Random 32bit value; If this changes existing restore preferences are invalidated
static const uint32_t RESTORE_STATE_VERSION = 0x828EA6ADUL;

/// Struct used to save the state of the device in restore memory.
/// Make sure to update RESTORE_STATE_VERSION when changing the struct entries.
struct EmerioPac125152ClimateDeviceRestoreState {
  climate::ClimateMode prev_mode;
  climate::ClimateFanMode prev_fan;
  float prev_temp{TEMP_MIN};    // Previous temperature setpoint
  bool prev_dehumidify{false};  // true if the device was in DRY mode, false otherwise
  bool prev_on_off{false};      // true if the device was ON, false if it was OFF
} __attribute__((packed));

class EmerioPac125152Climate : public climate_ir::ClimateIR {
 public:
  EmerioPac125152Climate();

  climate::ClimateTraits traits() override;
  void setup() override;
  void control(const climate::ClimateCall &call) override;

  // Manual state calibration
  void calibrate_state(climate::ClimateMode mode, float temperature, climate::ClimateFanMode fan_mode);

  // Action for ESPHome automations
  void do_calibrate_state(int mode, float temperature, int fan_mode);

 protected:
  bool supports_heat_{false};
  void transmit_state() override;
  ESPPreferenceObject emeriopac_rtc_;

  // Internal state tracking - what we think the AC hardware is set to
  climate::ClimateMode mode_before_{climate::CLIMATE_MODE_OFF};
  climate::ClimateFanMode fan_mode_before_{climate::CLIMATE_FAN_LOW};
  float target_temperature_before_{TEMP_MIN};
  bool prev_dehumidify_{false};
  bool prev_on_off_{false};                                           // true if the device was ON, false if it was OFF
  climate::ClimateFanMode fan_before_dry_{climate::CLIMATE_FAN_LOW};  // Fan setting before entering DRY mode
  uint32_t setpoint_busy_until_{0};                                   // Not saved

  // Helper to send NEC command
  void send_nec_command_(uint16_t command);

  // Device-specific state management
  optional<EmerioPac125152ClimateDeviceRestoreState> restore_emerio_pac_state_();
  void save_emerio_pac_state_();

  // Helper to sync all state variables consistently
  void sync_all_state_variables_();
};

template<typename... Ts> class CalibrateStateAction : public Action<Ts...> {
 public:
  explicit CalibrateStateAction(EmerioPac125152Climate *emerio_pac_125152_climate)
      : emerio_pac_125152_climate_(emerio_pac_125152_climate) {}

  TEMPLATABLE_VALUE(int, mode)
  TEMPLATABLE_VALUE(float, temperature)
  TEMPLATABLE_VALUE(int, fan_mode)

  void play(Ts... x) override {
    int mode = this->mode_.value(x...);
    float temp = this->temperature_.value(x...);
    int fan = this->fan_mode_.value(x...);
    emerio_pac_125152_climate_->do_calibrate_state(mode, temp, fan);
  }

 protected:
  EmerioPac125152Climate *emerio_pac_125152_climate_;
};

}  // namespace emerio_pac_125152
}  // namespace esphome
