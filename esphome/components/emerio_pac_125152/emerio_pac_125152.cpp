#include "emerio_pac_125152.h"
#include "esphome/core/log.h"

namespace esphome {
namespace emerio_pac_125152 {

static const char *const TAG = "climate.emerio_pac_125152";

optional<EmerioPac125152ClimateDeviceRestoreState> EmerioPac125152Climate::restore_emerio_pac_state_() {
  this->emeriopac_rtc_ = global_preferences->make_preference<EmerioPac125152ClimateDeviceRestoreState>(
      this->get_object_id_hash() ^ RESTORE_STATE_VERSION);
  EmerioPac125152ClimateDeviceRestoreState recovered{};
  if (!this->emeriopac_rtc_.load(&recovered))
    return {};

  ESP_LOGD(TAG, "Restored state: prev_mode=%d, prev_fan=%d, prev_temp=%.1f, prev_dehumidify=%s, prev_on_off=%s",
           to_internal_mode(recovered.prev_mode), to_internal_fan(recovered.prev_fan), recovered.prev_temp,
           recovered.prev_dehumidify ? "true" : "false", recovered.prev_on_off ? "true" : "false");
  return recovered;
}

void EmerioPac125152Climate::save_emerio_pac_state_() {
#if (defined(USE_ESP_IDF) || (defined(USE_ESP8266) && USE_ARDUINO_VERSION_CODE >= VERSION_CODE(3, 0, 0))) && \
    !defined(CLANG_TIDY)
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#define TEMP_IGNORE_MEMACCESS
#endif
  EmerioPac125152ClimateDeviceRestoreState state{};
  // initialize as zero to prevent random data on stack triggering erase
  memset(&state, 0, sizeof(EmerioPac125152ClimateDeviceRestoreState));
#ifdef TEMP_IGNORE_MEMACCESS
#pragma GCC diagnostic pop
#undef TEMP_IGNORE_MEMACCESS
#endif

  // Set the state values
  state.prev_mode = this->mode_before_;  // This tracks the AC's operational mode (even when OFF)
  state.prev_fan = this->fan_mode_before_;
  state.prev_temp = this->target_temperature_before_;
  state.prev_dehumidify = this->prev_dehumidify_;  // Use the actual dehumidify state
  state.prev_on_off = this->prev_on_off_;

  ESP_LOGD(TAG, "Saving state: prev_mode=%d, prev_fan=%d, prev_temp=%.1f, prev_dehumidify=%s, prev_on_off=%s",
           to_internal_mode(state.prev_mode), to_internal_fan(state.prev_fan), state.prev_temp,
           state.prev_dehumidify ? "true" : "false", state.prev_on_off ? "true" : "false");

  if (this->emeriopac_rtc_.save(&state)) {
    ESP_LOGD(TAG, "Saved state successfully");
  } else {
    ESP_LOGE(TAG, "Failed to save state");
  }
}

void EmerioPac125152Climate::sync_all_state_variables_() {
  // Validate current ESPHome state before syncing
  if (!this->fan_mode.has_value()) {
    ESP_LOGW(TAG, "Fan mode not set during sync, defaulting to LOW");
    this->fan_mode = climate::CLIMATE_FAN_LOW;
  } else {
    climate::ClimateFanMode current_fan = this->fan_mode.value();
    if (current_fan != climate::CLIMATE_FAN_LOW && current_fan != climate::CLIMATE_FAN_MEDIUM &&
        current_fan != climate::CLIMATE_FAN_HIGH) {
      ESP_LOGW(TAG, "Invalid fan mode %d during sync, correcting to LOW", to_internal_fan(current_fan));
      this->fan_mode = climate::CLIMATE_FAN_LOW;
    }
  }

  // Sync our tracking variables with the current ESPHome state
  // CRITICAL: Don't overwrite mode_before_ when OFF or in DRY mode
  // mode_before_ should always track the underlying operational mode (COOL/AUTO/FAN)
  if (this->mode != climate::CLIMATE_MODE_OFF && this->mode != climate::CLIMATE_MODE_DRY) {
    this->mode_before_ = this->mode;
    ESP_LOGD(TAG, "Synced mode_before_ to %d", to_internal_mode(this->mode_before_));
  } else if (this->mode == climate::CLIMATE_MODE_OFF) {
    ESP_LOGD(TAG, "Mode is OFF, keeping mode_before_ as %d", to_internal_mode(this->mode_before_));
  } else {
    ESP_LOGD(TAG, "Mode is DRY, keeping underlying mode_before_ as %d", to_internal_mode(this->mode_before_));
  }

  this->fan_mode_before_ = this->fan_mode.value();
  this->target_temperature_before_ = this->target_temperature;
  this->prev_dehumidify_ = (this->mode == climate::CLIMATE_MODE_DRY);
  this->prev_on_off_ = (this->mode != climate::CLIMATE_MODE_OFF);

  ESP_LOGD(TAG, "Synced all state: mode=%d, fan=%d, temp=%.1f, dehumidify=%s, on_off=%s",
           to_internal_mode(this->mode_before_), to_internal_fan(this->fan_mode_before_),
           this->target_temperature_before_, this->prev_dehumidify_ ? "true" : "false",
           this->prev_on_off_ ? "true" : "false");
}

EmerioPac125152Climate::EmerioPac125152Climate()
    : climate_ir::ClimateIR(TEMP_MIN, TEMP_MAX, 1.0f, true, true,
                            {climate::CLIMATE_FAN_LOW, climate::CLIMATE_FAN_MEDIUM, climate::CLIMATE_FAN_HIGH}) {}

void EmerioPac125152Climate::setup() {
  // Initialize base class first - this handles ESPHome's own state restoration
  climate_ir::ClimateIR::setup();

  // Restore our device-specific tracking state (what we think the AC hardware is set to)
  optional<EmerioPac125152ClimateDeviceRestoreState> state = this->restore_emerio_pac_state_();
  if (state.has_value()) {
    // Restore our tracking of what the AC hardware should be set to
    this->mode_before_ = state->prev_mode;
    this->fan_mode_before_ = state->prev_fan;
    this->target_temperature_before_ = state->prev_temp;
    this->prev_dehumidify_ = state->prev_dehumidify;
    this->prev_on_off_ = state->prev_on_off;
    ESP_LOGD(TAG, "Restored AC hardware tracking state");

    // Use our restored device state
    this->mode = this->prev_on_off_ ? (this->prev_dehumidify_ ? climate::CLIMATE_MODE_DRY : this->mode_before_)
                                    : climate::CLIMATE_MODE_OFF;
    this->fan_mode = this->fan_mode_before_;
    this->target_temperature = this->target_temperature_before_;
    ESP_LOGD(TAG, "Overwrote ESPHome state with restored device state");

  } else {
    // Default tracking state - assume AC is off with default settings
    this->mode_before_ = climate::CLIMATE_MODE_OFF;
    this->fan_mode_before_ = climate::CLIMATE_FAN_LOW;
    this->target_temperature_before_ = TEMP_MIN;
    this->prev_dehumidify_ = false;
    this->prev_on_off_ = false;
    ESP_LOGD(TAG, "Using default AC hardware tracking state");

    // Use our default device state
    this->mode = climate::CLIMATE_MODE_OFF;
    this->fan_mode = climate::CLIMATE_FAN_LOW;
    this->target_temperature = TEMP_MIN;
    ESP_LOGD(TAG, "Overwrote ESPHome state with default device state");
  }

  ESP_LOGD(TAG,
           "AC hardware tracking state: mode_before_=%d, fan_mode_before_=%d, target_temperature_before_=%.1f, "
           "prev_dehumidify_=%s, prev_on_off_=%s",
           to_internal_mode(this->mode_before_), to_internal_fan(this->fan_mode_before_),
           this->target_temperature_before_, this->prev_dehumidify_ ? "true" : "false",
           this->prev_on_off_ ? "true" : "false");

  // NOTE: We deliberately DO NOT sync ESPHome state with our tracking state here
  // This avoids sending unwanted IR commands on startup
  // transmit_state() will be called when needed and will handle any required sync
}

climate::ClimateTraits EmerioPac125152Climate::traits() {
  auto traits = climate_ir::ClimateIR::traits();
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_AUTO, climate::CLIMATE_MODE_COOL,
                              climate::CLIMATE_MODE_DRY, climate::CLIMATE_MODE_FAN_ONLY});
  return traits;
}

void EmerioPac125152Climate::send_nec_command_(uint16_t command) {
  // We send 3 repeats to ensure it actually receives the command
  this->transmit_<remote_base::NECProtocol>({ADDRESS, command, 3});
  // Sleep for a short time to allow the device to process the command
  delay(20);
}

void EmerioPac125152Climate::transmit_state() {
  // Validate ESPHome state before proceeding
  if (!this->fan_mode.has_value()) {
    ESP_LOGE(TAG, "Fan mode not set! Defaulting to last known");
    this->fan_mode = this->fan_mode_before_;
  }

  // Validate fan mode value
  climate::ClimateFanMode current_fan = this->fan_mode.value();
  if (current_fan != climate::CLIMATE_FAN_LOW && current_fan != climate::CLIMATE_FAN_MEDIUM &&
      current_fan != climate::CLIMATE_FAN_HIGH) {
    ESP_LOGE(TAG, "Invalid fan mode %d! Defaulting to last known", to_internal_fan(current_fan));
    this->fan_mode = this->fan_mode_before_;
  }

  // Save previous state for cycling math
  climate::ClimateMode prev_mode = this->mode_before_;
  ESP_LOGD(TAG, "Previous mode: %d", to_internal_mode(prev_mode));
  climate::ClimateFanMode prev_fan = this->fan_mode_before_;
  ESP_LOGD(TAG, "Previous fan: %d", to_internal_fan(prev_fan));
  bool prev_dehumidify = this->prev_dehumidify_;
  ESP_LOGD(TAG, "Previous dehumidify: %s", prev_dehumidify ? "true" : "false");
  float prev_temp = this->target_temperature_before_;
  ESP_LOGD(TAG, "Previous temperature: %.1f", prev_temp);
  ESP_LOGD(TAG, "Previous on/off state: %s", this->prev_on_off_ ? "true" : "false");

  // Requested state
  climate::ClimateMode requested_mode = this->mode;
  ESP_LOGD(TAG, "Requested mode: %d", to_internal_mode(requested_mode));
  climate::ClimateFanMode requested_fan = this->fan_mode.value();
  ESP_LOGD(TAG, "Requested fan: %d", to_internal_fan(requested_fan));
  bool requested_dehumidify = (this->mode == climate::CLIMATE_MODE_DRY);
  ESP_LOGD(TAG, "Requested dehumidify: %s", requested_dehumidify ? "true" : "false");
  float requested_temp = this->target_temperature;
  ESP_LOGD(TAG, "Requested temperature: %.1f", requested_temp);

  // Handle power
  if (requested_mode == climate::CLIMATE_MODE_OFF) {
    if (this->prev_on_off_) {
      ESP_LOGD(TAG, "Turning off climate");
      send_nec_command_(CMD_POWER);

      // CRITICAL: When turning OFF, DON'T change mode_before_
      // The AC hardware remembers its mode when turned off
      // Update fan/temp tracking only if they represent valid user settings
      // (not corrected values from rejected changes)
      bool fan_is_valid = (this->fan_mode.value() == climate::CLIMATE_FAN_LOW ||
                           this->fan_mode.value() == climate::CLIMATE_FAN_MEDIUM ||
                           this->fan_mode.value() == climate::CLIMATE_FAN_HIGH);
      bool temp_is_valid = (this->target_temperature >= TEMP_MIN && this->target_temperature <= TEMP_MAX);

      if (fan_is_valid) {
        this->fan_mode_before_ = this->fan_mode.value();
        ESP_LOGD(TAG, "Updated fan_mode_before_ to %d", to_internal_fan(this->fan_mode_before_));
      } else {
        ESP_LOGW(TAG, "Invalid fan mode %d, keeping previous: %d", to_internal_fan(this->fan_mode.value()),
                 to_internal_fan(this->fan_mode_before_));
      }

      if (temp_is_valid) {
        this->target_temperature_before_ = this->target_temperature;
        ESP_LOGD(TAG, "Updated target_temperature_before_ to %.1f", this->target_temperature_before_);
      } else {
        ESP_LOGW(TAG, "Invalid temperature %.1f, keeping previous: %.1f", this->target_temperature,
                 this->target_temperature_before_);
      }

      this->prev_dehumidify_ = false;  // Device turns off, no dehumidify
      this->prev_on_off_ = false;      // Set previous state to OFF

      ESP_LOGD(TAG, "AC turned OFF, but hardware remembers mode: %d", to_internal_mode(this->mode_before_));

      this->publish_state();
      this->save_emerio_pac_state_();
      return;
    } else {
      // Already OFF, no action needed
      ESP_LOGD(TAG, "AC already OFF, no power command needed");

      // CRITICAL: Don't overwrite our tracking state if fan/temp were just corrected in control()
      // Only update tracking state if the values actually represent valid user intent
      bool fan_matches_tracking = (this->fan_mode.value() == this->fan_mode_before_);
      bool temp_matches_tracking = (abs(this->target_temperature - this->target_temperature_before_) < 0.1f);

      if (fan_matches_tracking && temp_matches_tracking) {
        ESP_LOGD(TAG, "ESPHome state matches tracking state, no update needed");
      } else {
        ESP_LOGW(TAG, "ESPHome state differs from tracking - keeping tracking state (fan=%d, temp=%.1f)",
                 to_internal_fan(this->fan_mode_before_), this->target_temperature_before_);

        // Force ESPHome state back to our tracking state
        this->fan_mode = this->fan_mode_before_;
        this->target_temperature = this->target_temperature_before_;
      }

      this->prev_dehumidify_ = false;  // Device is off, no dehumidify
      this->prev_on_off_ = false;      // Ensure this stays false

      ESP_LOGD(TAG, "OFF->OFF transition completed, AC remains OFF");
      this->publish_state();
      this->save_emerio_pac_state_();
      return;
    }
  } else {
    if (!this->prev_on_off_) {
      ESP_LOGD(TAG, "Turning on climate from OFF state, AC will return to mode: %d",
               to_internal_mode(this->mode_before_));
      send_nec_command_(CMD_POWER);  // Turn on if previously off

      // Update on/off state immediately
      this->prev_on_off_ = true;
    }
  }

  // Handle dehumidify (DRY) mode
  if (requested_dehumidify != prev_dehumidify) {
    if (requested_dehumidify) {
      ESP_LOGD(TAG, "Entering DRY mode");

      // CRITICAL: Save current fan setting before AC changes it to LOW
      this->fan_before_dry_ = this->fan_mode_before_;
      ESP_LOGD(TAG, "Saved fan setting before DRY: %d", to_internal_fan(this->fan_before_dry_));

      send_nec_command_(CMD_DEHUMIDIFY_TOGGLE);

      // AC automatically sets fan to LOW and overlays DRY on current mode
      this->fan_mode_before_ = climate::CLIMATE_FAN_LOW;  // AC forces LOW fan in DRY
      this->target_temperature_before_ = this->target_temperature;
      this->prev_dehumidify_ = requested_dehumidify;
      this->prev_on_off_ = (this->mode != climate::CLIMATE_MODE_OFF);

      ESP_LOGD(TAG, "Entered DRY mode: fan changed to LOW, underlying mode remains: %d",
               to_internal_mode(this->mode_before_));

      this->publish_state();
      this->save_emerio_pac_state_();
      return;
    } else {
      ESP_LOGD(TAG, "Leaving DRY mode - sending MODE command");
      // To leave DRY mode, send MODE command (not DRY command again)
      send_nec_command_(CMD_MODE);

      // CRITICAL: AC hardware always returns to AUTO mode and restores previous fan setting
      prev_mode = climate::CLIMATE_MODE_AUTO;
      this->mode_before_ = climate::CLIMATE_MODE_AUTO;

      // Restore the fan setting that was active before entering DRY
      prev_fan = this->fan_before_dry_;
      this->fan_mode_before_ = this->fan_before_dry_;

      ESP_LOGD(TAG, "Left DRY mode: AC returned to AUTO mode, fan restored to %d",
               to_internal_fan(this->fan_before_dry_));
    }
  }

  // Handle mode cycling (AUTO/COOL/FAN) - only if not DRY and mode change is needed
  if (requested_mode != climate::CLIMATE_MODE_DRY) {
    // Only cycle modes if the requested mode is different from current AC hardware mode
    if (requested_mode != prev_mode) {
      int steps = (to_internal_mode(requested_mode) - to_internal_mode(prev_mode) + MODE_COUNT) % MODE_COUNT;
      ESP_LOGD(TAG, "Cycling modes: %d steps from %d to %d", steps, to_internal_mode(prev_mode),
               to_internal_mode(requested_mode));
      for (int i = 0; i < steps; i++) {
        ESP_LOGD(TAG, "Sending mode toggle command");
        send_nec_command_(CMD_MODE);
      }
    } else {
      ESP_LOGD(TAG, "No mode cycling needed - AC already in requested mode %d", to_internal_mode(requested_mode));
    }
  }

  // Handle fan cycling
  int fan_steps = (to_internal_fan(requested_fan) - to_internal_fan(prev_fan) + FAN_COUNT) % FAN_COUNT;
  ESP_LOGD(TAG, "Cycling fans: %d steps from %d to %d", fan_steps, to_internal_fan(prev_fan),
           to_internal_fan(requested_fan));
  for (int i = 0; i < fan_steps; i++) {
    ESP_LOGD(TAG, "Sending fan toggle command");
    send_nec_command_(CMD_FAN_TOGGLE);
  }

  // Handle temperature setpoint (only when device is ON)
  if (this->prev_on_off_) {
    int temp_diff = int(roundf(prev_temp)) - int(roundf(requested_temp));
    // Invert the sign as we otherwise move in the wrong direction
    temp_diff = -temp_diff;
    ESP_LOGD(TAG, "Temperature difference: %d", temp_diff);

    // There is a 6s busy/programming window after sending the initial setpoint command.
    // If we send again within this window the device will immediately accept it instead of waiting for another command.
    // This means we need to initially send 2 commands to set the initial temperature,
    // and then we can send 1 command at a time after that if we are still within the busy window.
    // We track the busy window with setpoint_busy_until_.
    uint32_t now = millis();
    if (temp_diff != 0) {
      if (now < this->setpoint_busy_until_) {
        ESP_LOGW(TAG, "Setpoint busy (until %u ms), sending only one step", this->setpoint_busy_until_);
        // Only send one step in the right direction
        if (temp_diff > 0) {
          ESP_LOGD(TAG, "Sending single temperature up command");
          send_nec_command_(CMD_TEMP_UP);
        } else {
          ESP_LOGD(TAG, "Sending single temperature down command");
          send_nec_command_(CMD_TEMP_DOWN);
        }
        // Do not update setpoint_busy_until_ (keep original busy window)
      } else {
        ESP_LOGD(TAG, "Setpoint not busy, sending full difference");
        if (temp_diff > 0) {
          ESP_LOGD(TAG, "Increasing temperature by %d steps", temp_diff);
          for (int i = 0; i < temp_diff + 1; i++) {
            ESP_LOGD(TAG, "Sending temperature up command");
            send_nec_command_(CMD_TEMP_UP);
          }
        } else {
          ESP_LOGD(TAG, "Decreasing temperature by %d steps", -temp_diff);
          for (int i = 0; i < -temp_diff + 1; i++) {
            ESP_LOGD(TAG, "Sending temperature down command");
            send_nec_command_(CMD_TEMP_DOWN);
          }
        }
        this->setpoint_busy_until_ = now + 6000;  // 6 seconds busy window
        ESP_LOGI(TAG, "Set setpoint busy until %u ms", this->setpoint_busy_until_);
      }
    }
  } else {
    ESP_LOGD(TAG, "Device is OFF, skipping temperature commands");
  }

  // Update all state tracking variables consistently
  // CRITICAL: Only update mode_before_ if not in DRY mode and not OFF
  // mode_before_ should always track the underlying operational mode (COOL/AUTO/FAN)
  if (this->mode != climate::CLIMATE_MODE_DRY && this->mode != climate::CLIMATE_MODE_OFF) {
    this->mode_before_ = this->mode;
    ESP_LOGD(TAG, "Updated mode_before_ to %d", to_internal_mode(this->mode_before_));
  } else if (this->mode == climate::CLIMATE_MODE_OFF) {
    ESP_LOGD(TAG, "Mode is OFF, keeping mode_before_ as %d", to_internal_mode(this->mode_before_));
  } else {
    ESP_LOGD(TAG, "Mode is DRY, keeping underlying mode_before_ as %d", to_internal_mode(this->mode_before_));
  }

  this->fan_mode_before_ = this->fan_mode.value();
  this->target_temperature_before_ = this->target_temperature;
  this->prev_dehumidify_ = requested_dehumidify;
  this->prev_on_off_ = (this->mode != climate::CLIMATE_MODE_OFF);

  ESP_LOGD(TAG, "Final state: mode=%d, fan_mode=%d, target_temperature=%.1f, dehumidify=%s, on_off=%s",
           to_internal_mode(this->mode), to_internal_fan(this->fan_mode.value()), this->target_temperature,
           this->prev_dehumidify_ ? "true" : "false", this->prev_on_off_ ? "true" : "false");

  // Now publish state at the end
  this->publish_state();
  this->save_emerio_pac_state_();
}

void EmerioPac125152Climate::calibrate_state(climate::ClimateMode mode, float temperature,
                                             climate::ClimateFanMode fan_mode) {
  // Validate inputs before setting state
  if (temperature < TEMP_MIN || temperature > TEMP_MAX) {
    ESP_LOGE(TAG, "Invalid temperature %.1f, clamping to range [%d, %d]", temperature, TEMP_MIN, TEMP_MAX);
    temperature = std::max((float) TEMP_MIN, std::min((float) TEMP_MAX, temperature));
  }

  // Validate fan mode
  if (fan_mode != climate::CLIMATE_FAN_LOW && fan_mode != climate::CLIMATE_FAN_MEDIUM &&
      fan_mode != climate::CLIMATE_FAN_HIGH) {
    ESP_LOGE(TAG, "Invalid fan mode %d, defaulting to LOW", to_internal_fan(fan_mode));
    fan_mode = climate::CLIMATE_FAN_LOW;
  }

  // Validate mode
  if (mode != climate::CLIMATE_MODE_OFF && mode != climate::CLIMATE_MODE_AUTO && mode != climate::CLIMATE_MODE_COOL &&
      mode != climate::CLIMATE_MODE_DRY && mode != climate::CLIMATE_MODE_FAN_ONLY) {
    ESP_LOGE(TAG, "Invalid climate mode %d, defaulting to OFF", to_internal_mode(mode));
    mode = climate::CLIMATE_MODE_OFF;
  }

  this->mode_before_ = mode;
  this->fan_mode_before_ = fan_mode;
  this->target_temperature_before_ = temperature;
  this->prev_dehumidify_ = (mode == climate::CLIMATE_MODE_DRY);
  this->prev_on_off_ = (mode != climate::CLIMATE_MODE_OFF);

  // Sync ESPHome state with our calibrated state
  this->mode = mode;
  this->fan_mode = fan_mode;
  this->target_temperature = temperature;

  ESP_LOGD(TAG, "Calibrating state: mode=%d, temperature=%.1f, fan_mode=%d", mode, temperature, fan_mode);
  this->publish_state();
  this->save_emerio_pac_state_();
}

void EmerioPac125152Climate::do_calibrate_state(int mode, float temperature, int fan_mode) {
  this->calibrate_state(static_cast<climate::ClimateMode>(mode), temperature,
                        static_cast<climate::ClimateFanMode>(fan_mode));
}

void EmerioPac125152Climate::control(const climate::ClimateCall &call) {
  // CRITICAL: Prevent desync by validating changes when device is OFF
  bool device_currently_off = (this->mode == climate::CLIMATE_MODE_OFF);

  // Check what the user is trying to change
  bool mode_change = call.get_mode().has_value();
  bool fan_change = call.get_fan_mode().has_value();
  bool temp_change = call.get_target_temperature().has_value();

  // If device is OFF, only allow mode changes (to turn it ON)
  if (device_currently_off) {
    if (fan_change || temp_change) {
      if (mode_change && call.get_mode().value() != climate::CLIMATE_MODE_OFF) {
        // User wants to turn device ON and change fan/temp - this is OK
        ESP_LOGD(TAG, "Device OFF: allowing mode change to %d with fan/temp changes",
                 to_internal_mode(call.get_mode().value()));
      } else {
        // User wants to change fan/temp while device stays OFF - reject these changes
        ESP_LOGW(TAG, "Device is OFF: ignoring fan/temperature changes to prevent desync");

        climate_ir::ClimateIR::control(call);

        // When device is OFF, ensure we display the state that will be active when device turns ON
        // Don't change mode (it should stay OFF), but ensure fan/temp match what the AC will use
        this->fan_mode = this->fan_mode_before_;
        this->target_temperature = this->target_temperature_before_;

        ESP_LOGD(TAG, "Device OFF: maintaining fan=%d, temp=%.1f (state when device turns ON)",
                 to_internal_fan(this->fan_mode_before_), this->target_temperature_before_);

        // Publish the corrected state to HA
        this->publish_state();
        return;
      }
    }
  }

  // If we get here, either device is ON or user is only changing mode
  // Call the base class implementation
  climate_ir::ClimateIR::control(call);

  // After the base class has updated this->mode, this->fan_mode, etc.,
  // check if we need to sync our tracking state
  bool state_changed = false;

  if (this->mode != this->mode_before_) {
    ESP_LOGD(TAG, "External mode change detected: %d -> %d", to_internal_mode(this->mode_before_),
             to_internal_mode(this->mode));
    state_changed = true;
  }

  if (this->fan_mode.has_value() && this->fan_mode.value() != this->fan_mode_before_) {
    ESP_LOGD(TAG, "External fan mode change detected: %d -> %d", to_internal_fan(this->fan_mode_before_),
             to_internal_fan(this->fan_mode.value()));
    state_changed = true;
  }

  if (abs(this->target_temperature - this->target_temperature_before_) > 0.1f) {
    ESP_LOGD(TAG, "External temperature change detected: %.1f -> %.1f", this->target_temperature_before_,
             this->target_temperature);
    state_changed = true;
  }

  if (state_changed) {
    // CRITICAL: Don't sync when mode change involves OFF or DRY, as transmit_state() handles this correctly
    // DRY mode transitions need special handling to preserve the underlying mode
    if (this->mode == climate::CLIMATE_MODE_OFF || this->mode_before_ == climate::CLIMATE_MODE_OFF ||
        this->mode == climate::CLIMATE_MODE_DRY || this->mode_before_ == climate::CLIMATE_MODE_DRY) {
      ESP_LOGD(TAG, "Mode change involves OFF or DRY state - letting transmit_state() handle sync");
    } else {
      ESP_LOGD(TAG, "Syncing tracking state after external change");
      // Don't call sync_all_state_variables_() here because transmit_state()
      // will be called next and will handle the state synchronization
    }
  }
}
}  // namespace emerio_pac_125152
}  // namespace esphome
