#pragma once

#include "esphome/core/component.h"
#include "esphome/components/atmos/atmos_component.h"
#include "esphome/components/light/addressable_light.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"

namespace esphome::atmos_led {

/* Drives an addressable strip with the colour of the rating on screen. It marks the
   strip's output as running an effect, so the strip light's own colour is not written,
   and holds that light on at full brightness. AtmosLed is itself a brightness-only
   light: Home Assistant's power and brightness scale the colours. */
class AtmosLed : public Component, public light::LightOutput {
 public:
  void set_atmos(atmos::AtmosComponent *atmos) { this->atmos_ = atmos; }
  void set_light(light::LightState *state) { this->state_ = state; }
  void set_fade_ms(uint32_t ms) { this->fade_ms_ = ms; }
  void set_boot_color(uint32_t rgb) { this->boot_color_ = rgb; }
  void set_reverse(bool reverse) { this->reverse_ = reverse; }
  void set_ambient_sensor(sensor::Sensor *s) { this->ambient_ = s; }
  void set_ambient_range(float bright, float dark, float min_brightness) {
    this->bright_value_ = bright;
    this->dark_value_ = dark;
    this->min_brightness_ = min_brightness;
  }
  void set_smoothing_ms(uint32_t ms) { this->smoothing_ms_ = ms; }
  void set_auto_dim(bool on) { this->auto_dim_ = on; }
  void setup() override;
  void loop() override;
  void dump_config() override;
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return traits;
  }
  void setup_state(light::LightState *state) override { this->own_ = state; }
  void write_state(light::LightState *state) override {}  // read in loop()
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 1; }

 protected:
  static constexpr float BOOT_PERIOD_S = 3.2f;     // one round trip of the bright glow
  static constexpr float BOOT_GLOW_WIDTH = 0.8f;   // bright glow's spread, in LEDs
  static constexpr float BOOT_BACKGROUND = 0.35f;  // soft background, as a share of the light's level
  static constexpr uint32_t HANDOFF_MS = 1500;     // glow settles, then fades into the page
  static constexpr int MAX_LEDS = 16;
  uint32_t target_() const;
  uint8_t encode_(float value) const;
  float limit_(float level, float dt);
  sensor::Sensor *ambient_{nullptr};
  float bright_value_{0}, dark_value_{1}, min_brightness_{0.13f};
  uint32_t smoothing_ms_{5000};
  float gamma_{2.8f};    // the strip light's gamma_correct, applied in float
  bool auto_dim_{true};
  float raw_{NAN};       // latest ambient reading
  float smooth_{NAN};    // smoothed ambient reading
  bool low_light_{false};  // reported to the UI with hysteresis
  atmos::AtmosComponent *atmos_{nullptr};
  light::LightState *state_{nullptr};  // the strip
  light::LightState *own_{nullptr};    // this brightness-only light
  light::AddressableLight *out_{nullptr};
  uint32_t fade_ms_{800};
  float fade_from_[3]{};            // colour when the still-page cross-fade started
  uint32_t fade_goal_{0xFFFFFFFF};  // its goal (packed RGB)
  uint32_t fade_start_ms_{0};
  uint32_t boot_color_{0x18BCF2};
  float boot_k_[MAX_LEDS]{};  // each LED's bright glow (0..1) when last drawn
  bool boot_shown_{false};
  bool reverse_{false};        // the strip's first LED is on the right
  uint32_t boot_start_ms_{0};
  bool handed_off_{false};
  uint32_t handoff_ms_{0};    // nonzero while the boot glow hands over to the first page  // 0: dark until the UI starts
  float shown_[3]{};      // colour at full intensity outside transitions (0..255)
  float from_[3]{};       // colour when the current transition started
  bool was_moving_{false};
  uint32_t last_ms_{0};
  uint32_t written_{0xFFFFFFFF};
  uint8_t written_brightness_{0};
};

/* Home Assistant switch for auto_dimming. */
class AutoDimSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(AtmosLed *parent) { this->parent_ = parent; }
  void setup() override {
    bool on = this->get_initial_state_with_restore_mode().value_or(true);
    this->parent_->set_auto_dim(on);
    this->publish_state(on);
  }

 protected:
  void write_state(bool state) override {
    this->parent_->set_auto_dim(state);
    this->publish_state(state);
  }
  AtmosLed *parent_{nullptr};
};

}  // namespace esphome::atmos_led
