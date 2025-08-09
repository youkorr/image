#pragma once

#include "esphome/core/component.h"
#include "esphome/core/color.h"
#include "esphome/components/display/display.h"
#include "../sd_mmc_card/sd_mmc_card.h"
#include <vector>
#include <string>
#include <algorithm>

#ifdef USE_LVGL
#include "esphome/components/lvgl/lvgl_proxy.h"
#endif  // USE_LVGL

namespace esphome {
namespace image {

enum ImageType {
  IMAGE_TYPE_BINARY = 0,
  IMAGE_TYPE_GRAYSCALE = 1,
  IMAGE_TYPE_RGB = 2,
  IMAGE_TYPE_RGB565 = 3,
};

enum Transparency {
  TRANSPARENCY_OPAQUE = 0,
  TRANSPARENCY_CHROMA_KEY = 1,
  TRANSPARENCY_ALPHA_CHANNEL = 2,
};

class Image : public display::BaseImage, public Component {
 public:
  Image(const uint8_t *data_start, int width, int height, ImageType type, Transparency transparency)
      : data_start_(data_start), width_(width), height_(height), type_(type), transparency_(transparency) {
    switch (type) {
      case IMAGE_TYPE_BINARY:   bpp_ = 1;  break;
      case IMAGE_TYPE_GRAYSCALE:bpp_ = 8;  break;
      case IMAGE_TYPE_RGB:      bpp_ = 24; break;
      case IMAGE_TYPE_RGB565:   bpp_ = 16; break;
    }
    stride_ = (width_ * bpp_ + 7u) / 8u;
  }

  Color get_pixel(int x, int y, Color color_on = display::COLOR_ON, Color color_off = display::COLOR_OFF) const;

  int get_width() const override;
  int get_height() const override;
  const uint8_t *get_data_start() const { return this->data_start_; }
  ImageType get_type() const;
  int get_bpp() const { return this->bpp_; }

  size_t get_width_stride() const { return stride_; }

  void draw(int x, int y, display::Display *display, Color color_on, Color color_off) override;

  bool has_transparency() const { return this->transparency_ != TRANSPARENCY_OPAQUE; }

  // Gestion SD
  void set_sd_path(const std::string &path) { this->sd_path_ = path; }
  void set_sd_runtime(bool enabled) { this->sd_runtime_ = enabled; }
  bool load_from_sd();

#ifdef USE_LVGL
  lv_img_dsc_t *get_lv_img_dsc();
#endif

 protected:
  bool get_binary_pixel_(int x, int y) const;
  Color get_rgb_pixel_(int x, int y) const;
  Color get_rgb565_pixel_(int x, int y) const;
  Color get_grayscale_pixel_(int x, int y) const;

  uint8_t get_data_byte_(size_t pos) const;

  bool decode_jpeg_from_sd();
  size_t get_expected_buffer_size() const;

  int width_;
  int height_;
  ImageType type_;
  const uint8_t *data_start_;  // Données en mémoire ou nullptr si depuis SD
  Transparency transparency_;
  size_t bpp_{};
  size_t stride_{};

  std::vector<uint8_t> sd_buffer_;
  std::string sd_path_;
  bool sd_runtime_ = false;

#ifdef USE_LVGL
  lv_img_dsc_t dsc_{};
#endif
};

}  // namespace image
}  // namespace esphome
