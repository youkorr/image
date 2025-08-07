#pragma once
#include "esphome/core/color.h"
#include "esphome/components/display/display.h"

#ifdef USE_SD_MMC_CARD
#include "../sd_mmc_card/sd_mmc_card.h"
#endif

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

enum TransparencyType {  // Renommé pour correspondre au Python
  TRANSPARENCY_OPAQUE = 0,
  TRANSPARENCY_CHROMA_KEY = 1,
  TRANSPARENCY_ALPHA_CHANNEL = 2,
};

class Image : public display::BaseImage {
 public:
  Image(const uint8_t *data_start, int width, int height, ImageType type, TransparencyType transparency);
  Color get_pixel(int x, int y, Color color_on = display::COLOR_ON, Color color_off = display::COLOR_OFF) const;
  int get_width() const override;
  int get_height() const override;
  const uint8_t *get_data_start() const { return this->data_start_; }
  ImageType get_type() const;

  int get_bpp() const { return this->bpp_; }

  /// Return the stride of the image in bytes, that is, the distance in bytes
  /// between two consecutive rows of pixels.
  size_t get_width_stride() const { return (this->width_ * this->get_bpp() + 7u) / 8u; }
  void draw(int x, int y, display::Display *display, Color color_on, Color color_off) override;

  bool has_transparency() const { return this->transparency_ != TRANSPARENCY_OPAQUE; }

#ifdef USE_LVGL
  lv_img_dsc_t *get_lv_img_dsc();
#endif

 protected:
  bool get_binary_pixel_(int x, int y) const;
  Color get_rgb_pixel_(int x, int y) const;
  Color get_rgb565_pixel_(int x, int y) const;
  Color get_grayscale_pixel_(int x, int y) const;

  int width_;
  int height_;
  ImageType type_;
  const uint8_t *data_start_;
  TransparencyType transparency_;
  size_t bpp_{};
  size_t stride_{};
#ifdef USE_LVGL
  lv_img_dsc_t dsc_{};
#endif
};

#ifdef USE_SD_MMC_CARD
class SDCardImage : public Image {
 public:
  SDCardImage(const std::string &path, ImageType type, TransparencyType transparency);
  ~SDCardImage();

  // Configuration methods (appelées depuis Python)
  void set_resize(int width, int height) { 
    this->resize_width_ = width; 
    this->resize_height_ = height; 
  }
  void set_dither(bool dither) { this->dither_ = dither; }
  void set_invert_alpha(bool invert) { this->invert_alpha_ = invert; }
  void set_big_endian(bool big_endian) { this->big_endian_ = big_endian; }

  // Override des méthodes de base
  void draw(int x, int y, display::Display *display, Color color_on, Color color_off) override;
  Color get_pixel(int x, int y, Color color_on = display::COLOR_ON, Color color_off = display::COLOR_OFF) const override;

 protected:
  bool load_image_();
  void process_image_data_();
  void cleanup_();

  std::string path_;
  uint8_t *image_data_;
  bool loaded_;
  
  // Options de configuration
  int resize_width_{0};
  int resize_height_{0};
  bool dither_{false};
  bool invert_alpha_{false};
  bool big_endian_{true};
  
  sd_mmc_card::SDMMCCardComponent *sd_card_{nullptr};
};
#endif

}  // namespace image
}  // namespace esphome
