#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/st7305_rlcd/st7305_rlcd.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/preferences.h"
#include "lvgl.h"

namespace esphome::atmos {

/* Frame statistics for one class of frames (still scenes or transitions). */
struct FrameStats {
  uint32_t frames{0};
  uint32_t late{0};  // presentation interval > 1.5 frame periods (a dropped frame slot)
  uint64_t interval_sum{0};
  uint32_t interval_max{0};
  uint64_t render_sum{0}, convert_sum{0}, push_sum{0};
  uint32_t render_max{0}, convert_max{0}, push_max{0};
  void add(uint32_t interval, uint32_t render, uint32_t convert, uint32_t push, uint32_t period);
  void log(const char *name, uint32_t period) const;
};

/* Owns LVGL (ESPHome's lvgl: component is not used) with a native 1-bit display:
   LVGL renders an I1 frame in internal RAM, flush converts the touched rows to the
   ST7305 layout and pushes the panel buffer over SPI. */
class AtmosReadingSelect;
class AtmosAutoSwitch;

class AtmosComponent : public Component {
 public:
  void set_display(st7305_rlcd::ST7305RLCD *display) { this->panel_ = display; }
  void set_frame_period_us(uint32_t us) { this->period_us_ = us; }
  void set_report_interval_ms(uint32_t ms) { this->report_ms_ = ms; }
  void set_frame_stats(bool on) { this->frame_stats_ = on; }
  void set_key_interval_ms(uint32_t ms) { this->key_interval_ms_ = ms; }
  void set_splash_name(const char *name) { this->splash_name_ = name; }
  void set_splash_subtitle(const char *text) { this->splash_subtitle_ = text; }
  void set_splash_timeout_ms(uint32_t ms) { this->splash_timeout_ms_ = ms; }
  // After the display (PROCESSOR) so the panel is ready for the splash; before the
  // number and switch entities (PROCESSOR - 1), which apply settings to it.
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 0.5f; }
  void setup() override;
  void dump_config() override;
  void advance(int direction);

  // Home Assistant controls. Settings made before the UI starts are applied when it does.
  void set_reading_select(AtmosReadingSelect *s) { this->reading_select_ = s; }
  void set_auto_switch(AtmosAutoSwitch *s) { this->auto_switch_ = s; }
  /* Select option: 0 "Auto advance" (turns cycling on), else reading option - 1. */
  void select_option(int option);
  void set_auto(bool on);
  void set_hold(float seconds);
  void set_history_window(float seconds);
  /* Rating band for a reading (atmos_band); bands for a reading are applied in order. */
  void add_band(int reading, float min, float max, int level, const char *text);
  /* Link a sensor to a reading (ATMOS_CO2 ...). Linking any replaces the demo data. */
  void set_sensor(int reading, sensor::Sensor *s);

 protected:
  static void flush_cb_(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
  static void display_event_(lv_event_t *e);
  void on_display_event_(lv_event_code_t code);
  void frame_();
  void wait_ready_();
  bool ready_() const;
  void report_();

  st7305_rlcd::ST7305RLCD *panel_{nullptr};
  lv_display_t *disp_{nullptr};
  uint32_t period_us_{59000};
  uint32_t report_ms_{10000};
  bool started_{false};
  const char *splash_name_{"Atmos"};
  const char *splash_subtitle_{"Visualizing Air Quality"};
  uint32_t splash_timeout_ms_{20000};
  uint32_t splash_ms_{0};  // when the splash was shown
  bool frame_stats_{false};
  uint32_t key_interval_ms_{250};
  uint32_t last_key_ms_{0};
  bool key_pressed_{false};
  uint32_t next_step_us_{0};
  uint32_t last_report_ms_{0};
  uint32_t missed_steps_{0};
  uint64_t busy_us_{0};  // time spent in frame_() since the last report
  uint32_t report_start_ms_{0};

  // Per-frame timing (micros()).
  uint32_t frame_start_{0}, last_present_{0};
  uint32_t convert_us_{0}, push_us_{0}, render_us_{0};
  bool frame_moving_{false}, frame_flushed_{false};
  FrameStats still_, transition_, window_still_, window_transition_;

  bool was_moving_{false};
  uint32_t transition_index_{0};
  FrameStats current_transition_;
  // Hardware pacing profile (for the simulator's --hw-pace): average work per frame
  // (REFR_START to REFR_READY) for still frames per scene and transitions per source
  // scene, and the atmos_step() time of hold-time pre-renders per rasterised scene.
  uint64_t still_cost_[6]{}, trans_cost_[6]{}, pre_cost_[6]{};
  uint32_t still_n_[6]{}, trans_n_[6]{}, pre_n_[6]{};
  int frame_scene_{0};

  AtmosReadingSelect *reading_select_{nullptr};
  AtmosAutoSwitch *auto_switch_{nullptr};
  int pending_reading_{-1}, published_option_{-1};
  // While cycling, the select shows a reading only while the reading chosen by the
  // select or a key (manual_) is on screen, otherwise "Auto advance", so cycling
  // does not record a state change per step. A key press during a transition takes
  // effect on arrival: adopt_ waits for the reading to move off adopt_from_.
  int manual_{-1}, adopt_from_{-1};
  bool adopt_{false};
  bool auto_{true};
  void publish_selection_();
  float values_[16];  // latest value per reading, NAN until reported
  bool has_sensors_{false}, values_dirty_{false};
  void push_readings_();
};

/* Drop-down of "Auto advance" and the readings: choosing a reading moves to its page and
   underlines/charts it; choosing "Auto advance" turns cycling on. */
class AtmosReadingSelect : public select::Select, public Parented<AtmosComponent> {
 protected:
  void control(size_t index) override { this->parent_->select_option((int) index); }
};

class AtmosAutoSwitch : public switch_::Switch, public Component, public Parented<AtmosComponent> {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 1; }

 protected:
  void write_state(bool state) override;
};

/* Page hold or history span, in the entity's unit (scale = seconds per unit). Restored
   from flash across reboots. */
class AtmosNumber : public number::Number, public Component, public Parented<AtmosComponent> {
 public:
  void set_history(bool history) { this->history_ = history; }
  void set_scale(float scale) { this->scale_ = scale; }
  void set_initial_value(float value) { this->initial_ = value; }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 1; }

 protected:
  void control(float value) override;
  bool history_{false};
  float scale_{1}, initial_{0};
  ESPPreferenceObject pref_;
};

/* atmos.next / atmos.previous: one key press. Runs in the main loop task, as the
   frame timeout does, so it may call into the UI and LVGL directly. */
template<typename... Ts> class AdvanceAction : public Action<Ts...>, public Parented<AtmosComponent> {
 public:
  void set_direction(int direction) { this->direction_ = direction; }
  void play(const Ts &...x) override { this->parent_->advance(this->direction_); }

 protected:
  int direction_{1};
};

}  // namespace esphome::atmos
