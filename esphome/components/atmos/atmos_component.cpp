#include "atmos_component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
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
  for (auto code : {LV_EVENT_REFR_START, LV_EVENT_REFR_READY})
    lv_display_add_event_cb(this->disp_, display_event_, code, this);
}

void AtmosComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Atmos UI: own LVGL %d.%d.%d, 1-bit display, frame period %u us, report every %u ms",
                LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, (unsigned) this->period_us_,
                (unsigned) this->report_ms_);
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

void AtmosComponent::loop() {
  uint32_t now = micros();
  if (!this->started_) {
    atmos_init();
    atmos_set_clock([]() -> uint32_t { return micros(); });
    this->high_freq_.start();
    this->started_ = true;
    this->next_step_us_ = now;
    this->last_report_ms_ = millis();
    ESP_LOGI(TAG, "Atmos UI started; internal free %u, PSRAM free %u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  // Fixed-cadence model steps: one step per frame period. A step that is a full period
  // late means a 40 fps slot was missed; the model then advances by the real elapsed time.
  if ((int32_t) (now - this->next_step_us_) >= 0) {
    uint32_t late = now - this->next_step_us_;
    uint32_t steps = 1 + late / this->period_us_;
    this->missed_steps_ += steps - 1;
    uint32_t step_start = micros();
    atmos_step(steps * this->period_us_ / 1e6);
    int pre = atmos_step_prerendered();
    if (pre >= 0) {
      this->pre_cost_[pre] += micros() - step_start;
      this->pre_n_[pre]++;
    }
    this->next_step_us_ += steps * this->period_us_;
  }
  lv_timer_handler();
  bool moving = atmos_is_moving();
  if (this->was_moving_ && !moving) {
    this->transition_index_++;
    char name[24];
    snprintf(name, sizeof name, "trans #%u", (unsigned) this->transition_index_);
    this->current_transition_.log(name, this->period_us_);
    this->current_transition_ = FrameStats();
  }
  this->was_moving_ = moving;
  if (millis() - this->last_report_ms_ >= this->report_ms_) {
    this->last_report_ms_ = millis();
    this->report_();
  }
}

void AtmosComponent::report_() {
  ESP_LOGI(TAG, "---- last %u s ----", (unsigned) (this->report_ms_ / 1000));
  this->window_still_.log("still", this->period_us_);
  this->window_transition_.log("transition", this->period_us_);
  this->still_.log("still tot", this->period_us_);
  this->transition_.log("trans tot", this->period_us_);
  ESP_LOGI(TAG, "pre-rendered transitions %u, compose avg %.1f ms, clip overruns %u %s",
           (unsigned) atmos_prerendered_transitions(), atmos_compose_ms(), (unsigned) atmos_clip_overruns(),
           atmos_overrun_detail());
  // One line the simulator's --hw-pace profile is copied from (milliseconds, 0 = no data).
  auto avg = [](uint64_t sum, uint32_t n) { return n ? sum / 1000.0f / n : 0.0f; };
  ESP_LOGI(TAG, "PROFILE still %.1f %.1f %.1f %.1f %.1f | trans %.1f %.1f %.1f %.1f %.1f | prerender %.1f %.1f %.1f %.1f %.1f",
           avg(still_cost_[0], still_n_[0]), avg(still_cost_[1], still_n_[1]), avg(still_cost_[2], still_n_[2]),
           avg(still_cost_[3], still_n_[3]), avg(still_cost_[4], still_n_[4]), avg(trans_cost_[0], trans_n_[0]),
           avg(trans_cost_[1], trans_n_[1]), avg(trans_cost_[2], trans_n_[2]), avg(trans_cost_[3], trans_n_[3]),
           avg(trans_cost_[4], trans_n_[4]), avg(pre_cost_[0], pre_n_[0]), avg(pre_cost_[1], pre_n_[1]),
           avg(pre_cost_[2], pre_n_[2]), avg(pre_cost_[3], pre_n_[3]), avg(pre_cost_[4], pre_n_[4]));
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
