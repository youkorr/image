#include "image.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "../sd_mmc_card/sd_mmc_card.h"

// Pour le décodage JPEG sur ESP32
#ifdef USE_ESP32
#include "esp_jpeg_dec.h"
#endif

namespace esphome {
namespace image {

static const char *const TAG = "image";

void Image::draw(int x, int y, display::Display *display, Color color_on, Color color_off) {
  // Essaye de charger depuis la SD si pas encore fait
  if (sd_runtime_ && !sd_buffer_.empty() == false && !sd_path_.empty()) {
    load_from_sd();
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
                continue;  // skip drawing
              }
              break;
            case TRANSPARENCY_ALPHA_CHANNEL: {
              auto on = (float) gray / 255.0f;
              auto off = 1.0f - on;
              // blend color_on and color_off
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

// Nouvelle méthode pour obtenir un byte de données (SD buffer ou placeholder)
uint8_t Image::get_data_byte_(size_t pos) const {
  if (!sd_buffer_.empty()) {
    // Utilise les données chargées depuis la SD
    if (pos < sd_buffer_.size()) {
      return sd_buffer_[pos];
    }
    ESP_LOGW(TAG, "Position %zu out of bounds for SD buffer (size: %zu)", pos, sd_buffer_.size());
    return 0;
  } else {
    // Utilise le placeholder original
    return progmem_read_byte(this->data_start_ + pos);
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

bool Image::load_from_sd() {
  if (sd_path_.empty()) {
    ESP_LOGE(TAG, "SD path is empty");
    return false;
  }

  ESP_LOGI(TAG, "Loading image from SD: %s", sd_path_.c_str());

  // Vérifie l'accès à la SD (utilise votre composant sd_mmc_card)
  // Remplacez par la méthode appropriée selon votre composant SD
  
  // Calcule la taille attendue du buffer
  size_t expected_size = get_expected_buffer_size();
  
  // Alloue le buffer si nécessaire
  if (sd_buffer_.empty()) {
    sd_buffer_.resize(expected_size, 0);
    ESP_LOGI(TAG, "Allocated SD buffer: %zu bytes", expected_size);
  }

  // Détermine le type de fichier
  std::string path_lower = sd_path_;
  std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
  
  bool success = false;
  if (path_lower.find(".jpg") != std::string::npos || path_lower.find(".jpeg") != std::string::npos) {
    success = decode_jpeg_from_sd();
  } else if (path_lower.find(".png") != std::string::npos) {
    ESP_LOGW(TAG, "PNG decoding not implemented yet");
    success = false;
  } else {
    ESP_LOGE(TAG, "Unsupported image format: %s", sd_path_.c_str());
    success = false;
  }

  if (success) {
    ESP_LOGI(TAG, "Successfully loaded image from SD: %s", sd_path_.c_str());
  } else {
    ESP_LOGE(TAG, "Failed to load image from SD: %s", sd_path_.c_str());
    // Vide le buffer en cas d'échec pour utiliser le placeholder
    sd_buffer_.clear();
  }

  return success;
}

bool Image::decode_jpeg_from_sd() {
#ifdef USE_ESP32
  // Ici vous devez utiliser votre composant sd_mmc_card pour lire le fichier
  // Je vais montrer un exemple générique, adaptez selon votre API
  
  ESP_LOGI(TAG, "Attempting to decode JPEG: %s", sd_path_.c_str());
  
  // Exemple d'utilisation de votre composant SD (à adapter)
  // Remplacez par les méthodes de votre composant sd_mmc_card
  
  /*
  // Supposons que votre composant a une méthode read_file
  std::vector<uint8_t> jpeg_data;
  if (!your_sd_component->read_file(sd_path_, jpeg_data)) {
    ESP_LOGE(TAG, "Failed to read file from SD: %s", sd_path_.c_str());
    return false;
  }
  */
  
  // Pour l'instant, méthode de test - remplacez par votre lecture SD
  ESP_LOGW(TAG, "SD file reading not implemented - using placeholder");
  
  // Rempli le buffer avec un motif de test pour vérifier que ça marche
  for (size_t i = 0; i < sd_buffer_.size(); i++) {
    // Motif de test coloré pour RGB565
    if (type_ == IMAGE_TYPE_RGB565) {
      if (i % 2 == 0) {
        sd_buffer_[i] = (i / 1000) % 256;  // Pattern rouge/vert
      } else {
        sd_buffer_[i] = ((i + 500) / 1000) % 256;  // Pattern bleu
      }
    } else {
      sd_buffer_[i] = (i * 123) % 256;  // Pattern général
    }
  }
  
  ESP_LOGI(TAG, "Test pattern loaded (replace with real JPEG decoding)");
  return true;
  
  /*
  // Vraie implémentation JPEG (une fois que vous avez les données du fichier):
  esp_jpeg_image_cfg_t jpeg_cfg = {
    .indata = jpeg_data.data(),
    .indata_size = jpeg_data.size(),
    .outbuf = sd_buffer_.data(),
    .outbuf_size = sd_buffer_.size(),
    .out_format = JPEG_IMAGE_FORMAT_RGB565,  // Selon votre format
    .out_scale = JPEG_IMAGE_SCALE_0,
    .flags = {
      .swap_color_bytes = (type_ == IMAGE_TYPE_RGB565) ? 1 : 0,
    }
  };
  
  esp_jpeg_image_output_t outimg;
  esp_err_t err = esp_jpeg_decode(&jpeg_cfg, &outimg);
  
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
    return false;
  }
  
  ESP_LOGI(TAG, "JPEG decoded: %dx%d", outimg.width, outimg.height);
  return true;
  */
#else
  ESP_LOGE(TAG, "JPEG decoding only supported on ESP32");
  return false;
#endif
}

size_t Image::get_expected_buffer_size() const {
  size_t bytes_per_pixel = 0;
  
  switch (type_) {
    case IMAGE_TYPE_BINARY:
      return ((width_ + 7) / 8) * height_;
    case IMAGE_TYPE_GRAYSCALE:
      bytes_per_pixel = (transparency_ == TRANSPARENCY_ALPHA_CHANNEL) ? 2 : 1;
      break;
    case IMAGE_TYPE_RGB565:
      bytes_per_pixel = (transparency_ == TRANSPARENCY_ALPHA_CHANNEL) ? 3 : 2;
      break;
    case IMAGE_TYPE_RGB:
      bytes_per_pixel = (transparency_ == TRANSPARENCY_ALPHA_CHANNEL) ? 4 : 3;
      break;
    default:
      bytes_per_pixel = 3;
  }
  
  return width_ * height_ * bytes_per_pixel;
}

// Mise à jour des méthodes de lecture des pixels pour utiliser get_data_byte_
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

#ifdef USE_LVGL
lv_img_dsc_t *Image::get_lv_img_dsc() {
  // lazily construct lvgl image_dsc.
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
#endif  // USE_LVGL

// Reste du code existant...
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
