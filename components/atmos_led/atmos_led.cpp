#include "atmos_led.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome::atmos_led {

static const char *const TAG = "atmos_led";

void AtmosLed::setup() {
  this->out_ = static_cast<light::AddressableLight *>(this->state_->get_output());
  this->last_ms_ = millis();
  // The strip only carries colours: keep it on at full brightness.
  this->state_->make_call().set_state(true).set_brightness(1.0f).set_transition_length(0).perform();
  if (this->ambient_ != nullptr)
    this->ambient_->add_on_state_callback([this](float x) { this->raw_ = x; });
}

/* Limit the user's level by ambient light: level at bright_value (and brighter), falling
   linearly to min_brightness at dark_value. Never raises a level already below the minimum. */
float AtmosLed::limit_(float level, float dt) {
  if (this->ambient_ == nullptr || !std::isfinite(this->raw_))
    return level;
  if (!std::isfinite(this->smooth_))
    this->smooth_ = this->raw_;
  else if (this->smoothing_ms_ > 0) {
    float k = dt * 1000.0f / this->smoothing_ms_;
    this->smooth_ += (this->raw_ - this->smooth_) * (k > 1.0f ? 1.0f : k);
  } else
    this->smooth_ = this->raw_;
  if (!this->auto_dim_ || level <= this->min_brightness_)
    return level;
  float span = this->dark_value_ - this->bright_value_;
  float f = span != 0.0f ? (this->dark_value_ - this->smooth_) / span : 1.0f;  // 1 bright, 0 dark
  f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
  return this->min_brightness_ + (level - this->min_brightness_) * f;
}

void AtmosLed::dump_config() {
  ESP_LOGCONFIG(TAG, "Atmos status LEDs: %d LEDs, fade %u ms", this->out_ ? this->out_->size() : 0,
                (unsigned) this->fade_ms_);
  if (this->ambient_ != nullptr)
    ESP_LOGCONFIG(TAG, "  Auto dimming: full at %.3f, %.0f%% at %.3f, smoothing %u ms", this->bright_value_,
                  this->min_brightness_ * 100.0f, this->dark_value_, (unsigned) this->smoothing_ms_);
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
  // Power and brightness of this light (with its transitions) scale the colour.
  float level = 0.0f;
  if (this->own_ != nullptr) {
    auto own = this->own_->current_values;
    level = own.get_state() * own.get_brightness();
  }
  level = this->limit_(level, dt);
  for (int i = 0; i < 3; i++)
    rgb[i] *= level;
  auto v = this->state_->current_values;
  uint8_t brightness = light::to_uint8_scale(v.get_brightness() * v.get_state());
  uint32_t packed = (uint32_t(rgb[0] + 0.5f) << 16) | (uint32_t(rgb[1] + 0.5f) << 8) | uint32_t(rgb[2] + 0.5f);
  if (packed == this->written_ && brightness == this->written_brightness_)
    return;
  this->written_ = packed;
  this->written_brightness_ = brightness;
  // With the effect flag set, update_state() only sets the correction's brightness
  // from the strip light's (full) brightness.
  this->out_->update_state(this->state_);
  this->out_->all() = Color(packed >> 16, (packed >> 8) & 0xFF, packed & 0xFF);
  this->out_->schedule_show();
}

}  // namespace esphome::atmos_led
