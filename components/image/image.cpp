#include "image.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "../sd_mmc_card/sd_mmc_card.h"

namespace esphome {
namespace image {

static const char *const TAG = "image";

void Image::draw(int x, int y, display::Display *display, Color color_on, Color color_off) {
  // Charge l'image depuis la SD si nécessaire
  if (sd_runtime_ && sd_buffer_.empty() && !sd_path_.empty()) {
    if (!load_from_sd()) {
      ESP_LOGE(TAG, "Failed to load SD image: %s", sd_path_.c_str());
      return;
    }
  }
  
  int img_x0 = 0;
  int img_y0 = 0;
  int w = width_;
  int h = height_;

  auto clipping = display->get_clipping();
  if (clipping.is_set()) {
    if (clipping.x > x)
      img_x0 += clipping.x - x;
    if (clipping.y > y)
      img_y0 += clipping.y - y;
    if (w > clipping.x2() - x)
      w = clipping.x2() - x;
    if (h > clipping.y2() - y)
      h = clipping.y2() - y;
  }

  switch (type_) {
    case IMAGE_TYPE_BINARY: {
      for (int img_x = img_x0; img_x < w; img_x++) {
        for (int img_y = img_y0; img_y < h; img_y++) {
          if (this->get_binary_pixel_(img_x, img_y)) {
            display->draw_pixel_at(x + img_x, y + img_y, color_on);
          } else if (!this->transparency_) {
            display->draw_pixel_at(x + img_x, y + img_y, color_off);
          }
        }
      }
      break;
    }
    case IMAGE_TYPE_GRAYSCALE:
      for (int img_x = img_x0; img_x < w; img_x++) {
        for (int img_y = img_y0; img_y < h; img_y++) {
          const uint32_t pos = (img_x + img_y * this->width_);
          const uint8_t gray = this->get_data_byte_(pos);
          Color color = Color(gray, gray, gray, 0xFF);
          switch (this->transparency_) {
            case TRANSPARENCY_CHROMA_KEY:
              if (gray == 1) {
                continue;
              }
              break;
            case TRANSPARENCY_ALPHA_CHANNEL: {
              auto on = (float) gray / 255.0f;
              auto off = 1.0f - on;
              color = Color(color_on.r * on + color_off.r * off, color_on.g * on + color_off.g * off,
                            color_on.b * on + color_off.b * off, 0xFF);
              break;
            }
            default:
              break;
          }
          display->draw_pixel_at(x + img_x, y + img_y, color);
        }
      }
      break;
    case IMAGE_TYPE_RGB565:
      for (int img_x = img_x0; img_x < w; img_x++) {
        for (int img_y = img_y0; img_y < h; img_y++) {
          auto color = this->get_rgb565_pixel_(img_x, img_y);
          if (color.w >= 0x80) {
            display->draw_pixel_at(x + img_x, y + img_y, color);
          }
        }
      }
      break;
    case IMAGE_TYPE_RGB:
      for (int img_x = img_x0; img_x < w; img_x++) {
        for (int img_y = img_y0; img_y < h; img_y++) {
          auto color = this->get_rgb_pixel_(img_x, img_y);
          if (color.w >= 0x80) {
            display->draw_pixel_at(x + img_x, y + img_y, color);
          }
        }
      }
      break;
  }
}

Color Image::get_pixel(int x, int y, const Color color_on, const Color color_off) const {
  if (x < 0 || x >= this->width_ || y < 0 || y >= this->height_)
    return color_off;
  switch (this->type_) {
    case IMAGE_TYPE_BINARY:
      if (this->get_binary_pixel_(x, y))
        return color_on;
      return color_off;
    case IMAGE_TYPE_GRAYSCALE:
      return this->get_grayscale_pixel_(x, y);
    case IMAGE_TYPE_RGB565:
      return this->get_rgb565_pixel_(x, y);
    case IMAGE_TYPE_RGB:
      return this->get_rgb_pixel_(x, y);
    default:
      return color_off;
  }
}

uint8_t Image::get_data_byte_(size_t pos) const {
  if (!sd_buffer_.empty()) {
    if (pos < sd_buffer_.size()) {
      return sd_buffer_[pos];
    }
    return 0;
  } else {
    return progmem_read_byte(this->data_start_ + pos);
  }
}

bool Image::load_from_sd() {
  if (sd_path_.empty()) {
    ESP_LOGE(TAG, "SD path is empty");
    return false;
  }

  if (!sd_card_component_ || !sd_card_component_->is_ready()) {
    ESP_LOGE(TAG, "SD card component not available or not ready");
    return false;
  }

  ESP_LOGI(TAG, "Loading image from SD: %s", sd_path_.c_str());
  
  return decode_image_from_sd();
}

bool Image::read_sd_file(const std::string &path, std::vector<uint8_t> &data) {
  FILE *file = fopen(path.c_str(), "rb");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open file: %s", path.c_str());
    return false;
  }

  // Obtient la taille du fichier
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (file_size <= 0) {
    ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
    fclose(file);
    return false;
  }

  // Lit le fichier
  data.resize(file_size);
  size_t read_size = fread(data.data(), 1, file_size, file);
  fclose(file);

  if (read_size != static_cast<size_t>(file_size)) {
    ESP_LOGE(TAG, "Failed to read complete file. Expected: %ld, Read: %zu", file_size, read_size);
    return false;
  }

  ESP_LOGI(TAG, "Successfully read %zu bytes from %s", data.size(), path.c_str());
  return true;
}

bool Image::decode_image_from_sd() {
  std::vector<uint8_t> file_data;
  if (!read_sd_file(sd_path_, file_data)) {
    return false;
  }

  // Détecte le format d'image
  if (file_data.size() >= 4) {
    // JPEG: FF D8 FF
    if (file_data[0] == 0xFF && file_data[1] == 0xD8 && file_data[2] == 0xFF) {
      ESP_LOGI(TAG, "Detected JPEG format");
      return decode_jpeg_data(file_data);
    }
    // PNG: 89 50 4E 47
    else if (file_data[0] == 0x89 && file_data[1] == 0x50 && 
             file_data[2] == 0x4E && file_data[3] == 0x47) {
      ESP_LOGI(TAG, "Detected PNG format");
      return decode_png_data(file_data);
    }
  }

  ESP_LOGE(TAG, "Unsupported image format or corrupted file");
  return false;
}

// Décodeur JPEG simple (vous pouvez utiliser une bibliothèque comme TJpgDec)
bool Image::decode_jpeg_data(const std::vector<uint8_t> &jpeg_data) {
  ESP_LOGI(TAG, "Decoding JPEG data (%zu bytes)", jpeg_data.size());
  
  // TODO: Intégrez ici une bibliothèque de décodage JPEG comme TJpgDec
  // Pour l'instant, on crée une image de test
  
  size_t expected_size = get_expected_buffer_size();
  sd_buffer_.resize(expected_size);
  
  // Image de test (carré rouge)
  switch (type_) {
    case IMAGE_TYPE_RGB:
      for (size_t i = 0; i < expected_size; i += 3) {
        if (i + 2 < expected_size) {
          sd_buffer_[i] = 255;     // R
          sd_buffer_[i + 1] = 0;   // G  
          sd_buffer_[i + 2] = 0;   // B
        }
      }
      break;
    case IMAGE_TYPE_RGB565:
      for (size_t i = 0; i < expected_size; i += 2) {
        if (i + 1 < expected_size) {
          uint16_t red565 = 0xF800; // Rouge en RGB565
          sd_buffer_[i] = red565 >> 8;
          sd_buffer_[i + 1] = red565 & 0xFF;
        }
      }
      break;
    case IMAGE_TYPE_GRAYSCALE:
      std::fill(sd_buffer_.begin(), sd_buffer_.end(), 128); // Gris moyen
      break;
    case IMAGE_TYPE_BINARY:
      std::fill(sd_buffer_.begin(), sd_buffer_.end(), 0xFF); // Blanc
      break;
  }
  
  ESP_LOGI(TAG, "JPEG decode completed (test pattern generated)");
  return true;
}

bool Image::decode_png_data(const std::vector<uint8_t> &png_data) {
  ESP_LOGI(TAG, "Decoding PNG data (%zu bytes)", png_data.size());
  
  // TODO: Intégrez ici une bibliothèque de décodage PNG
  // Pour l'instant, on crée une image de test
  
  size_t expected_size = get_expected_buffer_size();
  sd_buffer_.resize(expected_size);
  
  // Image de test (carré bleu)
  switch (type_) {
    case IMAGE_TYPE_RGB:
      for (size_t i = 0; i < expected_size; i += 3) {
        if (i + 2 < expected_size) {
          sd_buffer_[i] = 0;       // R
          sd_buffer_[i + 1] = 0;   // G
          sd_buffer_[i + 2] = 255; // B
        }
      }
      break;
    case IMAGE_TYPE_RGB565:
      for (size_t i = 0; i < expected_size; i += 2) {
        if (i + 1 < expected_size) {
          uint16_t blue565 = 0x001F; // Bleu en RGB565
          sd_buffer_[i] = blue565 >> 8;
          sd_buffer_[i + 1] = blue565 & 0xFF;
        }
      }
      break;
    case IMAGE_TYPE_GRAYSCALE:
      std::fill(sd_buffer_.begin(), sd_buffer_.end(), 64); // Gris foncé
      break;
    case IMAGE_TYPE_BINARY:
      std::fill(sd_buffer_.begin(), sd_buffer_.end(), 0x00); // Noir
      break;
  }
  
  ESP_LOGI(TAG, "PNG decode completed (test pattern generated)");
  return true;
}

size_t Image::get_expected_buffer_size() const {
  switch (type_) {
    case IMAGE_TYPE_RGB565:
      return width_ * height_ * (transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 3 : 2);
    case IMAGE_TYPE_RGB:
      return width_ * height_ * (transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 4 : 3);
    case IMAGE_TYPE_GRAYSCALE:
      return width_ * height_;
    case IMAGE_TYPE_BINARY:
      return ((width_ + 7) / 8) * height_;
    default:
      return width_ * height_ * 3;
  }
}

#ifdef USE_LVGL
lv_img_dsc_t *Image::get_lv_img_dsc() {
  const uint8_t *data_ptr = sd_buffer_.empty() ? this->data_start_ : sd_buffer_.data();
  
  if (this->dsc_.data != data_ptr) {
    this->dsc_.data = data_ptr;
    this->dsc_.header.always_zero = 0;
    this->dsc_.header.reserved = 0;
    this->dsc_.header.w = this->width_;
    this->dsc_.header.h = this->height_;
    this->dsc_.data_size = this->get_width_stride() * this->get_height();
    switch (this->get_type()) {
      case IMAGE_TYPE_BINARY:
        this->dsc_.header.cf = LV_IMG_CF_ALPHA_1BIT;
        break;
      case IMAGE_TYPE_GRAYSCALE:
        this->dsc_.header.cf = LV_IMG_CF_ALPHA_8BIT;
        break;
      case IMAGE_TYPE_RGB:
#if LV_COLOR_DEPTH == 32
        switch (this->transparency_) {
          case TRANSPARENCY_ALPHA_CHANNEL:
            this->dsc_.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
            break;
          case TRANSPARENCY_CHROMA_KEY:
            this->dsc_.header.cf = LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED;
            break;
          default:
            this->dsc_.header.cf = LV_IMG_CF_TRUE_COLOR;
            break;
        }
#else
        this->dsc_.header.cf =
            this->transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? LV_IMG_CF_RGBA8888 : LV_IMG_CF_RGB888;
#endif
        break;
      case IMAGE_TYPE_RGB565:
#if LV_COLOR_DEPTH == 16
        switch (this->transparency_) {
          case TRANSPARENCY_ALPHA_CHANNEL:
            this->dsc_.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
            break;
          case TRANSPARENCY_CHROMA_KEY:
            this->dsc_.header.cf = LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED;
            break;
          default:
            this->dsc_.header.cf = LV_IMG_CF_TRUE_COLOR;
            break;
        }
#else
        this->dsc_.header.cf = this->transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? LV_IMG_CF_RGB565A8 : LV_IMG_CF_RGB565;
#endif
        break;
    }
  }
  return &this->dsc_;
}
#endif

bool Image::get_binary_pixel_(int x, int y) const {
  const uint32_t width_8 = ((this->width_ + 7u) / 8u) * 8u;
  const uint32_t pos = x + y * width_8;
  return this->get_data_byte_(pos / 8u) & (0x80 >> (pos % 8u));
}

Color Image::get_rgb_pixel_(int x, int y) const {
  const uint32_t pos = (x + y * this->width_) * this->bpp_ / 8;
  Color color = Color(this->get_data_byte_(pos + 0), this->get_data_byte_(pos + 1),
                      this->get_data_byte_(pos + 2), 0xFF);

  switch (this->transparency_) {
    case TRANSPARENCY_CHROMA_KEY:
      if (color.g == 1 && color.r == 0 && color.b == 0) {
        color.w = 0;
      }
      break;
    case TRANSPARENCY_ALPHA_CHANNEL:
      color.w = this->get_data_byte_(pos + 3);
      break;
    default:
      break;
  }
  return color;
}

Color Image::get_rgb565_pixel_(int x, int y) const {
  const uint32_t pos = (x + y * this->width_) * this->bpp_ / 8;
  uint16_t rgb565 = encode_uint16(this->get_data_byte_(pos), this->get_data_byte_(pos + 1));
  auto r = (rgb565 & 0xF800) >> 11;
  auto g = (rgb565 & 0x07E0) >> 5;
  auto b = rgb565 & 0x001F;
  auto a = 0xFF;
  switch (this->transparency_) {
    case TRANSPARENCY_ALPHA_CHANNEL:
      a = this->get_data_byte_(pos + 2);
      break;
    case TRANSPARENCY_CHROMA_KEY:
      if (rgb565 == 0x0020)
        a = 0;
      break;
    default:
      break;
  }
  return Color((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), a);
}

Color Image::get_grayscale_pixel_(int x, int y) const {
  const uint32_t pos = (x + y * this->width_);
  const uint8_t gray = this->get_data_byte_(pos);
  switch (this->transparency_) {
    case TRANSPARENCY_CHROMA_KEY:
      if (gray == 1)
        return Color(0, 0, 0, 0);
      return Color(gray, gray, gray, 0xFF);
    case TRANSPARENCY_ALPHA_CHANNEL:
      return Color(0, 0, 0, gray);
    default:
      return Color(gray, gray, gray, 0xFF);
  }
}

int Image::get_width() const { return this->width_; }
int Image::get_height() const { return this->height_; }
ImageType Image::get_type() const { return this->type_; }

Image::Image(const uint8_t *data_start, int width, int height, ImageType type, Transparency transparency)
    : width_(width), height_(height), type_(type), data_start_(data_start), transparency_(transparency) {
  switch (this->type_) {
    case IMAGE_TYPE_BINARY:
      this->bpp_ = 1;
      break;
    case IMAGE_TYPE_GRAYSCALE:
      this->bpp_ = 8;
      break;
    case IMAGE_TYPE_RGB565:
      this->bpp_ = transparency == TRANSPARENCY_ALPHA_CHANNEL ? 24 : 16;
      break;
    case IMAGE_TYPE_RGB:
      this->bpp_ = this->transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 32 : 24;
      break;
  }
}

}  // namespace image
}  // namespace esphome
