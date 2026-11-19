#include "atmos_led.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome::atmos_led {

static const char *const TAG = "atmos_led";

void AtmosLed::setup() {
  this->out_ = static_cast<light::AddressableLight *>(this->state_->get_output());
  this->last_ms_ = millis();
  // Claim the strip's colour before switching it on: a light's default colour is
  // white, so turning it on first would flash white until the first loop().
  this->out_->set_effect_active(true);
  this->out_->all() = Color::BLACK;
  this->out_->schedule_show();
  // The strip only carries colours: keep it on at full brightness.
  this->state_->make_call().set_state(true).set_brightness(1.0f).set_transition_length(0).perform();
  // Gamma is applied here in float, then rounded once. The strip's own table maps
  // already-rounded 8-bit values, so at low levels many inputs land on the same
  // output (gamma 2.8: inputs below about 27 give 0 or 1) and dimming moves in
  // visible steps. Take the strip's gamma and switch its table off.
  this->gamma_ = this->state_->get_gamma_correct();
#ifdef USE_LIGHT_GAMMA_LUT
  this->state_->set_gamma_table(nullptr);
  this->out_->setup_state(this->state_);
#endif
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
  float span = this->dark_value_ - this->bright_value_;
  float f = span != 0.0f ? (this->dark_value_ - this->smooth_) / span : 1.0f;  // 1 bright, 0 dark
  f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
  // Low light for the UI (overview in low light, muted alert sounds), whatever the
  // auto-dimming switch: dark within 10 % of dark_value, lit again from 30 %.
  bool dark = this->low_light_ ? f < 0.3f : f <= 0.1f;
  if (dark != this->low_light_) {
    this->low_light_ = dark;
    atmos_set_low_light(dark);
  }
  if (!this->auto_dim_ || level <= this->min_brightness_)
    return level;
  return this->min_brightness_ + (level - this->min_brightness_) * f;
}

void AtmosLed::dump_config() {
  ESP_LOGCONFIG(TAG, "Atmos status LEDs: %d LEDs, fade %u ms", this->out_ ? this->out_->size() : 0,
                (unsigned) this->fade_ms_);
  if (this->ambient_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Auto dimming: full at %.3f, %.0f%% at %.3f, smoothing %u ms", this->bright_value_,
                  this->min_brightness_ * 100.0f, this->dark_value_, (unsigned) this->smoothing_ms_);
  }
}

/* One channel (0..255, linear) to the strip's 8-bit value, with gamma applied in float. */
uint8_t AtmosLed::encode_(float value) const {
  float c = value / 255.0f;
  c = c <= 0.0f ? 0.0f : c >= 1.0f ? 1.0f : (this->gamma_ > 0.0f ? powf(c, this->gamma_) : c);
  uint32_t b = uint32_t(c * 255.0f + 0.5f);
  return b == 0 && c > 0.0f ? 1 : b;  // as the strip's table: lit stays lit
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
  auto v = this->state_->current_values;
  uint8_t brightness = light::to_uint8_scale(v.get_brightness() * v.get_state());

  if (!this->atmos_->started() && this->boot_color_ != 0) {
    // Boot animation, while the splash waits for the connection: the boot colour
    // breathes along the strip, each LED a quarter cycle behind the one before, so a
    // soft wave runs from the first LED to the last. When the UI starts, its intro
    // transition fades from the boot colour (shown_) into the first page's.
    float goal_boot[3] = {float((this->boot_color_ >> 16) & 0xFF), float((this->boot_color_ >> 8) & 0xFF),
                          float(this->boot_color_ & 0xFF)};
    for (int i = 0; i < 3; i++)
      this->shown_[i] = goal_boot[i] * 0.6f;
    float phase = float(fmod(now / 1000.0, double(BOOT_PERIOD_S))) / BOOT_PERIOD_S;
    // Rise from dark over the first second after power-up.
    float rise = now < 1000 ? now / 1000.0f : 1.0f;
    int n = this->out_->size();
    for (int led = 0; led < n; led++) {
      float w = 0.5f - 0.5f * cosf(6.2831853f * (phase - 0.25f * float(led)));
      float k = (0.15f + 0.85f * w) * rise * level;
      uint8_t c[3];
      for (int i = 0; i < 3; i++)
        c[i] = this->encode_(goal_boot[i] * k);
      (*this->out_)[led] = Color(c[0], c[1], c[2]);
    }
    this->written_ = 0xFFFFFFFF;  // force a write once the UI takes over
    this->out_->update_state(this->state_);
    this->out_->schedule_show();
    return;
  }

  uint32_t packed = 0;
  for (int i = 0; i < 3; i++)
    packed = (packed << 8) | this->encode_(rgb[i] * level);
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
