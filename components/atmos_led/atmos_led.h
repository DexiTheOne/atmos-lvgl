#pragma once

#include "esphome/core/component.h"
#include "esphome/components/atmos/atmos_component.h"
#include "esphome/components/light/addressable_light.h"
#include "esphome/components/light/light_state.h"

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
  uint32_t target_() const;
  atmos::AtmosComponent *atmos_{nullptr};
  light::LightState *state_{nullptr};  // the strip
  light::LightState *own_{nullptr};    // this brightness-only light
  light::AddressableLight *out_{nullptr};
  uint32_t fade_ms_{500};
  float shown_[3]{};      // colour at full intensity outside transitions (0..255)
  float from_[3]{};       // colour when the current transition started
  bool was_moving_{false};
  uint32_t last_ms_{0};
  uint32_t written_{0xFFFFFFFF};
  uint8_t written_brightness_{0};
};

}  // namespace esphome::atmos_led
