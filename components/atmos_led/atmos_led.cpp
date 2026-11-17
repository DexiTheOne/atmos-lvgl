#include "atmos_led.h"
#include "esphome/core/log.h"

namespace esphome::atmos_led {

static const char *const TAG = "atmos_led";

void AtmosLed::setup() {
  this->out_ = static_cast<light::AddressableLight *>(this->state_->get_output());
  this->last_ms_ = millis();
}

void AtmosLed::dump_config() {
  ESP_LOGCONFIG(TAG, "Atmos status LEDs: %d LEDs, fade %u ms", this->out_ ? this->out_->size() : 0,
                (unsigned) this->fade_ms_);
}

/* Colour of the rating on screen (the destination's while moving); off before the UI starts. */
uint32_t AtmosLed::target_() const {
  if (!this->atmos_->started())
    return 0;
  int scene = atmos_is_moving() ? atmos_destination() : atmos_scene();
  return this->atmos_->led_color(atmos_led_reading(scene));
}

void AtmosLed::loop() {
  uint32_t now = millis();
  float dt = (now - this->last_ms_) / 1000.0f;
  this->last_ms_ = now;
  uint32_t target = this->target_();
  float goal[3] = {float((target >> 16) & 0xFF), float((target >> 8) & 0xFF), float(target & 0xFF)};
  float rgb[3];
  bool moving = this->atmos_->started() && atmos_is_moving();
  if (moving) {
    // Fade out what was shown over the first half of the transition, in over the second.
    if (!this->was_moving_)
      for (int i = 0; i < 3; i++)
        this->from_[i] = this->shown_[i];
    float p = atmos_transition_progress();
    for (int i = 0; i < 3; i++)
      rgb[i] = p < 0.5f ? this->from_[i] * (1.0f - 2.0f * p) : goal[i] * (2.0f * p - 1.0f);
    for (int i = 0; i < 3; i++)
      this->shown_[i] = goal[i];
  } else {
    // A rating change on the same page cross-fades over fade_time.
    float step = this->fade_ms_ ? 255.0f * dt * 1000.0f / this->fade_ms_ : 255.0f;
    for (int i = 0; i < 3; i++) {
      float d = goal[i] - this->shown_[i];
      this->shown_[i] += d > step ? step : d < -step ? -step : d;
      rgb[i] = this->shown_[i];
    }
  }
  this->was_moving_ = moving;

  // Keep the light from writing its own colour (selecting an effect clears this).
  this->out_->set_effect_active(true);
  auto v = this->state_->current_values;
  uint8_t brightness = light::to_uint8_scale(v.get_brightness() * v.get_state());
  uint32_t packed = (uint32_t(rgb[0] + 0.5f) << 16) | (uint32_t(rgb[1] + 0.5f) << 8) | uint32_t(rgb[2] + 0.5f);
  if (packed == this->written_ && brightness == this->written_brightness_)
    return;
  this->written_ = packed;
  this->written_brightness_ = brightness;
  // With the effect flag set, update_state() only sets the correction's brightness
  // from the light's power and brightness.
  this->out_->update_state(this->state_);
  this->out_->all() = Color(packed >> 16, (packed >> 8) & 0xFF, packed & 0xFF);
  this->out_->schedule_show();
}

}  // namespace esphome::atmos_led
