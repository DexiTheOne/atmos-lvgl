/**
 * @file st7305_rlcd.cpp
 * @brief ESPHome driver for ST7305 reflective LCD displays
 *
 * References:
 * - ST7305 Datasheet: https://files.waveshare.com/wiki/common/ST_7305_V0_2.pdf
 * - Waveshare Arduino driver (display_bsp.cpp)
 * - Waveshare XiaoZhi driver (custom_lcd_display.cc)
 *
 * @version 2.0.0
 *
 * Modified for the Atmos project (2026-10-18): native 1-bit load_rows_i1()/push_buffer(),
 * bulk RGB565 draw_pixels_at() for LVGL,
 * display buffer in internal RAM, and a start-up check of the bulk layout against the
 * pixel LUTs. Original: https://github.com/kylehase/ESPHome-ST7305-RLCD (Apache-2.0).
 */

#include "st7305_rlcd.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <vector>

namespace esphome {
namespace st7305_rlcd {

static const char *const TAG = "st7305_rlcd";

// =============================================================================
// Setup and Configuration
// =============================================================================

void ST7305RLCD::setup() {
  ESP_LOGCONFIG(TAG, "Setting up ST7305 RLCD...");

  // Apply model-specific settings
  this->apply_model_settings_();

  // Configure pins
  this->dc_pin_->setup();
  this->dc_pin_->digital_write(true);

  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
  }

  // Initialize SPI
  this->spi_setup();

  // Allocate display buffer in internal RAM: the bulk flush writes it randomly every frame.
  RAMAllocator<uint8_t> buffer_allocator(RAMAllocator<uint8_t>::ALLOC_INTERNAL);
  this->buffer_ = buffer_allocator.allocate(this->buffer_size_);
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate display buffer (%zu bytes)", this->buffer_size_);
    this->mark_failed();
    return;
  }
  memset(this->buffer_, 0xFF, this->buffer_size_);

  // Initialize pixel lookup tables
  this->init_pixel_lut_();
  if (this->pixel_index_lut_ == nullptr || this->pixel_bit_lut_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate pixel LUTs");
    this->mark_failed();
    return;
  }
  if (this->orientation_ == ST7305_ORIENTATION_LANDSCAPE && !this->verify_landscape_layout_())
    ESP_LOGE(TAG, "Bulk layout disagrees with the pixel LUTs; the bulk flush will be wrong");
  if (this->orientation_ == ST7305_ORIENTATION_LANDSCAPE && !this->verify_i1_path_())
    ESP_LOGE(TAG, "1-bit row conversion disagrees with the pixel LUTs");

  // Hardware initialization
  this->hardware_reset_();
  this->init_display_();

  ESP_LOGCONFIG(TAG, "ST7305 RLCD setup complete");
}

void ST7305RLCD::apply_model_settings_() {
  switch (this->model_) {
    case ST7305_MODEL_WAVESHARE_400X300:
      this->width_ = 400;
      this->height_ = 300;
      this->orientation_ = ST7305_ORIENTATION_LANDSCAPE;
      this->buffer_size_ = (400 * 300) / 8;  // 15000 bytes
      this->col_start_ = 0x12;
      this->col_end_ = 0x2A;
      this->row_start_ = 0x00;
      this->row_end_ = 0xC7;
      break;

    case ST7305_MODEL_OSPTEK_200X200:
      this->width_ = 200;
      this->height_ = 200;
      this->orientation_ = ST7305_ORIENTATION_PORTRAIT;
      this->buffer_size_ = (200 * 200) / 8;  // 5000 bytes
      // Address window for 200×200 - estimated based on panel size
      this->col_start_ = 0x13;
      this->col_end_ = 0x25;
      this->row_start_ = 0x00;
      this->row_end_ = 0x63;
      break;

    case ST7305_MODEL_CUSTOM:
      // User has set width_, height_, orientation_ directly
      this->buffer_size_ = (this->width_ * this->height_) / 8;
      // User should also set address window via separate methods if needed
      break;
  }

  ESP_LOGD(TAG, "Model settings: %dx%d, %s, buffer=%zu bytes",
           this->width_, this->height_,
           this->orientation_ == ST7305_ORIENTATION_LANDSCAPE ? "landscape" : "portrait",
           this->buffer_size_);
}

void ST7305RLCD::dump_config() {
  LOG_DISPLAY("", "ST7305 RLCD", this);

  const char *model_name;
  switch (this->model_) {
    case ST7305_MODEL_WAVESHARE_400X300:
      model_name = "Waveshare 400x300";
      break;
    case ST7305_MODEL_OSPTEK_200X200:
      model_name = "Osptek 200x200";
      break;
    case ST7305_MODEL_CUSTOM:
      model_name = "Custom";
      break;
    default:
      model_name = "Unknown";
  }

  ESP_LOGCONFIG(TAG, "  Model: %s", model_name);
  ESP_LOGCONFIG(TAG, "  Resolution: %dx%d", this->width_, this->height_);
  ESP_LOGCONFIG(TAG, "  Orientation: %s",
                this->orientation_ == ST7305_ORIENTATION_LANDSCAPE ? "Landscape (2x4)" : "Portrait (4x2)");
  ESP_LOGCONFIG(TAG, "  Buffer Size: %zu bytes", this->buffer_size_);
  ESP_LOGCONFIG(TAG, "  Rotated Size: %dx%d", this->get_width(), this->get_height());
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
}

// =============================================================================
// Display Operations
// =============================================================================

void ST7305RLCD::update() {
  this->do_update_();
  this->write_display_();
}

void ST7305RLCD::fill(Color color) {
  const uint8_t fill_value = (color.is_on()) ? 0x00 : 0xFF;
  memset(this->buffer_, fill_value, this->buffer_size_);
}

// =============================================================================
// Bulk RGB565 flush (Atmos)
// =============================================================================

// Landscape layout, as in init_lut_landscape_(): byte = (x/2) * (H/4) + (H-1-y)/4,
// bit = 7 - (((H-1-y) % 4) * 2 + x % 2). Bit set = white.
bool ST7305RLCD::verify_landscape_layout_() {
  const uint16_t h4 = this->height_ >> 2;
  for (uint16_t y = 0; y < this->height_; y++) {
    const uint16_t inv_y = this->height_ - 1 - y;
    for (uint16_t x = 0; x < this->width_; x++) {
      const uint32_t pixel_idx = static_cast<uint32_t>(x) * this->height_ + y;
      const uint32_t index = (x >> 1) * h4 + (inv_y >> 2);
      const uint8_t mask = 0x80 >> (((inv_y & 3) << 1) | (x & 1));
      if (this->pixel_index_lut_[pixel_idx] != index || this->pixel_bit_lut_[pixel_idx] != mask)
        return false;
    }
  }
  ESP_LOGCONFIG(TAG, "Bulk flush layout matches the pixel LUTs for all %u pixels",
                (unsigned) this->width_ * this->height_);
  return true;
}

// Each panel byte holds a 2-column x 4-row block: bits 7..0 = (row r, col c) for
// r = 0..3, c = 0..1, where row r is screen row H-1-(4*block+r). A source byte of a
// row holds 8 columns, i.e. the 2-bit pairs for 4 consecutive panel bytes.
bool HOT ST7305RLCD::load_rows_i1(const uint8_t *frame, int y1, int y2) {
  if (this->buffer_ == nullptr || this->orientation_ != ST7305_ORIENTATION_LANDSCAPE || this->width_ != 400 ||
      this->height_ % 4 != 0)
    return false;
  const int h = this->height_, h4 = h >> 2, row_bytes = this->width_ / 8;
  y1 = std::max(y1, 0);
  y2 = std::min(y2, h - 1);
  // Blocks covering the rows (screen rows run bottom-up through the blocks).
  const int b_first = (h - 1 - y2) >> 2, b_last = (h - 1 - y1) >> 2;
  for (int b = b_first; b <= b_last; b++) {
    const uint8_t *r0 = frame + (h - 1 - 4 * b) * row_bytes;
    const uint8_t *r1 = r0 - row_bytes, *r2 = r1 - row_bytes, *r3 = r2 - row_bytes;
    uint8_t *dest = this->buffer_ + b;
    for (int sx = 0; sx < row_bytes; sx++) {
      const uint8_t a = r0[sx], c1 = r1[sx], c2 = r2[sx], c3 = r3[sx];
      for (int p = 0; p < 4; p++) {
        const int shift = 6 - 2 * p;
        dest[(sx * 4 + p) * h4] = (uint8_t) ((((a >> shift) & 3) << 6) | (((c1 >> shift) & 3) << 4) |
                                             (((c2 >> shift) & 3) << 2) | ((c3 >> shift) & 3));
      }
    }
  }
  return true;
}

// Start-up check: a pseudo-random frame converted by load_rows_i1() must match the
// per-pixel LUT placement. Restores the buffer afterwards.
bool ST7305RLCD::verify_i1_path_() {
  const int row_bytes = this->width_ / 8;
  std::vector<uint8_t> frame(row_bytes * this->height_);
  uint32_t seed = 0x12345678;
  for (auto &v : frame) {
    seed = seed * 1664525u + 1013904223u;
    v = seed >> 24;
  }
  if (!this->load_rows_i1(frame.data(), 0, this->height_ - 1))
    return false;
  bool ok = true;
  for (int y = 0; y < this->height_ && ok; y++) {
    for (int x = 0; x < this->width_; x++) {
      const bool white = (frame[y * row_bytes + x / 8] >> (7 - (x & 7))) & 1;
      const uint32_t idx = static_cast<uint32_t>(x) * this->height_ + y;
      const bool set = this->buffer_[this->pixel_index_lut_[idx]] & this->pixel_bit_lut_[idx];
      if (white != set) {
        ok = false;
        break;
      }
    }
  }
  memset(this->buffer_, 0xFF, this->buffer_size_);
  if (ok)
    ESP_LOGI(TAG, "1-bit row conversion matches the pixel LUTs for all pixels");
  return ok;
}

void HOT ST7305RLCD::draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr,
                                    display::ColorOrder order, display::ColorBitness bitness, bool big_endian,
                                    int x_offset, int y_offset, int x_pad) {
  if (bitness != display::COLOR_BITNESS_565 || this->get_rotation() != display::DISPLAY_ROTATION_0_DEGREES ||
      this->orientation_ != ST7305_ORIENTATION_LANDSCAPE || this->buffer_ == nullptr) {
    display::DisplayBuffer::draw_pixels_at(x_start, y_start, w, h, ptr, order, bitness, big_endian, x_offset,
                                           y_offset, x_pad);
    return;
  }
  const int stride = x_offset + w + x_pad;
  const int x0 = std::max(x_start, 0), x1 = std::min(x_start + w, (int) this->width_);
  const int y0 = std::max(y_start, 0), y1 = std::min(y_start + h, (int) this->height_);
  const uint32_t h4 = this->height_ >> 2;
  for (int y = y0; y < y1; y++) {
    const uint16_t *row = reinterpret_cast<const uint16_t *>(ptr) + (y - y_start + y_offset) * stride + x_offset;
    const int inv_y = this->height_ - 1 - y;
    const uint32_t block = inv_y >> 2;
    const int shift = (inv_y & 3) << 1;
    for (int x = x0; x < x1; x++) {
      uint16_t c = row[x - x_start];
      if (big_endian)
        c = (c >> 8) | (c << 8);
      // Same threshold as the simulator: 8-bit luminance (Rec. 601) above 127 is white.
      const uint32_t lum = (c >> 11) * 157573u + ((c >> 5) & 63) * 152033u + (c & 31) * 60078u;
      const uint8_t mask = 0x80 >> (shift | (x & 1));
      uint8_t &dest = this->buffer_[(x >> 1) * h4 + block];
      if (lum >= 128u * 64000u) {
        dest |= mask;
      } else {
        dest &= ~mask;
      }
    }
  }
}

void ST7305RLCD::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= this->width_ || y < 0 || y >= this->height_)
    return;

  // O(1) lookup using precomputed tables
  const uint32_t pixel_idx = static_cast<uint32_t>(x) * this->height_ + y;
  const uint16_t buffer_idx = this->pixel_index_lut_[pixel_idx];
  const uint8_t bit_mask = this->pixel_bit_lut_[pixel_idx];

  if (color.is_on()) {
    this->buffer_[buffer_idx] &= ~bit_mask;  // Black = bit clear
  } else {
    this->buffer_[buffer_idx] |= bit_mask;   // White = bit set
  }
}

// =============================================================================
// Hardware Initialization
// =============================================================================

void ST7305RLCD::hardware_reset_() {
  if (this->reset_pin_ == nullptr)
    return;

  // Hardware reset timing per ST7305 datasheet.
  // These delays are acceptable as they only occur during setup().
  this->reset_pin_->digital_write(true);
  delay(50);
  this->reset_pin_->digital_write(false);
  delay(20);
  this->reset_pin_->digital_write(true);
  delay(50);
}

void ST7305RLCD::init_display_() {
  // Initialization sequence from Waveshare reference driver
  // Most commands are common across ST7305 panels

  // NVM Load Control - Load settings from non-volatile memory
  this->send_command_(0xD6);
  this->send_data_(0x17);
  this->send_data_(0x02);

  // Booster Enable - Enable charge pump
  this->send_command_(0xD1);
  this->send_data_(0x01);

  // Gate Voltage Setting - VGH/VGL voltages
  this->send_command_(0xC0);
  this->send_data_(0x11);
  this->send_data_(0x04);

  // VSHP Setting - Positive source voltage (high power mode)
  this->send_command_(0xC1);
  this->send_data_(0x69);
  this->send_data_(0x69);
  this->send_data_(0x69);
  this->send_data_(0x69);

  // VSLP Setting - Positive source voltage (low power mode)
  this->send_command_(0xC2);
  this->send_data_(0x19);
  this->send_data_(0x19);
  this->send_data_(0x19);
  this->send_data_(0x19);

  // VSHN Setting - Negative source voltage (high power mode)
  this->send_command_(0xC4);
  this->send_data_(0x4B);
  this->send_data_(0x4B);
  this->send_data_(0x4B);
  this->send_data_(0x4B);

  // VSLN Setting - Negative source voltage (low power mode)
  this->send_command_(0xC5);
  this->send_data_(0x19);
  this->send_data_(0x19);
  this->send_data_(0x19);
  this->send_data_(0x19);

  // OSC Setting - Oscillator frequency control
  this->send_command_(0xD8);
  this->send_data_(0x80);
  this->send_data_(0xE9);

  // Frame Rate Control
  this->send_command_(0xB2);
  this->send_data_(0x02);

  // Gate EQ Control (High Power Mode) - Update period timing
  this->send_command_(0xB3);
  this->send_data_(0xE5);
  this->send_data_(0xF6);
  this->send_data_(0x05);
  this->send_data_(0x46);
  this->send_data_(0x77);
  this->send_data_(0x77);
  this->send_data_(0x77);
  this->send_data_(0x77);
  this->send_data_(0x76);
  this->send_data_(0x45);

  // Gate EQ Control (Low Power Mode) - Update period timing
  this->send_command_(0xB4);
  this->send_data_(0x05);
  this->send_data_(0x46);
  this->send_data_(0x77);
  this->send_data_(0x77);
  this->send_data_(0x77);
  this->send_data_(0x77);
  this->send_data_(0x76);
  this->send_data_(0x45);

  // Gate Timing Control
  this->send_command_(0x62);
  this->send_data_(0x32);
  this->send_data_(0x03);
  this->send_data_(0x1F);

  // Source EQ Enable
  this->send_command_(0xB7);
  this->send_data_(0x13);

  // Gate Line Setting - Number of gate lines (panel-specific)
  this->send_command_(0xB0);
  if (this->model_ == ST7305_MODEL_WAVESHARE_400X300) {
    this->send_data_(0x64);  // 100 * 3 = 300 lines
  } else if (this->model_ == ST7305_MODEL_OSPTEK_200X200) {
    this->send_data_(0x32);  // 50 * 4 = 200 lines
  } else {
    // Custom: calculate based on height
    this->send_data_(static_cast<uint8_t>(this->height_ / 3));
  }

  // Sleep Out - Exit sleep mode (required delay per datasheet, acceptable during setup)
  this->send_command_(0x11);
  delay(200);

  // Source Voltage Select - Use VSHP1/VSLP1/VSHN1/VSLN1
  this->send_command_(0xC9);
  this->send_data_(0x00);

  // Memory Data Access Control (MADCTL) - MX=1, DO=1
  this->send_command_(0x36);
  this->send_data_(0x48);

  // Data Format Select - 1-bit monochrome mode
  this->send_command_(0x3A);
  this->send_data_(0x11);

  // Gamma Mode Setting - Monochrome mode
  this->send_command_(0xB9);
  this->send_data_(0x20);

  // Panel Setting - 1-dot inversion, frame inversion, interlace
  this->send_command_(0xB8);
  this->send_data_(0x29);

  // Display Inversion On
  this->send_command_(0x21);

  // Column Address Set - Panel specific
  this->send_command_(0x2A);
  this->send_data_(this->col_start_);
  this->send_data_(this->col_end_);

  // Row Address Set - Panel specific
  this->send_command_(0x2B);
  this->send_data_(this->row_start_);
  this->send_data_(this->row_end_);

  // Tearing Effect Line On
  this->send_command_(0x35);
  this->send_data_(0x00);

  // Auto Power Down Control
  this->send_command_(0xD0);
  this->send_data_(0xFF);

  // Fill the panel memory (the buffer is still white) before Display On, so power-on
  // shows a blank frame instead of whatever the memory held.
  this->write_ram_();

  // High Power Mode On
  this->send_command_(0x38);

  // Display On
  this->send_command_(0x29);
}

// =============================================================================
// Pixel Lookup Table Initialization
// =============================================================================

void ST7305RLCD::init_pixel_lut_() {
  const uint32_t total_pixels = static_cast<uint32_t>(this->width_) * this->height_;

  // Allocate LUTs in PSRAM if available
  ExternalRAMAllocator<uint16_t> index_allocator(ExternalRAMAllocator<uint16_t>::ALLOW_FAILURE);
  ExternalRAMAllocator<uint8_t> bit_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);

  this->pixel_index_lut_ = index_allocator.allocate(total_pixels);
  this->pixel_bit_lut_ = bit_allocator.allocate(total_pixels);

  if (this->pixel_index_lut_ == nullptr || this->pixel_bit_lut_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate LUTs for %u pixels", total_pixels);
    return;
  }

  ESP_LOGD(TAG, "Building pixel LUTs for %ux%u (%s)...",
           this->width_, this->height_,
           this->orientation_ == ST7305_ORIENTATION_LANDSCAPE ? "landscape" : "portrait");

  if (this->orientation_ == ST7305_ORIENTATION_LANDSCAPE) {
    this->init_lut_landscape_();
  } else {
    this->init_lut_portrait_();
  }

  ESP_LOGD(TAG, "LUT initialization complete");
}

void ST7305RLCD::init_lut_landscape_() {
  // Landscape orientation: 2×4 pixel blocks
  // Reference: Waveshare InitLandscapeLUT() in custom_lcd_display.cc
  //
  // Each byte contains 8 pixels (2 columns × 4 rows):
  // - Bit 7: (row 0, col 0), Bit 6: (row 0, col 1)
  // - Bit 5: (row 1, col 0), Bit 4: (row 1, col 1)
  // - Bit 3: (row 2, col 0), Bit 2: (row 2, col 1)
  // - Bit 1: (row 3, col 0), Bit 0: (row 3, col 1)

  const uint16_t H4 = this->height_ >> 2;  // Vertical blocks (height/4)

  for (uint16_t y = 0; y < this->height_; y++) {
    const uint16_t inv_y = this->height_ - 1 - y;
    const uint16_t block_y = inv_y >> 2;
    const uint8_t local_y = inv_y & 3;

    for (uint16_t x = 0; x < this->width_; x++) {
      const uint16_t byte_x = x >> 1;
      const uint8_t local_x = x & 1;

      const uint32_t buffer_idx = byte_x * H4 + block_y;
      const uint8_t bit = 7 - ((local_y << 1) | local_x);

      const uint32_t pixel_idx = static_cast<uint32_t>(x) * this->height_ + y;
      this->pixel_index_lut_[pixel_idx] = buffer_idx;
      this->pixel_bit_lut_[pixel_idx] = (1 << bit);
    }
  }
}

void ST7305RLCD::init_lut_portrait_() {
  // Portrait orientation: 4×2 pixel blocks
  // Reference: Waveshare InitPortraitLUT()
  //
  // Each byte contains 8 pixels (4 columns × 2 rows):
  //   col0 col1 col2 col3
  // row0  b7   b6   b5   b4
  // row1  b3   b2   b1   b0
  //
  // Bit position = 7 - (row * 4 + col)

  const uint16_t W4 = this->width_ >> 2;  // Horizontal blocks (width/4)

  for (uint16_t y = 0; y < this->height_; y++) {
    const uint16_t byte_y = y >> 1;
    const uint8_t local_y = y & 1;

    for (uint16_t x = 0; x < this->width_; x++) {
      const uint16_t byte_x = x >> 2;
      const uint8_t local_x = x & 3;

      const uint32_t buffer_idx = byte_y * W4 + byte_x;
      const uint8_t bit = 7 - (local_y * 4 + local_x);

      const uint32_t pixel_idx = static_cast<uint32_t>(x) * this->height_ + y;
      this->pixel_index_lut_[pixel_idx] = buffer_idx;
      this->pixel_bit_lut_[pixel_idx] = (1 << bit);
    }
  }
}

// =============================================================================
// Display Write
// =============================================================================

void ST7305RLCD::write_display_() {
  if (this->buffer_ == nullptr)
    return;

  // Ensure display is awake
  this->send_command_(0x38);  // High Power Mode
  this->send_command_(0x29);  // Display On
  this->write_ram_();
}

void ST7305RLCD::write_ram_() {
  // Set column address window
  this->send_command_(0x2A);
  this->send_data_(this->col_start_);
  this->send_data_(this->col_end_);

  // Set row address window
  this->send_command_(0x2B);
  this->send_data_(this->row_start_);
  this->send_data_(this->row_end_);

  // Memory Write - CS must stay LOW for command + all data bytes
  this->dc_pin_->digital_write(false);  // Command mode
  this->enable();                        // CS LOW
  this->write_byte(0x2C);               // Memory Write command

  this->dc_pin_->digital_write(true);   // Data mode (CS still LOW)
  this->write_array(this->buffer_, this->buffer_size_);
  this->disable();                       // CS HIGH
}

// =============================================================================
// SPI Helpers
// =============================================================================

void ST7305RLCD::send_command_(uint8_t cmd) {
  this->dc_pin_->digital_write(false);
  this->enable();
  this->write_byte(cmd);
  this->disable();
}

void ST7305RLCD::send_data_(uint8_t data) {
  this->dc_pin_->digital_write(true);
  this->enable();
  this->write_byte(data);
  this->disable();
}

// =============================================================================
// Power Control
// =============================================================================

void ST7305RLCD::sleep() {
  this->send_command_(0x10);  // Sleep In
  ESP_LOGD(TAG, "Entered sleep mode");
}

void ST7305RLCD::wake() {
  this->send_command_(0x11);  // Sleep Out
  // Per ST7305 datasheet: 120ms delay required after sleep out before sending commands.
  // This is acceptable because wake() is only called explicitly by user from lambda,
  // not from loop() or update().
  delay(120);
  ESP_LOGD(TAG, "Exited sleep mode");
}

void ST7305RLCD::low_power_mode() {
  this->send_command_(0x39);  // Low Power Mode
  ESP_LOGD(TAG, "Switched to low power mode");
}

void ST7305RLCD::high_power_mode() {
  this->send_command_(0x38);  // High Power Mode
  ESP_LOGD(TAG, "Switched to high power mode");
}

void ST7305RLCD::display_on() {
  this->send_command_(0x29);  // Display On
  ESP_LOGD(TAG, "Display on");
}

void ST7305RLCD::display_off() {
  this->send_command_(0x28);  // Display Off
  ESP_LOGD(TAG, "Display off");
}

}  // namespace st7305_rlcd
}  // namespace esphome
