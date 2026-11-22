/**
 * @file st7305_rlcd.h
 * @brief ESPHome driver for ST7305 reflective LCD displays
 *
 * Supports multiple panel configurations:
 * - Waveshare ESP32-S3-RLCD-4.2 (400×300) - also: GooDisplay GDTL042T71
 * - Osptek YDP154H008 (200×200)
 * - Custom user-defined panels
 *
 * References:
 * - ST7305 Datasheet: https://files.waveshare.com/wiki/common/ST_7305_V0_2.pdf
 * - Waveshare product: https://www.waveshare.com/wiki/ESP32-S3-RLCD-4.2
 *
 * @version 2.0.0
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/spi/spi.h"
#include "esphome/components/display/display_buffer.h"
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace st7305_rlcd {

/// Panel model enumeration
enum ST7305Model : uint8_t {
  ST7305_MODEL_WAVESHARE_400X300 = 0,  ///< Landscape 2×4 blocks
  ST7305_MODEL_OSPTEK_200X200,         ///< Square 4×2 blocks
  ST7305_MODEL_CUSTOM,                 ///< User-defined
};

/// Pixel block orientation (determines LUT calculation)
enum ST7305Orientation : uint8_t {
  ST7305_ORIENTATION_LANDSCAPE = 0,  ///< 2 cols × 4 rows per byte
  ST7305_ORIENTATION_PORTRAIT,       ///< 4 cols × 2 rows per byte
};

class ST7305RLCD : public display::DisplayBuffer,
                   public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                         spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_10MHZ> {
 public:
  void setup() override;
  // Ahead of sensors and other DATA components, so the splash can follow at once.
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  void update() override;
  void dump_config() override;
  void fill(Color color) override;

  // Configuration setters (called from Python codegen)
  void set_dc_pin(GPIOPin *pin) { this->dc_pin_ = pin; }
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  /// Tearing-effect output (TE): rises at the start of each vertical blank.
  void set_te_pin(InternalGPIOPin *pin) { this->te_pin_ = pin; }
  bool has_te() const { return this->te_pin_ != nullptr; }
  /// TE rising edges since setup, and the micros() of the latest.
  uint32_t te_count() const { return this->te_count_; }
  uint32_t te_time_us() const { return this->te_time_us_; }
  /// Measured refresh period (TE pulse interval, averaged), 0 until known.
  uint32_t te_period_us() const { return this->te_period_q4_ / 16; }
  /// Push only the changed window (default); false sends the whole frame every time.
  void set_partial_updates(bool on) { this->partial_ = on; }
  void set_model(ST7305Model model) { this->model_ = model; }
  void set_width(uint16_t width) { this->width_ = width; }
  void set_height(uint16_t height) { this->height_ = height; }
  void set_orientation(ST7305Orientation orientation) { this->orientation_ = orientation; }
  /// High-power refresh in half-hertz: 32 (16 Hz), 51 (25.5), 64 (32) or 102 (51).
  void set_refresh_hz_x2(uint8_t hz_x2) { this->refresh_hz_x2_ = hz_x2; }
  /* High-power source voltages as register codes: VSHP C1h (3.7 V + 0.02 V * code),
     VSHN C4h (-2.5 V - 0.02 V * code). */
  void set_source_voltages(uint8_t vshp, uint8_t vshn) {
    this->vshp_ = vshp;
    this->vshn_ = vshn;
  }

  /// Enter sleep mode (lowest power, display blanks, RAM retained)
  void sleep();
  /// Exit sleep mode (120ms delay required by controller)
  void wake();
  /// Switch to low power refresh (~1Hz) for static content
  void low_power_mode();
  /// Switch to high power refresh (~51Hz) for animations
  void high_power_mode();
  /* Panel test: send a raw command with data. Refused (false) while a push is queued
     or running; call it with frames stopped (atmos freeze). */
  bool command(uint8_t cmd, const uint8_t *data, size_t len);
  /// Turn display on (recover from display_off)
  void display_on();
  /// Turn display off (RAM retained, instant recovery)
  void display_off();

  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_BINARY; }

  /// Native 1-bit path (Atmos): convert rows y1..y2, columns x1..x2 (rounded out to
  /// whole bytes) of a row-major 1-bit frame (50-byte MSB-first rows for 400 px, bit
  /// set = white) into the panel layout, growing the window the next push sends.
  /// Returns false if the model/orientation is not the 400x300 landscape panel.
  bool load_rows_i1(const uint8_t *frame, int y1, int y2, int x1 = 0, int x2 = 399);
  /// Queue the changed window for sending: by DMA at the first TE pulse at or after
  /// target_te (te_count() numbering), or at once without a TE pin. Returns false
  /// while the previous push is still waiting or sending (try again later); the
  /// panel buffer may be written again as soon as this returns true.
  bool present(uint32_t target_te);
  /// True while a presented frame waits for its TE pulse or is being sent.
  bool busy() const { return this->busy_; }
  /// True while converted rows wait for present().
  bool has_unsent() const { return this->dirty_; }
  /// The latest completed push: TE pulse it went out at and the one it was aimed at,
  /// micros() of that pulse and of the push's start, its SPI time and bytes.
  struct PushRecord {
    uint32_t te, target, edge_us, start_us, us, bytes;
  };
  PushRecord last_push() const { return this->done_; }
  /// Completed pushes since setup.
  uint32_t pushes() const { return this->pushes_; }
  /// Send the whole panel buffer now and wait for it (splash, tests).
  void push_now();
  void push_buffer() { this->push_now(); }

  /// Bulk RGB565 path used by LVGL's flush (Atmos modification). Converts each pixel by
  /// luminance (above half = white) and writes the panel's 2x4 block layout directly.
  /// Other formats, rotations or orientations fall back to the per-pixel path.
  void draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, display::ColorOrder order,
                      display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset,
                      int x_pad) override;

 protected:
  void draw_absolute_pixel_internal(int x, int y, Color color) override;
  int get_width_internal() override { return this->width_; }
  int get_height_internal() override { return this->height_; }
  size_t get_buffer_length_() { return this->buffer_size_; }

  void apply_model_settings_();
  void hardware_reset_();
  void init_display_();
  void init_pixel_lut_();
  void init_lut_landscape_();
  void init_lut_portrait_();
  bool verify_landscape_layout_();
  bool verify_i1_path_();
  void write_display_();
  void write_ram_();
  void send_command_(uint8_t cmd);
  void send_data_(uint8_t data);

  struct Window {
    uint8_t r1, r2;  // panel rows (pairs of screen columns), inclusive
    uint8_t u1, u2;  // column units (3 bytes = 12 screen rows), inclusive
  };
  bool setup_spi_();
  void start_push_task_();
  static void push_task_(void *arg);
  void transmit_(const uint8_t *data, size_t len, bool dc);
  void send_window_(const Window &w);
  void pack_window_(const Window &w);
  void mark_dirty_(int y1, int y2, int x1, int x2);

  spi_device_handle_t spi_{nullptr};
  uint8_t *tx_buf_{nullptr};
  TaskHandle_t push_task_handle_{nullptr};
  Window win_{}, tx_win_{};
  bool dirty_{false};
  bool partial_{true};
  volatile bool busy_{false}, armed_{false};
  volatile uint32_t target_te_{0}, push_te_{0}, push_target_{0}, push_edge_us_{0};
  volatile uint32_t push_bytes_{0}, pushes_{0};
  volatile int32_t te_period_q4_{0};
  PushRecord done_{};

  GPIOPin *dc_pin_{nullptr};
  GPIOPin *reset_pin_{nullptr};
  InternalGPIOPin *te_pin_{nullptr};
  volatile uint32_t te_count_{0};
  volatile uint32_t te_time_us_{0};
  static void te_isr_(ST7305RLCD *self);

  ST7305Model model_{ST7305_MODEL_WAVESHARE_400X300};
  ST7305Orientation orientation_{ST7305_ORIENTATION_LANDSCAPE};
  uint16_t width_{400};
  uint16_t height_{300};
  uint8_t refresh_hz_x2_{64};
  uint8_t vshp_{0x69};  // +5.80 V
  uint8_t vshn_{0x19};  // -3.00 V
  size_t buffer_size_{15000};

  // Address window parameters (panel-specific)
  uint8_t col_start_{0x12};
  uint8_t col_end_{0x2A};
  uint8_t row_start_{0x00};
  uint8_t row_end_{0xC7};

  // Pixel coordinate lookup tables for O(1) buffer access
  uint16_t *pixel_index_lut_{nullptr};
  uint8_t *pixel_bit_lut_{nullptr};
};

}  // namespace st7305_rlcd
}  // namespace esphome
