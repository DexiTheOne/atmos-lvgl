#include "scd30.h"
#include <algorithm>
#include <cmath>
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#ifdef USE_ESP8266
#include <Wire.h>
#endif

namespace esphome::scd30 {

static const char *const TAG = "scd30";

static const uint16_t SCD30_CMD_GET_FIRMWARE_VERSION = 0xd100;
static const uint16_t SCD30_CMD_START_CONTINUOUS_MEASUREMENTS = 0x0010;
static const uint16_t SCD30_CMD_ALTITUDE_COMPENSATION = 0x5102;
static const uint16_t SCD30_CMD_AUTOMATIC_SELF_CALIBRATION = 0x5306;
static const uint16_t SCD30_CMD_GET_DATA_READY_STATUS = 0x0202;
static const uint16_t SCD30_CMD_READ_MEASUREMENT = 0x0300;

/// Commands for future use
static const uint16_t SCD30_CMD_STOP_MEASUREMENTS = 0x0104;
static const uint16_t SCD30_CMD_MEASUREMENT_INTERVAL = 0x4600;
static const uint16_t SCD30_CMD_FORCED_CALIBRATION = 0x5204;
static const uint16_t SCD30_CMD_TEMPERATURE_OFFSET = 0x5403;
static const uint16_t SCD30_CMD_SOFT_RESET = 0xD304;

void SCD30Component::setup() {
#ifdef USE_ESP8266
  Wire.setClockStretchLimit(150000);
#endif

  /// Firmware version identification
  uint16_t raw_firmware_version[3];
  if (!this->get_register(SCD30_CMD_GET_FIRMWARE_VERSION, raw_firmware_version, 3)) {
    this->error_code_ = FIRMWARE_IDENTIFICATION_FAILED;
    this->mark_failed();
    return;
  }
  ESP_LOGD(TAG, "SCD30 Firmware v%0d.%02d", (uint16_t(raw_firmware_version[0]) >> 8),
           uint16_t(raw_firmware_version[0] & 0xFF));

  uint16_t temp_offset;
  if (this->temperature_offset_ > 0) {
    temp_offset = (this->temperature_offset_ * 100);
  } else {
    temp_offset = 0;
  }

  if (!this->write_command(SCD30_CMD_TEMPERATURE_OFFSET, temp_offset)) {
    ESP_LOGE(TAG, "Sensor SCD30 error setting temperature offset.");
    this->error_code_ = MEASUREMENT_INIT_FAILED;
    this->mark_failed();
    return;
  }
#ifdef USE_ESP32
  // According ESP32 clock stretching is typically 30ms and up to 150ms "due to
  // internal calibration processes". The I2C peripheral only supports 13ms (at
  // least when running at 80MHz).
  // In practice it seems that clock stretching occurs during this calibration
  // calls. It also seems that delays in between calls makes them
  // disappear/shorter. Hence work around with delays for ESP32.
  //
  // By experimentation a delay of 20ms as already sufficient. Let's go
  // safe and use 30ms delays.
  delay(30);
#endif

  if (!this->write_command(SCD30_CMD_MEASUREMENT_INTERVAL, update_interval_)) {
    ESP_LOGE(TAG, "Sensor SCD30 error setting update interval.");
    this->error_code_ = MEASUREMENT_INIT_FAILED;
    this->mark_failed();
    return;
  }
#ifdef USE_ESP32
  delay(30);
#endif

  // The start measurement command disables the altitude compensation, if any, so we only set it if it's turned on
  if (this->altitude_compensation_ != 0xFFFF) {
    if (!this->write_command(SCD30_CMD_ALTITUDE_COMPENSATION, altitude_compensation_)) {
      ESP_LOGE(TAG, "Sensor SCD30 error setting altitude compensation.");
      this->error_code_ = MEASUREMENT_INIT_FAILED;
      this->mark_failed();
      return;
    }
  }
#ifdef USE_ESP32
  delay(30);
#endif

  // atmos: write ASC only when the sensor's stored setting differs, so a reboot
  // never resends it (re-enabling may restart ASC's initial period).
  if (!this->sync_asc_()) {
    ESP_LOGE(TAG, "Sensor SCD30 error setting automatic self calibration.");
    this->error_code_ = MEASUREMENT_INIT_FAILED;
    this->mark_failed();
    return;
  }
#ifdef USE_ESP32
  delay(30);
#endif

  /// Sensor initialization
  // atmos: start with the pressure source's reading when it already has one.
  if (this->pressure_source_ != nullptr && this->pressure_source_->has_state()) {
    float p = this->pressure_source_->state;
    if (p >= 700.0f && p <= 1400.0f)
      this->ambient_pressure_compensation_ = (uint16_t) lroundf(p);
  }
  if (!this->write_command(SCD30_CMD_START_CONTINUOUS_MEASUREMENTS, this->ambient_pressure_compensation_)) {
    ESP_LOGE(TAG, "Sensor SCD30 error starting continuous measurements.");
    this->error_code_ = MEASUREMENT_INIT_FAILED;
    this->mark_failed();
    return;
  }

  // check each 500ms if data is ready, and read it in that case
  this->sensor_restarted_();
  this->calibration_publish_("Idle");

  this->set_interval("status-check", 500, [this]() {
    this->apply_pressure_source_();
    if (this->is_data_ready_())
      this->update();
  });
}

void SCD30Component::dump_config() {
  ESP_LOGCONFIG(TAG, "scd30:");
  LOG_I2C_DEVICE(this);
  if (this->is_failed()) {
    switch (this->error_code_) {
      case COMMUNICATION_FAILED:
        ESP_LOGW(TAG, ESP_LOG_MSG_COMM_FAIL);
        break;
      case MEASUREMENT_INIT_FAILED:
        ESP_LOGW(TAG, "Measurement Initialization failed");
        break;
      case FIRMWARE_IDENTIFICATION_FAILED:
        ESP_LOGW(TAG, "Unable to read sensor firmware version");
        break;
      default:
        ESP_LOGW(TAG, "Unknown setup error");
        break;
    }
  }
  if (this->altitude_compensation_ == 0xFFFF) {
    ESP_LOGCONFIG(TAG, "  Altitude compensation: OFF");
  } else {
    ESP_LOGCONFIG(TAG, "  Altitude compensation: %dm", this->altitude_compensation_);
  }
  ESP_LOGCONFIG(TAG,
                "  Automatic self calibration: %s\n"
                "  Ambient pressure compensation: %dmBar\n"
                "  Temperature offset: %.2f °C\n"
                "  Update interval: %ds",
                ONOFF(this->enable_asc_), this->ambient_pressure_compensation_, this->temperature_offset_,
                this->update_interval_);
  LOG_SENSOR("  ", "CO2", this->co2_sensor_);
  LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
  LOG_SENSOR("  ", "Humidity", this->humidity_sensor_);
  LOG_SENSOR("  ", "Pressure source", this->pressure_source_);
}

void SCD30Component::update() {
  uint16_t raw_read_status;
  if (!this->read_data(raw_read_status) || raw_read_status == 0x00) {
    this->status_set_warning();
    ESP_LOGW(TAG, "Data not ready yet!");
    return;
  }

  if (!this->write_command(SCD30_CMD_READ_MEASUREMENT)) {
    ESP_LOGW(TAG, "Error reading measurement!");
    this->status_set_warning();
    return;
  }

  this->set_timeout(50, [this]() {
    uint16_t raw_data[6];
    if (!this->read_data(raw_data, 6)) {
      this->status_set_warning();
      return;
    }

    union uint32_float_t {
      uint32_t uint32;
      float value;
    };
    uint32_t temp_c_o2_u32 = (((uint32_t(raw_data[0])) << 16) | (uint32_t(raw_data[1])));
    uint32_float_t co2{.uint32 = temp_c_o2_u32};

    uint32_t temp_temp_u32 = (((uint32_t(raw_data[2])) << 16) | (uint32_t(raw_data[3])));
    uint32_float_t temperature{.uint32 = temp_temp_u32};

    uint32_t temp_hum_u32 = (((uint32_t(raw_data[4])) << 16) | (uint32_t(raw_data[5])));
    uint32_float_t humidity{.uint32 = temp_hum_u32};

    ESP_LOGD(TAG, "Got CO2=%.2fppm temperature=%.2f°C humidity=%.2f%%", co2.value, temperature.value, humidity.value);
    if (this->co2_sensor_ != nullptr)
      this->co2_sensor_->publish_state(co2.value);
    if (this->temperature_sensor_ != nullptr)
      this->temperature_sensor_->publish_state(temperature.value);
    if (this->humidity_sensor_ != nullptr)
      this->humidity_sensor_->publish_state(humidity.value);
    this->calibration_sample_(co2.value, humidity.value);

    this->status_clear_warning();
  });
}

bool SCD30Component::is_data_ready_() {
  if (!this->write_command(SCD30_CMD_GET_DATA_READY_STATUS)) {
    return false;
  }
  delay(4);
  uint16_t is_data_ready;
  if (!this->read_data(&is_data_ready, 1)) {
    return false;
  }
  return is_data_ready == 1;
}

bool SCD30Component::force_recalibration_with_reference(uint16_t co2_reference) {
  ESP_LOGD(TAG, "Performing CO2 force recalibration with reference %dppm.", co2_reference);
  if (this->write_command(SCD30_CMD_FORCED_CALIBRATION, co2_reference)) {
    ESP_LOGD(TAG, "Force recalibration complete.");
    return true;
  } else {
    ESP_LOGE(TAG, "Failed to force recalibration with reference.");
    this->error_code_ = FORCE_RECALIBRATION_FAILED;
    this->status_set_warning();
    return false;
  }
}

uint16_t SCD30Component::get_forced_calibration_reference() {
  uint16_t forced_calibration_reference = 0;
  // Get current CO2 calibration
  if (!this->get_register(SCD30_CMD_FORCED_CALIBRATION, forced_calibration_reference)) {
    ESP_LOGE(TAG, "Unable to read forced calibration reference.");
  }
  return forced_calibration_reference;
}

bool SCD30Component::sync_asc_() {
  uint16_t stored;
  if (this->get_register(SCD30_CMD_AUTOMATIC_SELF_CALIBRATION, stored)) {
    if ((stored != 0) == this->enable_asc_) {
      ESP_LOGI(TAG, "ASC already %s; not rewritten", ONOFF(this->enable_asc_));
      return true;
    }
  } else {
    ESP_LOGW(TAG, "Unable to read ASC setting; writing it");
  }
#ifdef USE_ESP32
  delay(30);
#endif
  ESP_LOGI(TAG, "Setting ASC %s", ONOFF(this->enable_asc_));
  this->sensor_restarted_();
  return this->write_command(SCD30_CMD_AUTOMATIC_SELF_CALIBRATION, this->enable_asc_ ? 1 : 0);
}

void SCD30Component::apply_automatic_self_calibration(bool asc) {
  this->enable_asc_ = asc;
  if (this->is_failed() || !this->is_ready())
    return;  // setup applies enable_asc_
  if (!this->sync_asc_())
    ESP_LOGE(TAG, "Unable to set ASC");
}
// atmos: send the pressure source's reading (hPa = mbar) to the sensor when it has
// moved by at least 1 mbar. Re-sending the start command updates the compensation
// without interrupting measurement; the range is the SCD30's (700-1400 mbar).
void SCD30Component::apply_pressure_source_() {
  if (this->pressure_source_ == nullptr || !this->pressure_source_->has_state())
    return;
  float p = this->pressure_source_->state;
  if (!(p >= 700.0f && p <= 1400.0f))
    return;  // also rejects NaN
  uint16_t mbar = (uint16_t) lroundf(p);
  if (mbar == this->ambient_pressure_compensation_)
    return;
  if (this->write_command(SCD30_CMD_START_CONTINUOUS_MEASUREMENTS, mbar)) {
    // INFO for the first reading after boot (the device logs at INFO), DEBUG after.
    if (this->pressure_sent_) {
      ESP_LOGD(TAG, "Ambient pressure compensation %u -> %u mbar", this->ambient_pressure_compensation_, mbar);
    } else {
      ESP_LOGI(TAG, "Ambient pressure compensation from source: %u mbar", mbar);
      this->pressure_sent_ = true;
    }
    this->ambient_pressure_compensation_ = mbar;
    this->sensor_restarted_();  // treated as a restart: the calibration window starts again
  } else {
    ESP_LOGW(TAG, "Unable to set ambient pressure compensation");
  }
}

// atmos: guarded forced calibration (see scd30.h).
static const uint32_t CAL_RESTART_DIP_MS = 120000;  // readings dip for 1-2 min after a restart
static const uint32_t CAL_VERIFY_MS = 60000;         // averaged after the dip
static const float CAL_VERIFY_TOLERANCE = 30.0f;     // ppm

void SCD30Component::calibration_publish_(const std::string &text) {
  ESP_LOGI(TAG, "Calibration: %s", text.c_str());
  if (this->cal_status_ != nullptr)
    this->cal_status_->publish_state(text);
}

void SCD30Component::start_calibration() {
  if (this->is_failed()) {
    this->calibration_publish_("Failed (sensor not running)");
    return;
  }
  this->cal_state_ = CAL_SETTLING;
  this->cal_start_ms_ = millis();
  this->window_start_ms_ = 0;
  this->last_minutes_ = -1;
  this->calibration_publish_("Waiting for steady air");
}

void SCD30Component::cancel_calibration() {
  if (this->cal_state_ == CAL_IDLE)
    return;
  this->cal_state_ = CAL_IDLE;
  this->calibration_publish_("Cancelled");
}

void SCD30Component::calibration_sample_(float co2, float rh) {
  if (this->cal_state_ == CAL_IDLE || !std::isfinite(co2) || !std::isfinite(rh))
    return;
  const uint32_t now = millis();
  char buf[96];

  if (this->cal_state_ == CAL_SETTLING) {
    if (now - this->cal_start_ms_ > this->cal_timeout_ms_) {
      this->cal_state_ = CAL_IDLE;
      snprintf(buf, sizeof buf, "Timed out (not steady for %u min)", (unsigned) (this->cal_settle_ms_ / 60000));
      this->calibration_publish_(buf);
      return;
    }
    if (now - this->restart_ms_ < CAL_RESTART_DIP_MS) {
      if (this->window_start_ms_ != 0 || this->last_minutes_ != -2)
        this->calibration_publish_("Waiting for sensor restart");
      this->window_start_ms_ = 0;
      this->last_minutes_ = -2;
      return;
    }
    bool restart = this->window_start_ms_ == 0;
    if (!restart) {
      float cmin = std::min(this->win_co2_min_, co2), cmax = std::max(this->win_co2_max_, co2);
      float rmin = std::min(this->win_rh_min_, rh), rmax = std::max(this->win_rh_max_, rh);
      if (cmax - cmin > this->cal_co2_spread_ || rmax - rmin > this->cal_rh_spread_) {
        restart = true;
      } else {
        this->win_co2_min_ = cmin, this->win_co2_max_ = cmax, this->win_rh_min_ = rmin, this->win_rh_max_ = rmax;
        this->win_co2_sum_ += co2;
        this->win_count_++;
      }
    }
    if (restart) {  // a new steady window starts with this reading
      this->window_start_ms_ = now;
      this->win_co2_min_ = this->win_co2_max_ = co2;
      this->win_rh_min_ = this->win_rh_max_ = rh;
      this->win_co2_sum_ = co2;
      this->win_count_ = 1;
    }
    const uint32_t steady = now - this->window_start_ms_;
    if (steady >= this->cal_settle_ms_) {
      this->cal_before_ = this->win_co2_sum_ / (float) this->win_count_;
      this->cal_state_ = CAL_VERIFYING;
      this->calibration_publish_("Applying");
      this->set_timeout("cal-apply", 50, [this]() {
        if (!this->force_recalibration_with_reference(this->cal_reference_)) {
          this->cal_state_ = CAL_IDLE;
          this->calibration_publish_("Failed (sensor rejected the command)");
          return;
        }
        this->sensor_restarted_();
        this->ver_sum_ = 0;
        this->ver_count_ = 0;
        this->calibration_publish_("Applied; verifying");
      });
      return;
    }
    int minutes = (int) (steady / 60000);
    if (minutes != this->last_minutes_) {
      this->last_minutes_ = minutes;
      snprintf(buf, sizeof buf, "Settling %d/%u min", minutes, (unsigned) (this->cal_settle_ms_ / 60000));
      this->calibration_publish_(buf);
    }
    return;
  }

  // CAL_VERIFYING: skip the post-calibration dip, then average for CAL_VERIFY_MS.
  if (now - this->restart_ms_ < CAL_RESTART_DIP_MS)
    return;
  this->ver_sum_ += co2;
  this->ver_count_++;
  if (now - this->restart_ms_ < CAL_RESTART_DIP_MS + CAL_VERIFY_MS)
    return;
  float after = this->ver_sum_ / (float) this->ver_count_;
  this->cal_state_ = CAL_IDLE;
  if (std::fabs(after - (float) this->cal_reference_) <= CAL_VERIFY_TOLERANCE) {
    snprintf(buf, sizeof buf, "Calibrated (was %d, now %d ppm)", (int) lroundf(this->cal_before_), (int) lroundf(after));
  } else {
    snprintf(buf, sizeof buf, "Check reading (%d ppm after calibrating to %u)", (int) lroundf(after),
             (unsigned) this->cal_reference_);
  }
  this->calibration_publish_(buf);
}

}  // namespace esphome::scd30