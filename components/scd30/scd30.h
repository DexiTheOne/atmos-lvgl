#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/hal.h"
#include <string>
#include "esphome/components/sensirion_common/i2c_sensirion.h"

namespace esphome::scd30 {

/// This class implements support for the Sensirion scd30 i2c GAS (VOC and CO2eq) sensors.
class SCD30Component final : public Component, public sensirion_common::SensirionI2CDevice {
 public:
  void set_co2_sensor(sensor::Sensor *co2) { co2_sensor_ = co2; }
  void set_humidity_sensor(sensor::Sensor *humidity) { humidity_sensor_ = humidity; }
  void set_temperature_sensor(sensor::Sensor *temperature) { temperature_sensor_ = temperature; }
  void set_automatic_self_calibration(bool asc) { enable_asc_ = asc; }
  /// Set ASC at run time; writes the sensor only when its stored setting differs.
  void apply_automatic_self_calibration(bool asc);
  void set_altitude_compensation(uint16_t altitude) { altitude_compensation_ = altitude; }
  void set_ambient_pressure_compensation(float pressure) {
    ambient_pressure_compensation_ = (uint16_t) (pressure * 1000);
  }
  void set_temperature_offset(float offset) { temperature_offset_ = offset; }
  /// atmos: pressure sensor (hPa = mbar) fed to the SCD30 whenever it changes by 1 mbar.
  void set_ambient_pressure_source(sensor::Sensor *source) { pressure_source_ = source; }
  void set_update_interval(uint16_t interval) { update_interval_ = interval; }
  bool force_recalibration_with_reference(uint16_t co2_reference);
  uint16_t get_forced_calibration_reference();

  /// atmos: guarded forced calibration. start_calibration() waits until CO2 and
  /// humidity have held within their spreads for settle_time (counting only after
  /// the sensor's restart dip), applies the reference, then verifies the readings.
  /// It gives up after timeout. Progress is published to the status text sensor.
  void start_calibration();
  void cancel_calibration();
  void set_calibration_status(text_sensor::TextSensor *status) { cal_status_ = status; }
  void set_calibration_reference(uint16_t ppm) { cal_reference_ = ppm; }
  void set_calibration_settle_time(uint32_t ms) { cal_settle_ms_ = ms; }
  void set_calibration_timeout(uint32_t ms) { cal_timeout_ms_ = ms; }
  void set_calibration_co2_spread(float ppm) { cal_co2_spread_ = ppm; }
  void set_calibration_humidity_spread(float rh) { cal_rh_spread_ = rh; }

  void setup() override;
  void update();
  void dump_config() override;

 protected:
  bool is_data_ready_();
  bool sync_asc_();
  void apply_pressure_source_();
  void calibration_sample_(float co2, float rh);
  void calibration_publish_(const std::string &text);
  void sensor_restarted_() { this->restart_ms_ = millis(); }

  enum CalState { CAL_IDLE, CAL_SETTLING, CAL_VERIFYING } cal_state_{CAL_IDLE};
  text_sensor::TextSensor *cal_status_{nullptr};
  uint16_t cal_reference_{420};
  uint32_t cal_settle_ms_{600000};
  uint32_t cal_timeout_ms_{3600000};
  float cal_co2_spread_{20.0f};
  float cal_rh_spread_{2.0f};
  uint32_t restart_ms_{0};      // last measurement restart (boot, pressure, calibration)
  uint32_t cal_start_ms_{0};    // button press
  uint32_t window_start_ms_{0}; // start of the current steady window (0 = none)
  float win_co2_min_{0}, win_co2_max_{0}, win_rh_min_{0}, win_rh_max_{0};
  float win_co2_sum_{0};
  uint32_t win_count_{0};
  float cal_before_{0};         // settled mean before applying
  float ver_sum_{0};
  uint32_t ver_count_{0};
  int last_minutes_{-1};

  enum ErrorCode {
    COMMUNICATION_FAILED,
    FIRMWARE_IDENTIFICATION_FAILED,
    MEASUREMENT_INIT_FAILED,
    FORCE_RECALIBRATION_FAILED,
    UNKNOWN
  } error_code_{UNKNOWN};
  bool enable_asc_{true};
  uint16_t altitude_compensation_{0xFFFF};
  uint16_t ambient_pressure_compensation_{0x0000};
  float temperature_offset_{0.0};
  uint16_t update_interval_{0xFFFF};

  sensor::Sensor *co2_sensor_{nullptr};
  sensor::Sensor *pressure_source_{nullptr};
  bool pressure_sent_{false};
  sensor::Sensor *humidity_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};
};

}  // namespace esphome::scd30
