#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/st7305_rlcd/st7305_rlcd.h"
#include "lvgl.h"

namespace esphome::atmos {

/* Frame statistics for one class of frames (still scenes or transitions). */
struct FrameStats {
  uint32_t frames{0};
  uint32_t late{0};  // presentation interval > 1.5 frame periods (a dropped 40 fps slot)
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
class AtmosComponent : public Component {
 public:
  void set_display(st7305_rlcd::ST7305RLCD *display) { this->panel_ = display; }
  void set_frame_period_us(uint32_t us) { this->period_us_ = us; }
  void set_report_interval_ms(uint32_t ms) { this->report_ms_ = ms; }
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }
  void setup() override;
  void loop() override;
  void dump_config() override;

 protected:
  static void flush_cb_(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
  static void display_event_(lv_event_t *e);
  void on_display_event_(lv_event_code_t code);
  void report_();

  st7305_rlcd::ST7305RLCD *panel_{nullptr};
  lv_display_t *disp_{nullptr};
  uint32_t period_us_{25000};
  uint32_t report_ms_{10000};
  bool started_{false};
  uint32_t next_step_us_{0};
  uint32_t last_report_ms_{0};
  uint32_t missed_steps_{0};
  HighFrequencyLoopRequester high_freq_;

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
};

}  // namespace esphome::atmos
