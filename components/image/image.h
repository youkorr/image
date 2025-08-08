#pragma once
#include "esphome/core/color.h"
#include "esphome/components/display/display.h"
#include "../sd_mmc_card/sd_mmc_card.h"
#include <vector>

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

class Image : public display::BaseImage {
 public:
  Image(const uint8_t *data_start, int width, int height, ImageType type, Transparency transparency);

  Color get_pixel(int x, int y, Color color_on = display::COLOR_ON, Color color_off = display::COLOR_OFF) const override;
  int get_width() const;
  int get_height() const;
  ImageType get_type() const;
  
  const uint8_t *get_data_start() const { return this->data_start_; }
  

  int get_bpp() const { return this->bpp_; }

  size_t get_width_stride() const { return (this->width_ * this->get_bpp() + 7u) / 8u; }
  void draw(int x, int y, display::Display *display, Color color_on, Color color_off) override;

  bool has_transparency() const { return this->transparency_ != TRANSPARENCY_OPAQUE; }

  void set_sd_path(const std::string &path) { this->sd_path_ = path; }
  void set_sd_runtime(bool enabled) { this->sd_runtime_ = enabled; }
  bool load_from_sd();  // nouvelle méthode

#ifdef USE_LVGL
  lv_img_dsc_t *get_lv_img_dsc();
#endif

 protected:
  bool get_binary_pixel_(int x, int y) const;
  Color get_rgb_pixel_(int x, int y) const;
  Color get_rgb565_pixel_(int x, int y) const;
  Color get_grayscale_pixel_(int x, int y) const;

  int width_{0};
  int height_{0};
  ImageType type_{IMAGE_TYPE_BINARY};
  const uint8_t *data_start_{nullptr};
  Transparency transparency_{TRANSPARENCY_OPAQUE};
  size_t bpp_{};
  size_t stride_{};

  // Ajout pour lecture SD
  std::string sd_path_{};
  bool sd_runtime_{false};
  std::vector<uint8_t> sd_buffer_;  // stockage image chargée

#ifdef USE_LVGL
  lv_img_dsc_t dsc_{};
#endif
};

}  // namespace image
}  // namespace esphome
