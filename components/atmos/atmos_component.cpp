#include "atmos_component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif
#ifdef USE_NETWORK
#include "esphome/components/network/util.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#include <esp_heap_caps.h>

extern "C" {
#include "../../../atmos_ui/atmos.h"  // copied from desktop/ by __init__.py
}

namespace esphome::atmos {

static const char *const TAG = "atmos";

/* LVGL's 1-bit frame: an 8-byte I1 palette, then 300 rows of 50 bytes (bit 1 = white).
   Static, so it lives in internal RAM. */
alignas(32) static uint8_t frame_buf[8 + 400 / 8 * 300];
static uint32_t mark_us;  // start of the current render/convert segment

void FrameStats::add(uint32_t interval, uint32_t render, uint32_t convert, uint32_t push, uint32_t period) {
  this->frames++;
  if (interval * 2 > period * 3)
    this->late++;
  this->interval_sum += interval;
  this->interval_max = std::max(this->interval_max, interval);
  this->render_sum += render;
  this->convert_sum += convert;
  this->push_sum += push;
  this->render_max = std::max(this->render_max, render);
  this->convert_max = std::max(this->convert_max, convert);
  this->push_max = std::max(this->push_max, push);
}

void FrameStats::log(const char *name, uint32_t period) const {
  if (this->frames == 0) {
    ESP_LOGI(TAG, "%-10s no frames", name);
    return;
  }
  uint32_t n = this->frames;
  ESP_LOGI(TAG,
           "%-10s %4u frames  %5.1f fps  interval avg %5.1f max %5.1f ms  late(>%.1fms) %u  |  "
           "render avg %5.1f max %5.1f  convert avg %5.2f max %5.2f  push avg %5.1f max %5.1f ms",
           name, (unsigned) n, n * 1e6f / (float) this->interval_sum, this->interval_sum / 1000.0f / n,
           this->interval_max / 1000.0f, period * 1.5f / 1000.0f, (unsigned) this->late,
           this->render_sum / 1000.0f / n, this->render_max / 1000.0f, this->convert_sum / 1000.0f / n,
           this->convert_max / 1000.0f, this->push_sum / 1000.0f / n, this->push_max / 1000.0f);
}

void AtmosComponent::setup() {
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  this->disp_ = lv_display_create(400, 300);
  lv_display_set_color_format(this->disp_, LV_COLOR_FORMAT_I1);
  lv_display_set_buffers(this->disp_, frame_buf, nullptr, sizeof frame_buf, LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(this->disp_, flush_cb_);
  lv_display_set_user_data(this->disp_, this);
  if (this->frame_stats_)
    for (auto code : {LV_EVENT_REFR_START, LV_EVENT_REFR_READY})
      lv_display_add_event_cb(this->disp_, display_event_, code, this);
  this->report_start_ms_ = millis();
  // The splash is cheap to draw, so it goes up at once; the UI, whose first frame is
  // far more work, waits until boot has finished and the device is connected.
  uint32_t start = micros();
  atmos_set_splash_subtitle(this->splash_subtitle_);
  atmos_show_splash(this->splash_name_);
  lv_refr_now(this->disp_);
  this->splash_ms_ = millis();
  ESP_LOGI(TAG, "Splash shown in %.1f ms", (micros() - start) / 1000.0f);
  this->set_timeout("frame", 100, [this]() { this->wait_ready_(); });
}

/* Connected means a Home Assistant API client when the API is configured, otherwise
   the network. A device with neither starts as soon as setup has finished. */
bool AtmosComponent::ready_() const {
#ifdef USE_API
  return api::global_api_server != nullptr && api::global_api_server->is_connected();
#elif defined(USE_NETWORK)
  return network::is_connected();
#else
  return true;
#endif
}

void AtmosComponent::wait_ready_() {
  bool timed_out = millis() - this->splash_ms_ >= this->splash_timeout_ms_;
  if (!this->ready_() && !timed_out) {
    this->set_timeout("frame", 100, [this]() { this->wait_ready_(); });
    return;
  }
  // Create the UI in this call and draw its first frame in the next, so neither
  // blocks the loop for long. The first frame opens from the splash.
  uint32_t start = micros();
  atmos_init();
  if (this->frame_stats_)
    atmos_set_clock([]() -> uint32_t { return micros(); });
  this->started_ = true;
  atmos_set_auto(this->auto_);
  if (this->pending_reading_ >= 0)
    atmos_select_reading(this->pending_reading_);
  this->last_report_ms_ = millis();
  ESP_LOGI(TAG, "Atmos UI started after %u ms of splash (%s) in %.1f ms; internal free %u, PSRAM free %u",
           (unsigned) (millis() - this->splash_ms_), timed_out ? "timed out" : "connected",
           (micros() - start) / 1000.0f, (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  this->set_timeout("frame", 0, [this]() {
    this->next_step_us_ = micros();
    this->frame_();
  });
}

void AtmosComponent::advance(int direction) {
  if (!this->started_)
    return;  // the UI is created on the first frame
  // Presses closer together than key_interval (bounce, repeats) are dropped.
  uint32_t now = millis();
  if (this->key_pressed_ && now - this->last_key_ms_ < this->key_interval_ms_)
    return;
  this->key_pressed_ = true;
  this->last_key_ms_ = now;
  int before = atmos_reading();
  if (this->frame_stats_)
    ESP_LOGI(TAG, "key %+d", direction);
  atmos_advance(direction);
  // During a transition the press waits for the arriving page; adopt the reading it moves to.
  this->adopt_ = true;
  this->adopt_from_ = before;
  this->publish_selection_();
}

void AtmosComponent::select_option(int option) {
  if (option <= 0) {
    if (this->auto_switch_ != nullptr)
      this->auto_switch_->turn_on();  // publishes the switch and calls set_auto()
    else
      this->set_auto(true);
  } else {
    int reading = option - 1;
    this->manual_ = reading;
    this->adopt_ = false;
    if (this->started_)
      atmos_select_reading(reading);
    else
      this->pending_reading_ = reading;
  }
  this->publish_selection_();
}

void AtmosComponent::set_auto(bool on) {
  this->auto_ = on;
  this->manual_ = -1;
  this->adopt_ = false;
  if (this->started_)
    atmos_set_auto(on);
  this->publish_selection_();
}

void AtmosComponent::publish_selection_() {
  if (this->reading_select_ == nullptr)
    return;
  int reading = this->started_ ? atmos_reading() : this->pending_reading_;  // -1 on the overview
  if (this->adopt_ && reading != this->adopt_from_) {
    this->manual_ = reading;
    this->adopt_ = false;
  }
  int option;
  if (!this->auto_) {
    option = reading >= 0 ? reading + 1 : this->published_option_;  // the overview keeps the last value
  } else {
    // The chosen reading has expired once cycling moves the display off it.
    if (this->manual_ >= 0 && reading != this->manual_ && !this->adopt_)
      this->manual_ = -1;
    option = this->manual_ >= 0 ? this->manual_ + 1 : 0;
  }
  if (option >= 0 && option != this->published_option_) {
    this->published_option_ = option;
    this->reading_select_->publish_state((size_t) option);
  }
}

// Hold, history span and bands are model settings that atmos_init() keeps, so they
// apply directly even before the UI starts.
void AtmosComponent::set_hold(float seconds) { atmos_set_hold(seconds); }
void AtmosComponent::set_history_window(float seconds) { atmos_set_history_window(seconds); }

void AtmosComponent::add_band(int reading, float min, float max, int level, const char *text) {
  static atmos_band bands[ATMOS_READINGS][ATMOS_BANDS_MAX];
  static int counts[ATMOS_READINGS];
  if (reading < 0 || reading >= ATMOS_READINGS || counts[reading] >= ATMOS_BANDS_MAX)
    return;
  atmos_band &b = bands[reading][counts[reading]++];
  b.min = min;
  b.max = max;
  b.level = level;
  snprintf(b.text, sizeof b.text, "%s", text);
  atmos_set_bands(reading, bands[reading], counts[reading]);
}

void AtmosComponent::set_sensor(int reading, sensor::Sensor *s) {
  if (reading < 0 || reading >= ATMOS_READINGS)
    return;
  if (!this->has_sensors_)
    std::fill(std::begin(this->values_), std::end(this->values_), NAN);
  this->has_sensors_ = this->values_dirty_ = true;
  s->add_on_state_callback([this, reading](float value) {
    this->values_[reading] = value;
    this->values_dirty_ = true;
  });
}

/* All linked readings in one atmos_set_readings() per frame, however many sensors reported. */
void AtmosComponent::push_readings_() {
  const float *v = this->values_;
  atmos_readings r{};
  r.co2 = v[ATMOS_CO2];
  r.voc = v[ATMOS_VOC];
  r.nox = v[ATMOS_NOX];
  r.hcho = v[ATMOS_HCHO];
  r.pm1 = v[ATMOS_PM1];
  r.pm = v[ATMOS_PM25];
  r.pm4 = v[ATMOS_PM4];
  r.pm10 = v[ATMOS_PM10];
  r.temp = v[ATMOS_TEMPERATURE];
  r.rh = v[ATMOS_HUMIDITY];
  r.pressure = v[ATMOS_PRESSURE];
  atmos_set_readings(r);
  this->values_dirty_ = false;
}

void AtmosAutoSwitch::setup() {
  optional<bool> initial = this->get_initial_state_with_restore_mode();
  this->write_state(initial.has_value() ? *initial : true);
}

void AtmosAutoSwitch::write_state(bool state) {
  this->parent_->set_auto(state);
  this->publish_state(state);
}

void AtmosNumber::setup() {
  float value = this->initial_;
  this->pref_ = this->make_entity_preference<float>();
  float restored;
  if (this->pref_.load(&restored) && std::isfinite(restored))
    value = restored;
  this->control(value);
}

void AtmosNumber::control(float value) {
  value = clamp(value, this->traits.get_min_value(), this->traits.get_max_value());
  if (this->history_)
    this->parent_->set_history_window(value * this->scale_);
  else
    this->parent_->set_hold(value * this->scale_);
  this->pref_.save(&value);
  this->publish_state(value);
}

void AtmosComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Atmos UI: own LVGL %d.%d.%d, 1-bit display, frame period %u us, frame stats %s every %u ms",
                LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, (unsigned) this->period_us_,
                ONOFF(this->frame_stats_), (unsigned) this->report_ms_);
  ESP_LOGCONFIG(TAG, "  Splash: \"%s\" / \"%s\", timeout %u ms", this->splash_name_, this->splash_subtitle_, (unsigned) this->splash_timeout_ms_);
}

/* Direct mode: px_map is the whole frame; only the rows LVGL redrew are converted.
   The panel buffer is pushed once per refresh, after the last area. */
void AtmosComponent::flush_cb_(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  auto *self = static_cast<AtmosComponent *>(lv_display_get_user_data(disp));
  uint32_t now = micros();
  self->render_us_ += now - mark_us;
  self->panel_->load_rows_i1(px_map + 8, area->y1, area->y2);
  uint32_t converted = micros();
  self->convert_us_ += converted - now;
  if (lv_display_flush_is_last(disp)) {
    self->panel_->push_buffer();
    self->push_us_ += micros() - converted;
    self->frame_flushed_ = true;
  }
  mark_us = micros();
  lv_display_flush_ready(disp);
}

void AtmosComponent::display_event_(lv_event_t *e) {
  static_cast<AtmosComponent *>(lv_event_get_user_data(e))->on_display_event_(lv_event_get_code(e));
}

void AtmosComponent::on_display_event_(lv_event_code_t code) {
  uint32_t now = micros();
  if (code == LV_EVENT_REFR_START) {
    this->frame_start_ = mark_us = now;
    this->render_us_ = this->convert_us_ = this->push_us_ = 0;
    this->frame_flushed_ = false;
    this->frame_moving_ = atmos_is_moving();
    this->frame_scene_ = atmos_scene();
    return;
  }
  // LV_EVENT_REFR_READY
  if (!this->frame_flushed_)
    return;  // refresh with nothing drawn
  uint32_t interval = this->last_present_ ? now - this->last_present_ : this->period_us_;
  this->last_present_ = now;
  bool moving = this->frame_moving_ || atmos_is_moving();
  uint32_t cost = now - this->frame_start_;
  if (this->frame_moving_) {
    this->trans_cost_[this->frame_scene_] += cost;
    this->trans_n_[this->frame_scene_]++;
  } else if (!moving) {
    this->still_cost_[this->frame_scene_] += cost;
    this->still_n_[this->frame_scene_]++;
  }
  for (FrameStats *s : {moving ? &this->transition_ : &this->still_,
                        moving ? &this->window_transition_ : &this->window_still_})
    s->add(interval, this->render_us_, this->convert_us_, this->push_us_, this->period_us_);
  if (moving)
    this->current_transition_.add(interval, this->render_us_, this->convert_us_, this->push_us_, this->period_us_);
}

/* Frames are driven by a scheduler timeout rather than loop(): the main loop task sleeps
   until the next frame is due, so Wi-Fi, the API and sensors get the CPU between frames
   and a frame never starts earlier than its period (the frame-rate cap). */
void AtmosComponent::frame_() {
  uint32_t now = micros(), step_us = 0, refr_us = 0;
  int pre = -1;
  // Fixed-cadence model steps: one step per frame period. A step that is a full period
  // late means a frame slot was missed; the model then advances by the real elapsed time.
  if ((int32_t) (now - this->next_step_us_) >= 0) {
    uint32_t late = now - this->next_step_us_;
    uint32_t steps = 1 + late / this->period_us_;
    this->missed_steps_ += steps - 1;
    if (this->has_sensors_ && this->values_dirty_)
      this->push_readings_();
#ifdef USE_WIFI
    // The badge's Wi-Fi icon; atmos redraws only when the bar count changes.
    auto *wifi = wifi::global_wifi_component;
    atmos_set_wifi(wifi != nullptr && wifi->is_connected() ? atmos_wifi_bars(wifi->wifi_rssi()) : -1);
#endif
    uint32_t step_start = micros();
    atmos_step(steps * this->period_us_ / 1e6);
    step_us = micros() - step_start;
    pre = atmos_step_prerendered();
    if (pre >= 0) {
      this->pre_cost_[pre] += micros() - step_start;
      this->pre_n_[pre]++;
    }
    this->next_step_us_ += steps * this->period_us_;
    uint32_t refr_start = micros();
    lv_refr_now(this->disp_);  // render now, not on LVGL's own refresh timer
    refr_us = micros() - refr_start;
  }
  uint32_t timer_start = micros();
  lv_timer_handler();
  uint32_t timer_us = micros() - timer_start, publish_start = micros();
  this->publish_selection_();
  uint32_t publish_us = micros() - publish_start;
  if (this->frame_stats_) {
    const char *state = atmos_debug_state();
    if (*state)
      ESP_LOGI(TAG, "state %s", state);
  }
  // Stall finder: where a slow frame's time went (model step incl. pre-render, LVGL
  // refresh = render + convert + push, LVGL timers, select publish).
  if (this->frame_stats_ && micros() - now > 150000)
    ESP_LOGW(TAG, "slow frame %.0f ms: step %.0f (pre-render %d) refresh %.0f (render %.0f convert %.0f push %.0f) timers %.0f publish %.0f | scene %d moving %d",
             (micros() - now) / 1000.0f, step_us / 1000.0f, pre, refr_us / 1000.0f, this->render_us_ / 1000.0f,
             this->convert_us_ / 1000.0f, this->push_us_ / 1000.0f, timer_us / 1000.0f, publish_us / 1000.0f,
             atmos_scene(), (int) atmos_is_moving());
  bool moving = atmos_is_moving();
  if (this->frame_stats_ && this->was_moving_ && !moving) {
    this->transition_index_++;
    char name[24];
    snprintf(name, sizeof name, "trans #%u", (unsigned) this->transition_index_);
    this->current_transition_.log(name, this->period_us_);
    this->current_transition_ = FrameStats();
  }
  this->was_moving_ = moving;
  if (this->frame_stats_ && millis() - this->last_report_ms_ >= this->report_ms_) {
    this->last_report_ms_ = millis();
    this->report_();
  }
  uint32_t done = micros();
  this->busy_us_ += done - now;
  int32_t wait = (int32_t) (this->next_step_us_ - done);
  this->set_timeout("frame", wait > 0 ? (wait + 999) / 1000 : 0, [this]() { this->frame_(); });
}

void AtmosComponent::report_() {
  uint32_t now_ms = millis();
  ESP_LOGI(TAG, "---- last %u s ----  UI busy %.0f%% of CPU (frame cap %.1f fps)", (unsigned) (this->report_ms_ / 1000),
           this->busy_us_ / 10.0f / std::max<uint32_t>(1, now_ms - this->report_start_ms_), 1e6f / this->period_us_);
  this->busy_us_ = 0;
  this->report_start_ms_ = now_ms;
  this->window_still_.log("still", this->period_us_);
  this->window_transition_.log("transition", this->period_us_);
  this->still_.log("still tot", this->period_us_);
  this->transition_.log("trans tot", this->period_us_);
  ESP_LOGI(TAG, "pre-rendered transitions %u, compose avg %.1f ms, clip overruns %u %s",
           (unsigned) atmos_prerendered_transitions(), atmos_compose_ms(), (unsigned) atmos_clip_overruns(),
           atmos_overrun_detail());
  // Lines the simulator's --hw-pace profile is copied from (ms per scene, 0 = no data).
  auto avg = [](uint64_t sum, uint32_t n) { return n ? sum / 1000.0f / n : 0.0f; };
  auto row = [&](const char *name, const uint64_t *sum, const uint32_t *n) {
    ESP_LOGI(TAG, "PROFILE %s %.1f %.1f %.1f %.1f %.1f %.1f", name, avg(sum[0], n[0]), avg(sum[1], n[1]),
               avg(sum[2], n[2]), avg(sum[3], n[3]), avg(sum[4], n[4]), avg(sum[5], n[5]));
  };
  row("still", still_cost_, still_n_);
  row("trans", trans_cost_, trans_n_);
  row("prerender", pre_cost_, pre_n_);
  ESP_LOGI(TAG, "missed model steps %u  |  heap internal free %u (min %u, largest %u)  PSRAM free %u",
           (unsigned) this->missed_steps_, (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  this->window_still_ = FrameStats();
  this->window_transition_ = FrameStats();
}

}  // namespace esphome::atmos

/* LVGL memory (LV_STDLIB_CUSTOM on the device, see lv_conf.h): PSRAM first, then
   internal RAM, as ESPHome's own LVGL component does. */
static unsigned lv_cap_bits = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
void lv_mem_init() {}
void lv_mem_deinit() {}
lv_result_t lv_mem_test_core() { return LV_RESULT_OK; }
void lv_mem_monitor_core(lv_mem_monitor_t *mon) {
  multi_heap_info_t info;
  heap_caps_get_info(&info, lv_cap_bits);
  memset(mon, 0, sizeof *mon);
  mon->total_size = info.total_allocated_bytes + info.total_free_bytes;
  mon->free_size = info.total_free_bytes;
  mon->free_biggest_size = info.largest_free_block;
}
void *lv_malloc_core(size_t size) {
  void *ptr = heap_caps_malloc(size, lv_cap_bits);
  if (ptr == nullptr) {
    lv_cap_bits = MALLOC_CAP_8BIT;
    ptr = heap_caps_malloc(size, lv_cap_bits);
  }
  if (ptr == nullptr)
    ESP_LOGE(esphome::atmos::TAG, "LVGL failed to allocate %u bytes", (unsigned) size);
  return ptr;
}
void lv_free_core(void *ptr) { heap_caps_free(ptr); }
void *lv_realloc_core(void *ptr, size_t size) { return heap_caps_realloc(ptr, size, lv_cap_bits); }
