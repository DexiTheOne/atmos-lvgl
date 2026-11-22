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
 * Modified for the Atmos project (2026-10-18): native 1-bit load_rows_i1()/present(), pushes
 * by DMA from a push task, timed to the TE signal and limited to the changed window (2026-11-20),
 * bulk RGB565 draw_pixels_at() for LVGL,
 * display buffer in internal RAM, and a start-up check of the bulk layout against the
 * pixel LUTs. Original: https://github.com/kylehase/ESPHome-ST7305-RLCD (Apache-2.0).
 */

#include "st7305_rlcd.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <vector>
#include <cstring>
#include <esp_heap_caps.h>

namespace esphome {
namespace st7305_rlcd {

static const char *const TAG = "st7305_rlcd";

// =============================================================================
// Setup and Configuration
// =============================================================================

void ST7305RLCD::setup() {
  this->te_gap_max_us_ = 3000000u / this->refresh_hz_x2_;  // 1.5 * 2e6 / hz_x2
  ESP_LOGCONFIG(TAG, "Setting up ST7305 RLCD...");

  // Apply model-specific settings
  this->apply_model_settings_();

  // Configure pins
  this->dc_pin_->setup();
  this->dc_pin_->digital_write(true);

  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
  }
  if (this->te_pin_ != nullptr) {
    this->te_pin_->setup();
    this->te_pin_->attach_interrupt(&ST7305RLCD::te_isr_, this, gpio::INTERRUPT_RISING_EDGE);
  }

  // SPI: the driver's own device on the bus ESPHome's spi: component initialised (with
  // DMA), so pushes can be queued and run while the CPU renders.
  if (!this->setup_spi_()) {
    this->mark_failed();
    return;
  }

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
  this->start_push_task_();

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
bool HOT ST7305RLCD::load_rows_i1(const uint8_t *frame, int y1, int y2, int x1, int x2) {
  if (this->buffer_ == nullptr || this->orientation_ != ST7305_ORIENTATION_LANDSCAPE || this->width_ != 400 ||
      this->height_ % 4 != 0)
    return false;
  const int h = this->height_, h4 = h >> 2, row_bytes = this->width_ / 8;
  y1 = std::max(y1, 0);
  y2 = std::min(y2, h - 1);
  // Whole source bytes (8 columns) covering x1..x2.
  const int sx1 = std::max(x1, 0) >> 3, sx2 = std::min(x2, (int) this->width_ - 1) >> 3;
  if (y1 > y2 || sx1 > sx2)
    return true;
  this->mark_dirty_(y1, y2, sx1 * 8, sx2 * 8 + 7);
  // Blocks covering the rows (screen rows run bottom-up through the blocks).
  const int b_first = (h - 1 - y2) >> 2, b_last = (h - 1 - y1) >> 2;
  for (int b = b_first; b <= b_last; b++) {
    const uint8_t *r0 = frame + (h - 1 - 4 * b) * row_bytes;
    const uint8_t *r1 = r0 - row_bytes, *r2 = r1 - row_bytes, *r3 = r2 - row_bytes;
    uint8_t *dest = this->buffer_ + b;
    for (int sx = sx1; sx <= sx2; sx++) {
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
  if (!this->load_rows_i1(frame.data(), 0, this->height_ - 1, 0, this->width_ - 1))
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
  this->dirty_ = false;
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

  // VSHP Setting - Positive source voltage (high power mode), source_voltage_positive
  this->send_command_(0xC1);
  for (int i = 0; i < 4; i++)
    this->send_data_(this->vshp_);

  // VSLP Setting - Positive source voltage (low power mode)
  this->send_command_(0xC2);
  this->send_data_(0x19);
  this->send_data_(0x19);
  this->send_data_(0x19);
  this->send_data_(0x19);

  // VSHN Setting - Negative source voltage (high power mode), source_voltage_negative.
  // Waveshare's 0x4B (-4.0 V) left the 1-dot inversion polarities unbalanced (a fine
  // checkerboard over black); 0x19 (-3.0 V) balances them on our panel.
  this->send_command_(0xC4);
  for (int i = 0; i < 4; i++)
    this->send_data_(this->vshn_);

  // VSLN Setting - Negative source voltage (low power mode)
  this->send_command_(0xC5);
  this->send_data_(0x19);
  this->send_data_(0x19);
  this->send_data_(0x19);
  this->send_data_(0x19);

  // OSC Setting - Oscillator frequency control
  // OSCSW=000 (0x80) gives 25.5/51 Hz, OSCSW=111 (0xA6) 16/32 Hz; FRCTRL picks the half.
  this->send_command_(0xD8);
  this->send_data_(this->refresh_hz_x2_ == 102 || this->refresh_hz_x2_ == 51 ? 0x80 : 0xA6);
  this->send_data_(0xE9);

  // Frame Rate Control: HFRA (bit 4) picks the faster of the OSC setting's two
  // high-power rates (16/32 or 25.5/51 Hz); LFRA (low_power_refresh_rate) is the
  // low-power rate, 0.25-8 Hz. 51 Hz is
  // the controller's maximum (datasheet v0.2, FRCTRL/OSCSET).
  this->send_command_(0xB2);
  this->send_data_((this->refresh_hz_x2_ == 102 || this->refresh_hz_x2_ == 64 ? 0x10 : 0x00) | this->lfra_);

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
// Display Write (Atmos: own DMA device, push task, TE-timed windowed pushes)
// =============================================================================
//
// The panel's memory is addressed as 200 rows (2Bh, one per pair of screen columns,
// left to right: the panel refreshes in this order) of 25 column units (2Ah, 3 bytes =
// 12 screen rows each). load_rows_i1() converts into buffer_ and grows the dirty window;
// present() packs the window into tx_buf_ (DMA-capable) and arms the push task, which
// sends it at the next TE rising edge (start of the vertical blank) at or after the
// target pulse, or at once without a TE pin. A 15 KB push at 40 MHz takes 3.6 ms, less
// than an eighth of a refresh, so the scan never overtakes the write: no tearing.

static const size_t CHUNK = 4092;  // the bus's max_transfer_sz (ESPHome's SPI component)

bool ST7305RLCD::setup_spi_() {
  this->cs_->setup();
  this->cs_->digital_write(true);
  spi_device_interface_config_t config = {};
  config.mode = 0;
  config.clock_speed_hz = (int) this->data_rate_;
  config.spics_io_num = -1;  // CS is held low by hand across a whole push
  config.queue_size = 8;
  config.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;
  // The bus ESPHome's spi: component initialised (its host is not exposed in every
  // ESPHome version): adding a device fails on a host without an initialised bus.
  esp_err_t err = spi_bus_add_device(SPI2_HOST, &config, &this->spi_);
  if (err != ESP_OK)
    err = spi_bus_add_device(SPI3_HOST, &config, &this->spi_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_device failed: %d", err);
    return false;
  }
  this->tx_buf_ = (uint8_t *) heap_caps_malloc(this->buffer_size_, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (this->tx_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate the DMA buffer");
    return false;
  }
  return true;
}

void ST7305RLCD::start_push_task_() {
  xTaskCreatePinnedToCore(&ST7305RLCD::push_task_, "st7305_push", 3072, this, 5, &this->push_task_handle_,
                          tskNO_AFFINITY);
}

void IRAM_ATTR ST7305RLCD::te_isr_(ST7305RLCD *self) {
  uint32_t now = micros();
  uint32_t dt = now - self->te_time_us_;
  // Refresh period EMA from gaps near the configured rate only (under 1.5 nominal
  // periods): low-power refreshes and mode switches left gaps near 100 ms that pulled
  // it to 10 Hz, and the model, stepped by it, ran 3.4x fast.
  bool hpm_gap = dt > 5000 && dt < self->te_gap_max_us_;
  if (self->te_count_ > 1 && hpm_gap)
    self->te_period_q4_ += (int32_t) (dt * 16 - self->te_period_q4_) / 16;  // EMA, 1/16
  else if (self->te_count_ == 1 && hpm_gap)
    self->te_period_q4_ = dt * 16;
  self->te_gap_us_ = dt;
  self->te_time_us_ = now;
  self->te_count_ = self->te_count_ + 1;
  if (self->armed_ && (int32_t) (self->te_count_ - self->target_te_) >= 0) {
    self->armed_ = false;
    self->push_edge_us_ = now;
    self->push_te_ = self->te_count_;
    self->push_target_ = self->target_te_;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(self->push_task_handle_, &woken);
    portYIELD_FROM_ISR(woken);
  }
}

void ST7305RLCD::push_task_(void *arg) {
  auto *self = static_cast<ST7305RLCD *>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t start = micros();
    uint32_t edge = self->push_edge_us_, te = self->push_te_, target = self->push_target_;
    self->send_window_(self->tx_win_);
    // Record of the completed push (the ISR overwrites the live fields at the next one).
    self->done_.te = te;
    self->done_.target = target;
    self->done_.edge_us = edge;
    self->done_.start_us = start;
    self->done_.us = micros() - start;
    self->done_.bytes = self->push_bytes_;
    self->pushes_ = self->pushes_ + 1;
    self->busy_ = false;
  }
}

void ST7305RLCD::transmit_(const uint8_t *data, size_t len, bool dc) {
  this->dc_pin_->digital_write(dc);
  spi_transaction_t t = {};
  t.length = len * 8;
  if (len <= 4) {
    t.flags = SPI_TRANS_USE_TXDATA;
    memcpy(t.tx_data, data, len);
  } else {
    t.tx_buffer = data;
  }
  spi_device_polling_transmit(this->spi_, &t);
}

/* One push: address window, Memory Write, then the packed data as queued DMA
   transactions (interrupt driven: the calling task sleeps while they run). */
void ST7305RLCD::send_window_(const Window &w) {
  // MADCTL MX=1: columns fill from the window's end address down, so buffer unit u
  // (its order in a full-frame write) sits at col_end - u.
  const uint8_t ca[2] = {(uint8_t) (this->col_end_ - w.u2), (uint8_t) (this->col_end_ - w.u1)};
  const uint8_t ra[2] = {(uint8_t) (this->row_start_ + w.r1), (uint8_t) (this->row_start_ + w.r2)};
  const uint8_t c2a = 0x2A, c2b = 0x2B, c2c = 0x2C;
  this->cs_->digital_write(false);
  this->transmit_(&c2a, 1, false);
  this->transmit_(ca, 2, true);
  this->transmit_(&c2b, 1, false);
  this->transmit_(ra, 2, true);
  this->transmit_(&c2c, 1, false);
  this->dc_pin_->digital_write(true);
  size_t len = (size_t) (w.r2 - w.r1 + 1) * (w.u2 - w.u1 + 1) * 3;
  static spi_transaction_t trans[8];
  int n = 0;
  for (size_t off = 0; off < len && n < 8; off += CHUNK, n++) {
    trans[n] = {};
    trans[n].length = std::min(CHUNK, len - off) * 8;
    trans[n].tx_buffer = this->tx_buf_ + off;
    spi_device_queue_trans(this->spi_, &trans[n], portMAX_DELAY);
  }
  spi_transaction_t *done;
  for (int i = 0; i < n; i++)
    spi_device_get_trans_result(this->spi_, &done, portMAX_DELAY);
  this->cs_->digital_write(true);
  this->push_bytes_ = len;
}

/* Packs the window of buffer_ into tx_buf_, row by row. */
void ST7305RLCD::pack_window_(const Window &w) {
  const int row_bytes = this->height_ >> 2;  // 75
  const int n = (w.u2 - w.u1 + 1) * 3;
  uint8_t *dst = this->tx_buf_;
  for (int r = w.r1; r <= w.r2; r++, dst += n)
    memcpy(dst, this->buffer_ + r * row_bytes + w.u1 * 3, n);
}

void ST7305RLCD::mark_dirty_(int y1, int y2, int x1, int x2) {
  const int h = this->height_;
  // Screen rows run bottom-up through the 4-row blocks; 3 blocks per column unit.
  int u1 = ((h - 1 - y2) >> 2) / 3, u2 = ((h - 1 - y1) >> 2) / 3;
  int r1 = x1 >> 1, r2 = x2 >> 1;
  if (!this->partial_) {
    u1 = 0, u2 = (h >> 2) / 3 - 1, r1 = 0, r2 = (this->width_ >> 1) - 1;
  }
  if (!this->dirty_) {
    this->win_ = {(uint8_t) r1, (uint8_t) r2, (uint8_t) u1, (uint8_t) u2};
    this->dirty_ = true;
    return;
  }
  this->win_.r1 = std::min<int>(this->win_.r1, r1);
  this->win_.r2 = std::max<int>(this->win_.r2, r2);
  this->win_.u1 = std::min<int>(this->win_.u1, u1);
  this->win_.u2 = std::max<int>(this->win_.u2, u2);
}

bool ST7305RLCD::present(uint32_t target_te) {
  if (!this->dirty_)
    return true;  // nothing changed
  if (this->busy_)
    return false;
  this->pack_window_(this->win_);
  this->tx_win_ = this->win_;
  this->dirty_ = false;
  this->busy_ = true;
  if (this->te_pin_ == nullptr || this->untimed_ || this->push_task_handle_ == nullptr) {
    this->push_edge_us_ = micros();
    this->push_te_ = this->push_target_ = this->te_count_;
    if (this->push_task_handle_ != nullptr) {
      xTaskNotifyGive(this->push_task_handle_);
    } else {
      uint32_t start = micros();
      this->send_window_(this->tx_win_);
      this->done_ = {this->push_te_, this->push_target_, this->push_edge_us_, start, micros() - start,
                     this->push_bytes_};
      this->pushes_ = this->pushes_ + 1;
      this->busy_ = false;
    }
    return true;
  }
  this->target_te_ = target_te;
  this->armed_ = true;  // the ISR starts the push at the target TE pulse or later
  return true;
}

void ST7305RLCD::push_now() {
  while (this->busy_)
    delay(1);
  this->win_ = {0, (uint8_t) ((this->width_ >> 1) - 1), 0, (uint8_t) ((this->height_ >> 2) / 3 - 1)};
  this->pack_window_(this->win_);
  this->tx_win_ = this->win_;
  this->dirty_ = false;
  this->send_window_(this->tx_win_);
}

void ST7305RLCD::write_display_() {
  if (this->buffer_ == nullptr)
    return;
  this->push_now();
}

void ST7305RLCD::write_ram_() { this->push_now(); }

// =============================================================================
// SPI Helpers (setup and power control only; never during a push)
// =============================================================================

void ST7305RLCD::send_command_(uint8_t cmd) {
  this->cs_->digital_write(false);
  this->transmit_(&cmd, 1, false);
  this->cs_->digital_write(true);
}

void ST7305RLCD::send_data_(uint8_t data) {
  this->cs_->digital_write(false);
  this->transmit_(&data, 1, true);
  this->cs_->digital_write(true);
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

bool ST7305RLCD::command(uint8_t cmd, const uint8_t *data, size_t len) {
  if (this->busy_ || this->armed_)
    return false;
  this->cs_->digital_write(false);
  this->transmit_(&cmd, 1, false);
  for (size_t i = 0; i < len; i++)
    this->transmit_(data + i, 1, true);
  this->cs_->digital_write(true);
  char hex[3 * 32 + 1] = "";
  for (size_t i = 0; i < len && i < 32; i++)
    snprintf(hex + 3 * i, 4, " %02X", data[i]);
  ESP_LOGI(TAG, "panel command %02Xh:%s", cmd, hex);
  return true;
}

void ST7305RLCD::set_high_power_slow(bool slow) {
  bool fast = this->refresh_hz_x2_ == 102 || this->refresh_hz_x2_ == 64;
  uint8_t fr = (fast && !slow ? 0x10 : 0x00) | this->lfra_;
  this->send_command_(0xB2);
  this->send_data_(fr);
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
