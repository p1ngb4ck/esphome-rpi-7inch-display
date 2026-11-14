#ifdef USE_ESP32_VARIANT_ESP32P4
#include <algorithm>
#include <utility>
#include "mipi_dsi.h"
#include "esphome/core/helpers.h"
#include "hal/mipi_dsi_hal.h"
#ifdef USE_MIPI_DSI_BOOT_IMAGE
#include <cmath>
#include "esp_cache.h"
#endif
#include "hal/mipi_dsi_host_ll.h"

#include <hal/axi_icm_ll.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::mipi_dsi {

// Maximum bytes to log for init commands (truncated if larger)
static constexpr size_t MIPI_DSI_MAX_CMD_LOG_BYTES = 64;
// Highest AXI QoS priority (4 bits); all masters start at 0
static constexpr uint32_t DSI_DMA_READ_PRIORITY = 15;

// With CONFIG_LCD_DSI_ISR_CACHE_SAFE the DPI driver only accepts callbacks in IRAM
#if CONFIG_LCD_DSI_ISR_CACHE_SAFE
#define MIPI_DSI_ISR_ATTR IRAM_ATTR
#else
#define MIPI_DSI_ISR_ATTR
#endif

bool MIPI_DSI_ISR_ATTR MipiDsi::notify_refresh_ready_(esp_lcd_panel_handle_t panel,
                                                      esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
  BaseType_t need_yield = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<MipiDsi *>(user_ctx)->io_lock_, &need_yield);
  return (need_yield == pdTRUE);
}

bool MIPI_DSI_ISR_ATTR MipiDsi::notify_frame_started_(esp_lcd_panel_handle_t panel,
                                                      esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
  auto *self = static_cast<MipiDsi *>(user_ctx);
  const uint32_t frames = self->scan_frames_.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (self->flip_waiting_.load(std::memory_order_acquire) &&
      static_cast<int32_t>(frames - self->flip_frame_.load(std::memory_order_acquire)) >= 0) {
    self->flip_waiting_.store(false, std::memory_order_release);
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(self->flip_sem_, &need_yield);
    return need_yield == pdTRUE;
  }
  return false;
}

void MipiDsi::smark_failed(const LogString *message, esp_err_t err) {
  ESP_LOGE(TAG, "%s: %s", LOG_STR_ARG(message), esp_err_to_name(err));
  this->mark_failed(message);
}

void MipiDsi::setup() {
  ESP_LOGCONFIG(TAG, "Running Setup");

  if (!this->enable_pins_.empty()) {
    for (auto *pin : this->enable_pins_) {
      pin->setup();
      pin->digital_write(true);
    }
    delay(10);
  }

  esp_lcd_dsi_bus_config_t bus_config = {
      .bus_id = 0,  // index from 0, specify the DSI host to use
      .num_data_lanes =
          this->lanes_,  // Number of data lanes to use, can't set a value that exceeds the chip's capability
      // phy_clk_src left at 0 to enable runtime auto-select.
      .lane_bit_rate_mbps = this->lane_bit_rate_,  // Bit rate of the data lanes, in Mbps
      .flags = {.clock_lane_force_hs = this->continuous_clock_},
  };
  auto err = esp_lcd_new_dsi_bus(&bus_config, &this->bus_handle_);
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("lcd_new_dsi_bus failed"), err);
    return;
  }
  if (!this->video_mode_only_) {
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,    // according to the LCD spec
        .lcd_param_bits = 8,  // according to the LCD spec
    };
    err = esp_lcd_new_panel_io_dbi(this->bus_handle_, &dbi_config, &this->io_handle_);
    if (err != ESP_OK) {
      this->smark_failed(LOG_STR("new_panel_io_dbi failed"), err);
      return;
    }
  }
  // clang-format off
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  auto color_format = LCD_COLOR_FMT_RGB565;
  if (this->color_depth_ == display::COLOR_BITNESS_888) {
    color_format = LCD_COLOR_FMT_RGB888;
  }
  esp_lcd_dpi_panel_config_t dpi_config = {.virtual_channel = 0,
                                           .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
                                           .dpi_clock_freq_mhz = this->pclk_frequency_,
                                           .in_color_format = color_format,
#else
  auto pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
  if (this->color_depth_ == display::COLOR_BITNESS_888) {
    pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
  }
  esp_lcd_dpi_panel_config_t dpi_config = {.virtual_channel = 0,
                                           .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
                                           .dpi_clock_freq_mhz = this->pclk_frequency_,
                                           .pixel_format = pixel_format,
#endif
                                           .num_fbs = this->num_fbs_,
                                           .video_timing =
                                               {
                                                   .h_size = this->width_,
                                                   .v_size = this->height_,
                                                   .hsync_pulse_width = this->hsync_pulse_width_,
                                                   .hsync_back_porch = this->hsync_back_porch_,
                                                   .hsync_front_porch = this->hsync_front_porch_,
                                                   .vsync_pulse_width = this->vsync_pulse_width_,
                                                   .vsync_back_porch = this->vsync_back_porch_,
                                                   .vsync_front_porch = this->vsync_front_porch_,
                                               },
                                           .flags = {
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
                                               .use_dma2d = true,
#endif
                                           }};
  // clang-format on
  err = esp_lcd_new_panel_dpi(this->bus_handle_, &dpi_config, &this->handle_);
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("esp_lcd_new_panel_dpi failed"), err);
    return;
  }
  MIPI_DSI_LL_GET_HOST(0)->vid_mode_cfg.vid_mode_type = this->video_mode_;
  MIPI_DSI_LL_GET_HOST(0)->vid_mode_cfg.frame_bta_ack_en = this->frame_ack_;
  mipi_dsi_host_ll_enable_tx_eotp(MIPI_DSI_LL_GET_HOST(0), this->eotp_, false);
  mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(MIPI_DSI_LL_GET_HOST(0), this->lp_horizontal_blanking_,
                                                   this->lp_horizontal_blanking_);
  mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(MIPI_DSI_LL_GET_HOST(0), true, true, true, this->lp_vertical_active_);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  err = esp_lcd_dpi_panel_enable_dma2d(this->handle_);
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("esp_lcd_dpi_panel_enable_dma2d failed"), err);
    return;
  }
#endif
  if (this->reset_pin_ != nullptr && this->video_mode_only_) {
    pulse_reset(this->reset_pin_, {.low_ms = 100, .high_after_ms = 200});
  } else if (this->reset_pin_ != nullptr) {
    pulse_reset(this->reset_pin_, {.high_before_ms = 5, .low_ms = 5});
  } else if (!this->generic_commands_ && !this->video_mode_only_) {
    esp_lcd_panel_io_tx_param(this->io_handle_, SW_RESET_CMD, nullptr, 0);
  }
  // need to know when the display is ready for SLPOUT command - will be 120ms after reset
  auto when = millis() + 120;
  if (this->generic_commands_) {
    MIPI_DSI_LL_GET_HOST(0)->cmd_mode_cfg.ack_rqst_en = 0;
    if (!this->send_init_sequence_(when))
      return;
    this->log_read_registers_();
  }
  err = esp_lcd_panel_init(this->handle_);
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("esp_lcd_init failed"), err);
    return;
  }
  if (!this->generic_commands_ && !this->send_init_sequence_(when))
    return;
  this->io_lock_ = xSemaphoreCreateBinary();
  this->flip_sem_ = xSemaphoreCreateBinary();
  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_color_trans_done = notify_refresh_ready_;
  cbs.on_frame_buf_complete = notify_frame_started_;

  err = (esp_lcd_dpi_panel_register_event_callbacks(this->handle_, &cbs, this));
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("Failed to register callbacks"), err);
    return;
  }

  if (this->num_fbs_ >= 2) {
    esp_err_t fberr;
    if (this->num_fbs_ >= 3) {
      fberr = esp_lcd_dpi_panel_get_frame_buffer(this->handle_, 3, &this->dsi_fbs_[0], &this->dsi_fbs_[1],
                                                 &this->dsi_fbs_[2]);
    } else {
      fberr = esp_lcd_dpi_panel_get_frame_buffer(this->handle_, 2, &this->dsi_fbs_[0], &this->dsi_fbs_[1]);
    }
    if (fberr != ESP_OK) {
      ESP_LOGW(TAG, "esp_lcd_dpi_panel_get_frame_buffer failed: %s", esp_err_to_name(fberr));
      this->dsi_fbs_[0] = this->dsi_fbs_[1] = this->dsi_fbs_[2] = nullptr;
    }
    this->draw_fb_ = this->dsi_fbs_[0];
  } else if (esp_lcd_dpi_panel_get_frame_buffer(this->handle_, 1, &this->draw_fb_) != ESP_OK) {
    this->draw_fb_ = nullptr;
  }

  // The panel reads its frame buffer from PSRAM all the time through DW-GDMA. With all AXI masters at the same
  // priority, heavy PSRAM use by the CPU or the PPA can hold those reads up until the panel runs out of data and
  // shows a corrupt frame. Raise the read priority on both DW-GDMA master ports; the panel does not write.
  axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 0, DSI_DMA_READ_PRIORITY);
  axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 0, DSI_DMA_READ_PRIORITY);

#ifdef USE_MIPI_DSI_BOOT_IMAGE
  this->boot_start_();
#endif
  ESP_LOGCONFIG(TAG, "MIPI DSI setup complete");
}

#ifdef USE_MIPI_DSI_BOOT_IMAGE
static constexpr int BOOT_RING_RADIUS = 112;
static constexpr float BOOT_RING_HALF_WIDTH = 2.5f;
static constexpr float BOOT_HEAD_RADIUS = 6.0f;
static constexpr int BOOT_HEAD_DOT = 8;
static constexpr int BOOT_RING_POINTS = 180;
static constexpr int BOOT_START_PERCENT = 10;
static constexpr int BOOT_ADVANCE = 3;
static constexpr int BOOT_FILL_START = 60;
static constexpr uint64_t BOOT_TICK_US = 40000;

void MipiDsi::boot_rect_(int x, int y, int w, int h, Color color) {
  auto *fb = static_cast<uint16_t *>(this->draw_fb_);
  const int stride = static_cast<int>(this->width_);
  const uint16_t raw = display::ColorUtil::color_to_565(color);
  const uint16_t value = this->big_endian_ ? convert_big_endian(raw) : raw;
  for (int row = 0; row < h; row++)
    std::fill_n(fb + (y + row) * stride + x, w, value);
}

void MipiDsi::boot_sync_(int top, int rows) {
  auto *fb = static_cast<uint16_t *>(this->draw_fb_);
  const int stride = static_cast<int>(this->width_);
  esp_cache_msync(fb + top * stride, static_cast<size_t>(rows * stride) * 2,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static Color boot_mix(const Color &from, const Color &to, int weight) {
  const auto mix = [weight](uint8_t a, uint8_t b) { return static_cast<uint8_t>(a + (b - a) * weight / 255); };
  return Color(mix(from.r, to.r), mix(from.g, to.g), mix(from.b, to.b));
}

void MipiDsi::boot_ring_paint_(int head) {
  const Color ring(0x2A, 0x2E, 0x3D);
  const Color accent(0x7A, 0xA2, 0xF7);
  auto *fb = static_cast<uint16_t *>(this->draw_fb_);
  const int stride = static_cast<int>(this->width_);
  const float outer = BOOT_RING_RADIUS + BOOT_RING_HALF_WIDTH + 1.0f;
  const float inner = BOOT_RING_RADIUS - BOOT_RING_HALF_WIDTH - 1.0f;
  const int rows = static_cast<int>(ceilf(outer));
  const float turn = 2.0f * static_cast<float>(M_PI);
  const auto put = [&](int dx, int dy, float y2) {
    const float distance = sqrtf(static_cast<float>(dx * dx) + y2);
    const float cover = std::min(1.0f, BOOT_RING_HALF_WIDTH + 0.5f - fabsf(distance - BOOT_RING_RADIUS));
    if (cover <= 0.0f)
      return;
    float angle = atan2f(static_cast<float>(dx), static_cast<float>(-dy));
    if (angle < 0.0f)
      angle += turn;
    const float position = angle / turn * BOOT_RING_POINTS;
    Color color = ring;
    if (head > 0 && position <= static_cast<float>(head))
      color = boot_mix(ring, accent, BOOT_FILL_START + static_cast<int>((255 - BOOT_FILL_START) * position / head));
    const uint16_t raw = display::ColorUtil::color_to_565(boot_mix(this->boot_background_, color, static_cast<int>(cover * 255.0f)));
    fb[(this->boot_cy_ + dy) * stride + this->boot_cx_ + dx] = this->big_endian_ ? convert_big_endian(raw) : raw;
  };
  for (int dy = -rows; dy <= rows; dy++) {
    const float y2 = static_cast<float>(dy * dy);
    if (y2 > outer * outer)
      continue;
    const int from = y2 < inner * inner ? static_cast<int>(floorf(sqrtf(inner * inner - y2))) : 0;
    const int to = static_cast<int>(ceilf(sqrtf(outer * outer - y2)));
    for (int dx = from; dx <= to; dx++) {
      put(dx, dy, y2);
      if (dx != 0)
        put(-dx, dy, y2);
    }
  }
}

void MipiDsi::boot_head_paint_(int head, Color color) {
  auto *fb = static_cast<uint16_t *>(this->draw_fb_);
  const int stride = static_cast<int>(this->width_);
  const float angle = static_cast<float>(head) * 2.0f * static_cast<float>(M_PI) / BOOT_RING_POINTS;
  const float cx = static_cast<float>(this->boot_cx_) + sinf(angle) * BOOT_RING_RADIUS;
  const float cy = static_cast<float>(this->boot_cy_) - cosf(angle) * BOOT_RING_RADIUS;
  const int reach = static_cast<int>(ceilf(BOOT_HEAD_RADIUS + 1.0f));
  for (int y = static_cast<int>(cy) - reach; y <= static_cast<int>(cy) + reach; y++) {
    for (int x = static_cast<int>(cx) - reach; x <= static_cast<int>(cx) + reach; x++) {
      const float dx = static_cast<float>(x) - cx;
      const float dy = static_cast<float>(y) - cy;
      const float cover = std::min(1.0f, BOOT_HEAD_RADIUS + 0.5f - sqrtf(dx * dx + dy * dy));
      if (cover <= 0.0f)
        continue;
      uint16_t &pixel = fb[y * stride + x];
      const uint16_t now = this->big_endian_ ? convert_big_endian(pixel) : pixel;
      const Color under((now >> 11 & 0x1F) * 255 / 31, (now >> 5 & 0x3F) * 255 / 63, (now & 0x1F) * 255 / 31);
      const uint16_t raw = display::ColorUtil::color_to_565(boot_mix(under, color, static_cast<int>(cover * 255.0f)));
      pixel = this->big_endian_ ? convert_big_endian(raw) : raw;
    }
  }
}

void MipiDsi::boot_start_() {
  if (this->boot_image_ == nullptr || this->draw_fb_ == nullptr || this->get_bytes_per_pixel() != 2)
    return;
  const int image_w = this->boot_image_->get_width();
  const int image_h = this->boot_image_->get_height();
  const int fb_w = static_cast<int>(this->width_);
  const int fb_h = static_cast<int>(this->height_);
  if (image_w > fb_w || image_h > fb_h || fb_h / 2 < BOOT_RING_RADIUS + BOOT_HEAD_DOT)
    return;
  auto *fb = static_cast<uint16_t *>(this->draw_fb_);
  const auto pixel = [this](int x, int y) {
    const uint16_t value = display::ColorUtil::color_to_565(this->boot_image_->get_pixel(x, y));
    return this->big_endian_ ? convert_big_endian(value) : value;
  };
  this->boot_background_ = this->boot_image_->get_pixel(0, 0);
  std::fill_n(fb, fb_w * fb_h, pixel(0, 0));
  const int left = (fb_w - image_w) / 2;
  const int top = (fb_h - image_h) / 2;
  for (int y = 0; y < image_h; y++) {
    for (int x = 0; x < image_w; x++)
      fb[(top + y) * fb_w + left + x] = pixel(x, y);
  }
  this->boot_cx_ = fb_w / 2;
  this->boot_cy_ = fb_h / 2;
  this->boot_base_ = std::min<uint8_t>(App.setup_percent(), 99);
  this->boot_ring_paint_(0);
  this->boot_sync_(0, fb_h);
  esp_timer_create_args_t args{};
  args.callback = [](void *arg) { static_cast<MipiDsi *>(arg)->boot_tick_(); };
  args.arg = this;
  args.name = "boot_screen";
  if (esp_timer_create(&args, &this->boot_timer_) == ESP_OK)
    esp_timer_start_periodic(this->boot_timer_, BOOT_TICK_US);
}

void MipiDsi::boot_tick_() {
  const Color accent(0x7A, 0xA2, 0xF7);
  const int raw = std::max<int>(this->boot_base_, std::max<uint8_t>(App.setup_percent(), this->boot_percent_));
  const int mapped = BOOT_START_PERCENT + (raw - this->boot_base_) * (100 - BOOT_START_PERCENT) / (100 - this->boot_base_);
  this->boot_shown_ = std::max<uint8_t>(this->boot_shown_, static_cast<uint8_t>(std::min(mapped, 100)));
  const int target = BOOT_RING_POINTS * static_cast<int>(this->boot_shown_) / 100;
  const int before = static_cast<int>(this->boot_step_);
  const int head = before == 0 ? target : std::min<int>(target, before + BOOT_ADVANCE);
  if (head == before)
    return;
  this->boot_step_ = static_cast<uint32_t>(head);
  if (before != 0) {
    const float angle = static_cast<float>(before) * 2.0f * static_cast<float>(M_PI) / BOOT_RING_POINTS;
    const int reach = static_cast<int>(ceilf(BOOT_HEAD_RADIUS + 1.0f));
    this->boot_rect_(this->boot_cx_ + static_cast<int>(sinf(angle) * BOOT_RING_RADIUS) - reach - 1,
                     this->boot_cy_ - static_cast<int>(cosf(angle) * BOOT_RING_RADIUS) - reach - 1, 2 * reach + 3, 2 * reach + 3,
                     this->boot_background_);
  }
  this->boot_ring_paint_(head);
  this->boot_head_paint_(head, accent);
  this->boot_sync_(this->boot_cy_ - BOOT_RING_RADIUS - BOOT_HEAD_DOT, 2 * (BOOT_RING_RADIUS + BOOT_HEAD_DOT));
}

void MipiDsi::boot_stop_() {
  esp_timer_stop(this->boot_timer_);
  esp_timer_delete(this->boot_timer_);
  this->boot_timer_ = nullptr;
}
#endif

void MipiDsi::present_dsi_frame_buffer(void *fb) {
  esp_lcd_panel_draw_bitmap(this->handle_, 0, 0, this->width_, this->height_, fb);
  xSemaphoreTake(this->io_lock_, 0);
  this->draw_fb_ = fb;
}

uint8_t MipiDsi::get_scanout_buffers(void **out, uint8_t max, display::ColorBitness *bitness) {
  if (this->dsi_fbs_[0] == nullptr)
    return 0;
  const uint8_t count = std::min<uint8_t>(std::min<uint8_t>(this->num_fbs_, 3), max);
  for (uint8_t i = 0; i < count; i++)
    out[i] = this->dsi_fbs_[i];
  *bitness = this->color_depth_;
  return count;
}

void MipiDsi::present_scanout_buffer(void *buffer) {
#ifdef USE_MIPI_DSI_BOOT_IMAGE
  if (this->boot_timer_ != nullptr && buffer != this->draw_fb_)
    this->boot_stop_();
#endif
  esp_lcd_panel_draw_bitmap(this->handle_, 0, 0, 1, 1, buffer);
  xSemaphoreTake(this->io_lock_, 0);
  this->draw_fb_ = buffer;
  this->flip_frame_.store(this->scan_frames_.load(std::memory_order_acquire) + 1, std::memory_order_release);
}

uint8_t *MipiDsi::get_draw_buffer(display::ColorBitness *bitness) {
  *bitness = this->color_depth_;
  return static_cast<uint8_t *>(this->draw_fb_);
}

bool MipiDsi::scanout_flipped() {
  return static_cast<int32_t>(this->scan_frames_.load(std::memory_order_acquire) -
                              this->flip_frame_.load(std::memory_order_acquire)) >= 0;
}

bool MipiDsi::wait_scanout_flipped(uint32_t timeout_ms) {
  if (this->scanout_flipped() || this->flip_sem_ == nullptr)
    return this->scanout_flipped();
  xSemaphoreTake(this->flip_sem_, 0);
  this->flip_waiting_.store(true, std::memory_order_release);
  if (!this->scanout_flipped())
    xSemaphoreTake(this->flip_sem_, pdMS_TO_TICKS(timeout_ms));
  this->flip_waiting_.store(false, std::memory_order_release);
  return this->scanout_flipped();
}

void MipiDsi::update() {
  if (this->auto_clear_enabled_) {
    this->clear();
  }
  this->updating_ = true;
  if (this->show_test_card_) {
    this->test_card();
  } else if (this->page_ != nullptr) {
    this->page_->get_writer()(*this);
  } else if (this->writer_.has_value()) {
    (*this->writer_)(*this);
  } else {
    this->stop_poller();
  }
  this->updating_ = false;
  this->flush_dirty_();
}

void MipiDsi::flush_dirty_() {
  // Inside update() the one flush at its end sends everything
  if (this->updating_ || this->buffer_ == nullptr || this->x_low_ > this->x_high_ || this->y_low_ > this->y_high_)
    return;
  ESP_LOGV(TAG, "x_low %d, y_low %d, x_high %d, y_high %d", this->x_low_, this->y_low_, this->x_high_, this->y_high_);
  int w = this->x_high_ - this->x_low_ + 1;
  int h = this->y_high_ - this->y_low_ + 1;
  this->write_to_display_(this->x_low_, this->y_low_, w, h, this->buffer_, this->x_low_, this->y_low_,
                          this->width_ - w - this->x_low_);
  // invalidate watermarks
  this->x_low_ = this->width_;
  this->y_low_ = this->height_;
  this->x_high_ = 0;
  this->y_high_ = 0;
}

void MipiDsi::draw_pixels_internal(int x_start, int y_start, int w, int h, const uint8_t *ptr,
                                   display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset,
                                   int x_pad) {
  if (w <= 0 || h <= 0)
    return;
  if (bitness != this->color_depth_ || (bitness == display::COLOR_BITNESS_565 && big_endian != this->big_endian_) ||
      this->rotation_ != display::DISPLAY_ROTATION_0_DEGREES || this->is_clipping()) {
    display::Display::draw_pixels_internal(x_start, y_start, w, h, ptr, bitness, big_endian, x_offset, y_offset, x_pad);
    this->mark_dirty_(x_start, y_start, w, h);
    this->flush_dirty_();
    return;
  }
  if (this->buffer_ != nullptr) {
    // With a buffer the block goes through it, or a flush of the old contents would paint over it
    const int first_col = std::max(0, -x_start), end_col = std::min(w, static_cast<int>(this->width_) - x_start);
    const int first_row = std::max(0, -y_start), end_row = std::min(h, static_cast<int>(this->height_) - y_start);
    if (first_col >= end_col || first_row >= end_row)
      return;
    const size_t bytes_per_pixel = 3 - this->color_depth_;
    const size_t stride = x_offset + w + x_pad;
    for (int y = first_row; y != end_row; y++) {
      memcpy(this->buffer_ + ((y_start + y) * this->width_ + x_start + first_col) * bytes_per_pixel,
             ptr + ((y_offset + y) * stride + x_offset + first_col) * bytes_per_pixel,
             (end_col - first_col) * bytes_per_pixel);
    }
    if (this->updating_) {
      this->mark_dirty_(x_start + first_col, y_start + first_row, end_col - first_col, end_row - first_row);
    } else {
      this->write_to_display_(x_start + first_col, y_start + first_row, end_col - first_col, end_row - first_row, ptr,
                              x_offset + first_col, y_offset + first_row, x_pad + w - end_col);
    }
    return;
  }
  this->write_to_display_(x_start, y_start, w, h, ptr, x_offset, y_offset, x_pad);
}

void MipiDsi::write_to_display_(int x_start, int y_start, int w, int h, const uint8_t *ptr, int x_offset, int y_offset,
                                int x_pad) {
#ifdef USE_MIPI_DSI_BOOT_IMAGE
  if (this->boot_timer_ != nullptr)
    this->boot_stop_();
#endif
  esp_err_t err = ESP_OK;
  auto bytes_per_pixel = 3 - this->color_depth_;
  auto stride = (x_offset + w + x_pad) * bytes_per_pixel;
  ptr += y_offset * stride + x_offset * bytes_per_pixel;  // skip to the first pixel
  // x_ and y_offset are offsets into the source buffer, unrelated to our own offsets into the display.
  if (x_offset == 0 && x_pad == 0) {
    err = esp_lcd_panel_draw_bitmap(this->handle_, x_start, y_start, x_start + w, y_start + h, ptr);
    xSemaphoreTake(this->io_lock_, portMAX_DELAY);

  } else {
    // draw line by line
    for (int y = 0; y != h; y++) {
      err = esp_lcd_panel_draw_bitmap(this->handle_, x_start, y + y_start, x_start + w, y + y_start + 1, ptr);
      if (err != ESP_OK)
        break;
      ptr += stride;  // next line
      xSemaphoreTake(this->io_lock_, portMAX_DELAY);
    }
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "lcd_lcd_panel_draw_bitmap failed: %s", esp_err_to_name(err));
  }
}

bool MipiDsi::check_buffer_() {
  if (this->is_failed())
    return false;
  if (this->buffer_ != nullptr)
    return true;
  // this is dependent on the enum values.
  auto bytes_per_pixel = 3 - this->color_depth_;
  RAMAllocator<uint8_t> allocator;
  this->buffer_ = allocator.allocate(this->height_ * this->width_ * bytes_per_pixel);
  if (this->buffer_ == nullptr) {
    this->mark_failed(LOG_STR("Could not allocate buffer for display!"));
    return false;
  }
  return true;
}

// Widen the dirty box to the drawn rectangle, even where the buffer already held the same values.
void MipiDsi::mark_dirty_(int x_start, int y_start, int w, int h) {
  int x1 = x_start, y1 = y_start;
  int x2 = x_start + w - 1, y2 = y_start + h - 1;
  this->rotate_(x1, y1);
  this->rotate_(x2, y2);
  const int right = static_cast<int>(this->width_) - 1;
  const int bottom = static_cast<int>(this->height_) - 1;
  const int left = std::clamp(std::min(x1, x2), 0, right);
  const int top = std::clamp(std::min(y1, y2), 0, bottom);
  x2 = std::clamp(std::max(x1, x2), 0, right);
  y2 = std::clamp(std::max(y1, y2), 0, bottom);
  // An empty box is low above high, so it is replaced rather than widened
  if (this->x_low_ > this->x_high_ || this->y_low_ > this->y_high_) {
    this->x_low_ = left;
    this->x_high_ = x2;
    this->y_low_ = top;
    this->y_high_ = y2;
    return;
  }
  this->x_low_ = std::min<int>(this->x_low_, left);
  this->x_high_ = std::max<int>(this->x_high_, x2);
  this->y_low_ = std::min<int>(this->y_low_, top);
  this->y_high_ = std::max<int>(this->y_high_, y2);
}

void MipiDsi::draw_pixel_at(int x, int y, Color color) {
  if (this->is_point_clipped(x, y))
    return;

  this->rotate_(x, y);
  if (x >= this->get_width_internal() || x < 0 || y >= this->get_height_internal() || y < 0) {
    return;
  }
  if (!this->check_buffer_())
    return;
  size_t pos = (y * this->width_) + x;
  switch (this->color_depth_) {
    case display::COLOR_BITNESS_565: {
      auto *ptr_16 = reinterpret_cast<uint16_t *>(this->buffer_);
      const uint16_t new_color = this->to_panel_565_(color);
      if (ptr_16[pos] == new_color)
        return;
      ptr_16[pos] = new_color;
      break;
    }
    case display::COLOR_BITNESS_888:
      // The panel applies the colour order, so the buffer keeps the standard b,g,r layout
      this->buffer_[pos * 3] = color.b;
      this->buffer_[pos * 3 + 1] = color.g;
      this->buffer_[pos * 3 + 2] = color.r;
      break;
    case display::COLOR_BITNESS_332:
      break;
  }
  // low and high watermark may speed up drawing from buffer
  if (x < this->x_low_)
    this->x_low_ = x;
  if (y < this->y_low_)
    this->y_low_ = y;
  if (x > this->x_high_)
    this->x_high_ = x;
  if (y > this->y_high_)
    this->y_high_ = y;
}
void MipiDsi::fill(Color color) {
  if (!this->check_buffer_())
    return;

  // If clipping is active, fall back to base implementation
  if (this->get_clipping().is_set()) {
    Display::fill(color);
    return;
  }

  switch (this->color_depth_) {
    case display::COLOR_BITNESS_565: {
      auto *ptr_16 = reinterpret_cast<uint16_t *>(this->buffer_);
      const uint16_t new_color = this->to_panel_565_(color);
      std::fill_n(ptr_16, this->width_ * this->height_, new_color);
      break;
    }

    case display::COLOR_BITNESS_888:
      for (size_t i = 0; i != this->width_ * this->height_; i++) {
        this->buffer_[i * 3 + 0] = color.b;
        this->buffer_[i * 3 + 1] = color.g;
        this->buffer_[i * 3 + 2] = color.r;
      }
      break;

    default:
      break;
  }
  this->mark_dirty_(0, 0, this->get_width(), this->get_height());
}

int MipiDsi::get_width() {
  switch (this->rotation_) {
    case display::DISPLAY_ROTATION_90_DEGREES:
    case display::DISPLAY_ROTATION_270_DEGREES:
      return this->get_height_internal();
    case display::DISPLAY_ROTATION_0_DEGREES:
    case display::DISPLAY_ROTATION_180_DEGREES:
    default:
      return this->get_width_internal();
  }
}

int MipiDsi::get_height() {
  switch (this->rotation_) {
    case display::DISPLAY_ROTATION_0_DEGREES:
    case display::DISPLAY_ROTATION_180_DEGREES:
      return this->get_height_internal();
    case display::DISPLAY_ROTATION_90_DEGREES:
    case display::DISPLAY_ROTATION_270_DEGREES:
    default:
      return this->get_width_internal();
  }
}

bool MipiDsi::send_init_sequence_(uint32_t when) {
  esp_err_t err;
  size_t index = 0;
  const uint8_t *seq = this->init_sequence_;
  const size_t len = this->init_sequence_len_;
  while (index != len) {
    if (len - index < 2) {
      this->mark_failed(LOG_STR("Malformed init sequence"));
      return false;
    }
    uint8_t cmd = seq[index++];
    uint8_t x = seq[index++];
    if (x == DELAY_FLAG) {
      ESP_LOGD(TAG, "Delay %dms", cmd);
      delay(cmd);
#ifdef USE_I2C
    } else if (x == I2C_FLAG) {
      if (len - index < 2) {
        this->mark_failed(LOG_STR("Malformed init sequence"));
        return false;
      }
      this->i2c_device_.set_i2c_address(cmd);
      if (!this->i2c_device_.write_registers(reinterpret_cast<const i2c::I2CRegValue *>(seq + index), 1)) {
        this->mark_failed(LOG_STR("I2C write failed"));
        return false;
      }
      index += 2;
#endif
    } else {
      uint8_t num_args = x & 0x7F;
      if (len - index < num_args) {
        this->mark_failed(LOG_STR("Malformed init sequence"));
        return false;
      }
      if (cmd == SLEEP_OUT && !this->generic_commands_) {
        // are we ready, boots?
        int duration = when - millis();
        if (duration > 0) {
          delay(duration);
        }
      }
      const auto *ptr = seq + index;
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
      char hex_buf[format_hex_pretty_size(MIPI_DSI_MAX_CMD_LOG_BYTES)];
#endif
      ESP_LOGVV(TAG, "Command %02X, length %d, byte(s) %s", cmd, num_args,
                format_hex_pretty_to(hex_buf, ptr, num_args, '.'));
      index += num_args;
      if (this->generic_commands_) {
        this->write_generic_(cmd, ptr, num_args);
        continue;
      }
      err = esp_lcd_panel_io_tx_param(this->io_handle_, cmd, ptr, num_args);
      if (err != ESP_OK) {
        this->smark_failed(LOG_STR("lcd_panel_io_tx_param failed"), err);
        return false;
      }
      if (cmd == SLEEP_OUT)
        delay(10);
    }
  }
  return true;
}

void MipiDsi::write_generic_(uint8_t cmd, const uint8_t *args, uint8_t num_args) {
  mipi_dsi_hal_context_t hal{};
  hal.host = MIPI_DSI_LL_GET_HOST(0);
  if (num_args == 0) {
    mipi_dsi_hal_host_gen_write_short_packet(&hal, 0, MIPI_DSI_DT_GENERIC_SHORT_WRITE_1, cmd);
  } else if (num_args == 1) {
    mipi_dsi_hal_host_gen_write_short_packet(&hal, 0, MIPI_DSI_DT_GENERIC_SHORT_WRITE_2, cmd | (args[0] << 8));
  } else {
    uint8_t payload[128];
    payload[0] = cmd;
    memcpy(payload + 1, args, num_args);
    mipi_dsi_hal_host_gen_write_long_packet(&hal, 0, MIPI_DSI_DT_GENERIC_LONG_WRITE, payload, num_args + 1);
  }
}

void MipiDsi::log_read_registers_() {
  auto *host = MIPI_DSI_LL_GET_HOST(0);
  mipi_dsi_hal_context_t hal{};
  hal.host = host;
  for (uint8_t i = 0; i < this->read_registers_count_; i++) {
    const uint16_t reg = this->read_registers_[i];
    mipi_dsi_hal_host_gen_write_short_packet(&hal, 0, MIPI_DSI_DT_SET_MAXIMUM_RETURN_PKT, 4);
    mipi_dsi_host_ll_enable_bta(host, true);
    mipi_dsi_host_ll_gen_set_rx_vcid(host, 0);
    mipi_dsi_hal_host_gen_write_short_packet(&hal, 0, MIPI_DSI_DT_GENERIC_READ_REQUEST_2, reg);
    uint8_t waited = 0;
    while ((mipi_dsi_host_ll_gen_is_read_cmd_busy(host) || mipi_dsi_host_ll_gen_is_read_fifo_empty(host)) &&
           waited < 20) {
      delay(1);
      waited++;
    }
    if (mipi_dsi_host_ll_gen_is_read_fifo_empty(host)) {
      ESP_LOGW(TAG, "Register 0x%04X: no answer", reg);
      continue;
    }
    uint32_t value = mipi_dsi_host_ll_gen_read_payload_fifo(host);
    while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(host))
      mipi_dsi_host_ll_gen_read_payload_fifo(host);
    ESP_LOGI(TAG, "Register 0x%04X = 0x%08" PRIX32, reg, value);
  }
}

static const uint8_t PIXEL_MODES[] = {0, 16, 18, 24};
static const char *const VIDEO_MODES[] = {"non-burst, sync pulses", "non-burst, sync events", "burst"};

void MipiDsi::dump_config() {
  ESP_LOGCONFIG(TAG,
                "MIPI_DSI RGB LCD"
                "\n  Model: %s"
                "\n  Width: %u"
                "\n  Height: %u"
                "\n  Rotation: %d degrees"
                "\n  DSI Lanes: %u"
                "\n  Lane Bit Rate: %.0fMbps"
                "\n  HSync Pulse Width: %u"
                "\n  HSync Back Porch: %u"
                "\n  HSync Front Porch: %u"
                "\n  VSync Pulse Width: %u"
                "\n  VSync Back Porch: %u"
                "\n  VSync Front Porch: %u"
                "\n  Buffer Color Depth: %d bit"
                "\n  Display Pixel Mode: %d bit"
                "\n  Invert Colors: %s"
                "\n  Pixel Clock: %.1fMHz"
                "\n  Video Mode: %s"
                "\n  Frame Ack: %s"
                "\n  Continuous Clock: %s"
                "\n  EoTp: %s"
                "\n  LP in horizontal blanking: %s"
                "\n  LP in active lines: %s"
                "\n  Commands: %s",
                this->model_, this->width_, this->height_, this->rotation_, this->lanes_, this->lane_bit_rate_,
                this->hsync_pulse_width_, this->hsync_back_porch_, this->hsync_front_porch_, this->vsync_pulse_width_,
                this->vsync_back_porch_, this->vsync_front_porch_, (3 - this->color_depth_) * 8, this->pixel_mode_,
                YESNO(this->invert_colors_), this->pclk_frequency_, VIDEO_MODES[this->video_mode_],
                YESNO(this->frame_ack_), YESNO(this->continuous_clock_), YESNO(this->eotp_),
                YESNO(this->lp_horizontal_blanking_), YESNO(this->lp_vertical_active_),
                this->generic_commands_ ? "generic" : "DCS");
  LOG_PIN("  Reset Pin ", this->reset_pin_);
}
}  // namespace esphome::mipi_dsi
#endif  // USE_ESP32_VARIANT_ESP32P4
