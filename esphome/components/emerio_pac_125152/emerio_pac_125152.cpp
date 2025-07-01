#include "emerio_pac_125152.h"
#include "esphome/core/log.h"

namespace esphome {
namespace emerio_pac_125152 {

static const char *const TAG = "climate.emerio_pac_125152";

// ============================================================================
// DEVICE STATE MANAGEMENT
// ============================================================================

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
  // Rate limit flash writes to prevent excessive wear (especially important for ESP8266)
  uint32_t now = millis();
  if (now - this->last_state_save_ < 1000) {  // Minimum 1 second between saves
    ESP_LOGV(TAG, "Skipping state save - too soon after last save (%.3fs ago)",
             (now - this->last_state_save_) / 1000.0f);
    return;
  }

  // Validate state before saving to prevent saving corrupted data
  if (this->target_temperature_before_ < TEMP_MIN || this->target_temperature_before_ > TEMP_MAX) {
    ESP_LOGE(TAG, "Invalid temperature %.1f - not saving state", this->target_temperature_before_);
    return;
  }

  if (this->fan_mode_before_ != climate::CLIMATE_FAN_LOW && this->fan_mode_before_ != climate::CLIMATE_FAN_MEDIUM &&
      this->fan_mode_before_ != climate::CLIMATE_FAN_HIGH) {
    ESP_LOGE(TAG, "Invalid fan mode %d - not saving state", to_internal_fan(this->fan_mode_before_));
    return;
  }

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
  state.prev_mode = this->mode_before_;
  state.prev_fan = this->fan_mode_before_;
  state.prev_temp = this->target_temperature_before_;
  state.prev_dehumidify = this->prev_dehumidify_;
  state.prev_on_off = this->prev_on_off_;

  ESP_LOGD(TAG, "Saving device state: mode=%d, fan=%d, temp=%.1f, dehumidify=%s, on_off=%s",
           to_internal_mode(state.prev_mode), to_internal_fan(state.prev_fan), state.prev_temp,
           state.prev_dehumidify ? "true" : "false", state.prev_on_off ? "true" : "false");

  // Use ESPHome's robust save mechanism with built-in retry logic
  if (this->emeriopac_rtc_.save(&state)) {
    this->last_state_save_ = now;  // Record successful save time
    ESP_LOGD(TAG, "Device state saved successfully");
  } else {
    ESP_LOGW(TAG, "Failed to save device state - state may be lost on reboot");
  }
}

// ============================================================================
// STATE VALIDATION AND HELPERS
// ============================================================================

bool EmerioPac125152Climate::is_fan_mode_supported_(climate::ClimateFanMode fan_mode) {
  return fan_mode == climate::CLIMATE_FAN_LOW || fan_mode == climate::CLIMATE_FAN_MEDIUM ||
         fan_mode == climate::CLIMATE_FAN_HIGH;
}

void EmerioPac125152Climate::ensure_valid_fan_mode_() {
  if (!this->fan_mode.has_value()) {
    ESP_LOGW(TAG, "Fan mode not set, defaulting to LOW");
    this->fan_mode = climate::CLIMATE_FAN_LOW;
    return;
  }

  if (!this->is_fan_mode_supported_(this->fan_mode.value())) {
    ESP_LOGW(TAG, "Unsupported fan mode %d (AUTO=%d), correcting to device state %d", (int) this->fan_mode.value(),
             (int) climate::CLIMATE_FAN_AUTO, to_internal_fan(this->fan_mode_before_));
    this->fan_mode = this->fan_mode_before_;
  }
}

void EmerioPac125152Climate::sync_all_state_variables_() {
  this->ensure_valid_fan_mode_();

  // Sync tracking variables with ESPHome state
  // CRITICAL: Don't overwrite mode_before_ when OFF or in DRY mode
  if (this->mode != climate::CLIMATE_MODE_OFF && this->mode != climate::CLIMATE_MODE_DRY) {
    this->mode_before_ = this->mode;
    ESP_LOGD(TAG, "Synced mode_before_ to %d", to_internal_mode(this->mode_before_));
  }

  this->fan_mode_before_ = this->fan_mode.value();
  this->target_temperature_before_ = this->target_temperature;
  this->prev_dehumidify_ = (this->mode == climate::CLIMATE_MODE_DRY);
  this->prev_on_off_ = (this->mode != climate::CLIMATE_MODE_OFF);

  ESP_LOGD(TAG, "Synced state: mode=%d, fan=%d, temp=%.1f, dehumidify=%s, on_off=%s",
           to_internal_mode(this->mode_before_), to_internal_fan(this->fan_mode_before_),
           this->target_temperature_before_, this->prev_dehumidify_ ? "true" : "false",
           this->prev_on_off_ ? "true" : "false");
}

// ============================================================================
// SETUP AND INITIALIZATION
// ============================================================================

EmerioPac125152Climate::EmerioPac125152Climate()
    : climate_ir::ClimateIR(TEMP_MIN, TEMP_MAX, 1.0f, true, true,
                            {climate::CLIMATE_FAN_LOW, climate::CLIMATE_FAN_MEDIUM, climate::CLIMATE_FAN_HIGH}) {}

bool EmerioPac125152Climate::validate_device_state_(const EmerioPac125152ClimateDeviceRestoreState &state) {
  // Validate mode
  if (state.prev_mode != climate::CLIMATE_MODE_OFF && state.prev_mode != climate::CLIMATE_MODE_AUTO &&
      state.prev_mode != climate::CLIMATE_MODE_COOL && state.prev_mode != climate::CLIMATE_MODE_DRY &&
      state.prev_mode != climate::CLIMATE_MODE_FAN_ONLY) {
    ESP_LOGW(TAG, "Invalid device mode %d in saved state", to_internal_mode(state.prev_mode));
    return false;
  }

  // Validate fan mode
  if (!this->is_fan_mode_supported_(state.prev_fan)) {
    ESP_LOGW(TAG, "Invalid device fan %d in saved state", to_internal_fan(state.prev_fan));
    return false;
  }

  // Validate temperature
  if (state.prev_temp < TEMP_MIN || state.prev_temp > TEMP_MAX) {
    ESP_LOGW(TAG, "Invalid device temperature %.1f in saved state", state.prev_temp);
    return false;
  }

  return true;
}

void EmerioPac125152Climate::apply_device_state_(const EmerioPac125152ClimateDeviceRestoreState &state) {
  this->mode_before_ = state.prev_mode;
  this->fan_mode_before_ = state.prev_fan;
  this->target_temperature_before_ = state.prev_temp;
  this->prev_dehumidify_ = state.prev_dehumidify;
  this->prev_on_off_ = state.prev_on_off;

  ESP_LOGI(TAG, "Restored device state: mode=%d, fan=%d, temp=%.1f, on_off=%s", to_internal_mode(this->mode_before_),
           to_internal_fan(this->fan_mode_before_), this->target_temperature_before_,
           this->prev_on_off_ ? "ON" : "OFF");
}

void EmerioPac125152Climate::initialize_default_device_state_() {
  // Initialize from ESPHome's restored state (or sensible defaults)
  this->mode_before_ = (this->mode != climate::CLIMATE_MODE_OFF && this->mode != climate::CLIMATE_MODE_DRY)
                           ? this->mode
                           : climate::CLIMATE_MODE_AUTO;
  this->fan_mode_before_ = climate::CLIMATE_FAN_LOW;  // Always start with LOW if no device state
  this->target_temperature_before_ = this->target_temperature;
  this->prev_dehumidify_ = (this->mode == climate::CLIMATE_MODE_DRY);
  this->prev_on_off_ = (this->mode != climate::CLIMATE_MODE_OFF);

  ESP_LOGD(TAG, "Initialized default device state: temp=%.1f", this->target_temperature);
  this->save_emerio_pac_state_();
}

void EmerioPac125152Climate::setup() {
  // Initialize base class first
  climate_ir::ClimateIR::setup();

  // Fix issues from ClimateIR base class
  if (std::isnan(this->target_temperature)) {
    this->target_temperature = TEMP_MIN + 3;  // 18°C default
    ESP_LOGW(TAG, "Fixed NaN temperature, set to %d°C", TEMP_MIN + 3);
  }

  ESP_LOGD(TAG, "ClimateIR set fan_mode to %d (AUTO=%d) - will override",
           (int) this->fan_mode.value_or((climate::ClimateFanMode) -1), (int) climate::CLIMATE_FAN_AUTO);

  // Restore device-specific state
  optional<EmerioPac125152ClimateDeviceRestoreState> device_state = this->restore_emerio_pac_state_();
  if (device_state.has_value() && this->validate_device_state_(device_state.value())) {
    this->apply_device_state_(device_state.value());
  } else {
    if (device_state.has_value()) {
      ESP_LOGW(TAG, "Device state corrupted, using defaults");
    }
    this->initialize_default_device_state_();
  }

  // Override ClimateIR's fan mode with our device state
  this->fan_mode = this->fan_mode_before_;
  ESP_LOGI(TAG, "Set fan_mode to device state: %d", to_internal_fan(this->fan_mode_before_));

  // Sync HA display state with device tracking
  this->sync_display_state_();

  ESP_LOGI(TAG, "Setup complete - HA: mode=%d, fan=%d, temp=%.1f | Device: mode=%d, on_off=%s",
           to_internal_mode(this->mode), to_internal_fan(this->fan_mode.value()), this->target_temperature,
           to_internal_mode(this->mode_before_), this->prev_on_off_ ? "ON" : "OFF");
}

void EmerioPac125152Climate::sync_display_state_() {
  // Ensure HA shows the correct state based on device tracking
  if (!this->prev_on_off_) {
    // Device is OFF - show the settings that will be used when device turns ON
    this->mode = climate::CLIMATE_MODE_OFF;
    this->fan_mode = this->fan_mode_before_;
    this->target_temperature = this->target_temperature_before_;
    ESP_LOGD(TAG, "Device OFF: HA shows ready state (fan=%d, temp=%.1f)", to_internal_fan(this->fan_mode_before_),
             this->target_temperature_before_);
  } else if (this->prev_dehumidify_) {
    // Device is in DRY mode
    this->mode = climate::CLIMATE_MODE_DRY;
    this->fan_mode = this->fan_mode_before_;  // Should be LOW in DRY mode
    this->target_temperature = this->target_temperature_before_;
    ESP_LOGD(TAG, "Device in DRY mode: HA shows DRY state");
  }
  // If device is ON in normal mode, ESPHome's restoration should be correct
}

climate::ClimateTraits EmerioPac125152Climate::traits() {
  auto traits = climate_ir::ClimateIR::traits();
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_AUTO, climate::CLIMATE_MODE_COOL,
                              climate::CLIMATE_MODE_DRY, climate::CLIMATE_MODE_FAN_ONLY});
  return traits;
}

// ============================================================================
// TRANSMIT STATE AND COMMAND HANDLING
// ============================================================================

void EmerioPac125152Climate::send_nec_command_(uint16_t command) {
  // We send 3 repeats to ensure it actually receives the command
  this->transmit_<remote_base::NECProtocol>({ADDRESS, command, 3});
  // Sleep for a short time to allow the device to process the command
  delay(20);
}

void EmerioPac125152Climate::validate_esphome_state_() {
  this->ensure_valid_fan_mode_();

  // Validate temperature
  if (this->target_temperature < TEMP_MIN || this->target_temperature > TEMP_MAX) {
    ESP_LOGW(TAG, "Invalid temperature %.1f! Using device temperature %.1f", this->target_temperature,
             this->target_temperature_before_);
    this->target_temperature = this->target_temperature_before_;
  }
}

bool EmerioPac125152Climate::handle_power_off_() {
  if (this->prev_on_off_) {
    ESP_LOGD(TAG, "Turning off climate");
    send_nec_command_(CMD_POWER);

    // Update tracking state (but keep mode_before_ - hardware remembers it)
    if (this->is_fan_mode_supported_(this->fan_mode.value())) {
      this->fan_mode_before_ = this->fan_mode.value();
    }
    if (this->target_temperature >= TEMP_MIN && this->target_temperature <= TEMP_MAX) {
      this->target_temperature_before_ = this->target_temperature;
    }

    this->prev_dehumidify_ = false;
    this->prev_on_off_ = false;

    ESP_LOGD(TAG, "AC turned OFF, hardware remembers mode: %d", to_internal_mode(this->mode_before_));
    this->publish_state();
    this->save_emerio_pac_state_();
    return true;
  } else {
    // Already OFF - ensure state consistency
    ESP_LOGD(TAG, "AC already OFF, ensuring state consistency");

    bool state_matches = (this->fan_mode.value() == this->fan_mode_before_) &&
                         (abs(this->target_temperature - this->target_temperature_before_) < 0.1f);

    if (!state_matches) {
      ESP_LOGW(TAG, "Correcting state to match device tracking");
      this->fan_mode = this->fan_mode_before_;
      this->target_temperature = this->target_temperature_before_;
    }

    this->prev_dehumidify_ = false;
    this->prev_on_off_ = false;
    this->publish_state();
    this->save_emerio_pac_state_();
    return true;
  }
}

void EmerioPac125152Climate::handle_power_on_() {
  if (!this->prev_on_off_) {
    ESP_LOGD(TAG, "Turning on climate, AC will return to mode: %d", to_internal_mode(this->mode_before_));
    send_nec_command_(CMD_POWER);
    this->prev_on_off_ = true;
  }
}

void EmerioPac125152Climate::transmit_state() {
  this->validate_esphome_state_();

  ESP_LOGD(TAG, "Transmit: mode=%d->%d, fan=%d->%d, temp=%.1f->%.1f, on_off=%s", to_internal_mode(this->mode_before_),
           to_internal_mode(this->mode), to_internal_fan(this->fan_mode_before_),
           to_internal_fan(this->fan_mode.value()), this->target_temperature_before_, this->target_temperature,
           this->prev_on_off_ ? "ON" : "OFF");

  // Handle power state
  if (this->mode == climate::CLIMATE_MODE_OFF) {
    if (this->handle_power_off_())
      return;
  } else {
    this->handle_power_on_();
  }

  // Handle mode and fan changes
  this->handle_mode_and_fan_changes_();

  // Handle temperature changes
  this->handle_temperature_change_();

  // Update tracking state and publish
  this->sync_all_state_variables_();
  this->publish_state();
  this->save_emerio_pac_state_();
}

void EmerioPac125152Climate::handle_mode_and_fan_changes_() {
  bool requested_dehumidify = (this->mode == climate::CLIMATE_MODE_DRY);

  // Handle DRY mode transitions
  if (requested_dehumidify != this->prev_dehumidify_) {
    if (requested_dehumidify) {
      ESP_LOGD(TAG, "Entering DRY mode");
      this->fan_before_dry_ = this->fan_mode_before_;
      send_nec_command_(CMD_DEHUMIDIFY_TOGGLE);

      // AC forces LOW fan in DRY mode
      this->fan_mode_before_ = climate::CLIMATE_FAN_LOW;
      this->prev_dehumidify_ = true;

      this->publish_state();
      this->save_emerio_pac_state_();
      return;  // Exit early for DRY mode
    } else {
      ESP_LOGD(TAG, "Leaving DRY mode");
      send_nec_command_(CMD_MODE);

      // AC returns to AUTO mode and restores fan setting
      this->mode_before_ = climate::CLIMATE_MODE_AUTO;
      this->fan_mode_before_ = this->fan_before_dry_;
      this->prev_dehumidify_ = false;
    }
  }

  // Handle normal mode cycling (AUTO/COOL/FAN)
  if (this->mode != climate::CLIMATE_MODE_DRY && this->mode != this->mode_before_) {
    int steps = (to_internal_mode(this->mode) - to_internal_mode(this->mode_before_) + MODE_COUNT) % MODE_COUNT;
    ESP_LOGD(TAG, "Cycling modes: %d steps", steps);
    for (int i = 0; i < steps; i++) {
      send_nec_command_(CMD_MODE);
    }
  }

  // Handle fan cycling
  int fan_steps =
      (to_internal_fan(this->fan_mode.value()) - to_internal_fan(this->fan_mode_before_) + FAN_COUNT) % FAN_COUNT;
  if (fan_steps > 0) {
    ESP_LOGD(TAG, "Cycling fans: %d steps", fan_steps);
    for (int i = 0; i < fan_steps; i++) {
      send_nec_command_(CMD_FAN_TOGGLE);
    }
  }
}

void EmerioPac125152Climate::handle_temperature_change_() {
  if (!this->prev_on_off_) {
    ESP_LOGD(TAG, "Device OFF, skipping temperature commands");
    return;
  }

  // FAN_ONLY mode doesn't support temperature setpoints
  if (this->mode == climate::CLIMATE_MODE_FAN_ONLY) {
    ESP_LOGD(TAG, "FAN_ONLY mode, skipping temperature commands");
    return;
  }

  int temp_diff = int(roundf(this->target_temperature)) - int(roundf(this->target_temperature_before_));
  if (temp_diff == 0)
    return;

  ESP_LOGD(TAG, "Temperature change: %d steps", temp_diff);

  uint32_t now = millis();
  if (now < this->setpoint_busy_until_) {
    // In busy window - send only one step
    ESP_LOGD(TAG, "Setpoint busy, sending single step");
    send_nec_command_(temp_diff > 0 ? CMD_TEMP_UP : CMD_TEMP_DOWN);
  } else {
    // Send full difference + 1 (device quirk)
    int steps = abs(temp_diff) + 1;
    uint16_t cmd = temp_diff > 0 ? CMD_TEMP_UP : CMD_TEMP_DOWN;

    ESP_LOGD(TAG, "Sending %d temperature commands", steps);
    for (int i = 0; i < steps; i++) {
      send_nec_command_(cmd);
    }

    this->setpoint_busy_until_ = now + 6000;  // 6 second busy window
  }
}

// ============================================================================
// CALIBRATION AND CONTROL
// ============================================================================

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
  bool device_currently_off = (this->mode == climate::CLIMATE_MODE_OFF);
  bool fan_change = call.get_fan_mode().has_value();
  bool temp_change = call.get_target_temperature().has_value();
  bool mode_change = call.get_mode().has_value();

  // Reject unsupported fan modes early
  if (fan_change && !this->is_fan_mode_supported_(call.get_fan_mode().value())) {
    ESP_LOGW(TAG, "Rejecting unsupported fan mode %d (AUTO=%d)", (int) call.get_fan_mode().value(),
             (int) climate::CLIMATE_FAN_AUTO);
    this->publish_state();
    return;
  }

  // Reject temperature changes in FAN_ONLY mode
  if (temp_change && this->mode == climate::CLIMATE_MODE_FAN_ONLY) {
    ESP_LOGW(TAG, "FAN_ONLY mode: ignoring temperature setpoint changes");
    this->publish_state();
    return;
  }

  // Prevent desync when device is OFF
  if (device_currently_off && (fan_change || temp_change)) {
    bool turning_on = mode_change && call.get_mode().value() != climate::CLIMATE_MODE_OFF;

    if (!turning_on) {
      ESP_LOGW(TAG, "Device OFF: ignoring fan/temperature changes to prevent desync");
      climate_ir::ClimateIR::control(call);

      // Maintain device state when OFF
      this->fan_mode = this->fan_mode_before_;
      this->target_temperature = this->target_temperature_before_;
      this->publish_state();
      return;
    }
  }

  // Process the call through base class
  climate_ir::ClimateIR::control(call);

  // Validate and correct any unsupported fan modes set by base class
  this->ensure_valid_fan_mode_();
}
}  // namespace emerio_pac_125152
}  // namespace esphome
