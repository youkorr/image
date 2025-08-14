#pragma once

#include "esphome/core/color.h"
#include "esphome/components/display/display.h"
#include "esphome/core/helpers.h"
#include <vector>
#include <string>
#include <algorithm>

#ifdef USE_LVGL
#include "esphome/components/lvgl/lvgl_proxy.h"
#endif

// Déclaration forward pour éviter l'inclusion circulaire
namespace esphome {
namespace sd_mmc_card {
class SdMmcCardComponent;
}
}

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
  
  Color get_pixel(int x, int y, Color color_on = display::COLOR_ON, Color color_off = display::COLOR_OFF) const;
  
  int get_width() const override;
  int get_height() const override;
  const uint8_t *get_data_start() const { return this->data_start_; }
  ImageType get_type() const;
  int get_bpp() const { return this->bpp_; }
  
  size_t get_width_stride() const { return (this->width_ * this->get_bpp() + 7u) / 8u; }
  
  void draw(int x, int y, display::Display *display, Color color_on, Color color_off) override;
  
  bool has_transparency() const { return this->transparency_ != TRANSPARENCY_OPAQUE; }
  
  // Méthodes pour les images SD
  void set_sd_path(const std::string &path) { this->sd_path_ = path; }
  void set_sd_runtime(bool enabled) { this->sd_runtime_ = enabled; }
  void set_sd_card_component(esphome::sd_mmc_card::SdMmcCardComponent *sd_card) { this->sd_card_component_ = sd_card; }
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
  
  // Méthodes privées pour le décodage d'images
  bool decode_image_from_sd();
  bool decode_jpeg_data(const std::vector<uint8_t> &jpeg_data);
  bool decode_png_data(const std::vector<uint8_t> &png_data);
  bool read_sd_file(const std::string &path, std::vector<uint8_t> &data);
  size_t get_expected_buffer_size() const;
  
  // Propriétés
  int width_;
  int height_;
  ImageType type_;
  const uint8_t *data_start_;
  Transparency transparency_;
  size_t bpp_{};
  
  // Support SD
  std::string sd_path_{};
  bool sd_runtime_{false};
  std::vector<uint8_t> sd_buffer_;
  esphome::sd_mmc_card::SdMmcCardComponent *sd_card_component_{nullptr};

#ifdef USE_LVGL
  lv_img_dsc_t dsc_{};
#endif
};

}  // namespace image
}  // namespace esphome
