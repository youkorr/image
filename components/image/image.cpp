#include "image.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "../sd_mmc_card/sd_mmc_card.h"

namespace esphome {
namespace image {

static const char *const TAG = "image";

void Image::draw(int x, int y, display::Display *display, Color color_on, Color color_off) {
  // Essaye de charger depuis la SD si pas encore fait
  if (sd_runtime_ && sd_buffer_.empty() && !sd_path_.empty()) {
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

  // Ici vous devez utiliser votre composant sd_mmc_card pour lire le fichier
  // et décoder l'image JPG/PNG avec les décodeurs natifs d'ESPHome
  
  // Exemple d'utilisation (à adapter selon votre API SD):
  /*
  std::vector<uint8_t> file_data;
  if (!your_sd_component->read_file(sd_path_, file_data)) {
    ESP_LOGE(TAG, "Failed to read file from SD: %s", sd_path_.c_str());
    return false;
  }
  
  // Utilise les décodeurs d'image natifs d'ESPHome
  bool success = decode_image(file_data);
  if (!success) {
    ESP_LOGE(TAG, "Failed to decode image: %s", sd_path_.c_str());
    sd_buffer_.clear();
    return false;
  }
  */
  
  // Pour l'instant, créé un motif de test
  ESP_LOGW(TAG, "SD file reading not implemented - using test pattern");
  
  // Calcule la taille attendue du buffer
  size_t expected_size = get_expected_buffer_size();
  
  // Alloue le buffer si nécessaire
  if (sd_buffer_.empty()) {
    sd_buffer_.resize(expected_size, 0);
    ESP_LOGI(TAG, "Allocated SD buffer: %zu bytes", expected_size);
  }
  
  // Crée un motif de test selon le type d'image
  create_test_pattern();
  
  ESP_LOGI(TAG, "Test pattern loaded (replace with real image decoding)");
  return true;
}

bool Image::decode_image(const std::vector<uint8_t> &file_data) {
  // Cette méthode devrait utiliser les décodeurs natifs d'ESPHome
  // Vous devrez probablement utiliser des fonctions comme :
  // - image::decode_image() pour PNG/JPG
  // - ou directement les décodeurs internes selon la version d'ESPHome
  
  ESP_LOGI(TAG, "Decoding image data (%zu bytes)", file_data.size());
  
  // Calcule la taille attendue du buffer
  size_t expected_size = get_expected_buffer_size();
  
  // Alloue le buffer si nécessaire
  if (sd_buffer_.size() != expected_size) {
    sd_buffer_.resize(expected_size, 0);
  }
  
  /*
  // Exemple d'utilisation des décodeurs ESPHome (à adapter selon la version):
  
  // Détecte le format
  bool is_png = file_data.size() > 8 && 
                file_data[0] == 0x89 && file_data[1] == 0x50 && 
                file_data[2] == 0x4E && file_data[3] == 0x47;
  
  bool is_jpg = file_data.size() > 2 && 
                file_data[0] == 0xFF && file_data[1] == 0xD8;
  
  if (is_png) {
    // Utilise le décodeur PNG d'ESPHome
    // return decode_png(file_data.data(), file_data.size(), sd_buffer_.data(), sd_buffer_.size());
  } else if (is_jpg) {
    // Utilise le décodeur JPEG d'ESPHome
    // return decode_jpeg(file_data.data(), file_data.size(), sd_buffer_.data(), sd_buffer_.size());
  } else {
    ESP_LOGE(TAG, "Unsupported image format");
    return false;
  }
  */
  
  // Pour l'instant, retourne true avec un pattern de test
  create_test_pattern();
  return true;
}

void Image::create_test_pattern() {
  // Crée un motif de test selon le type d'image
  for (size_t i = 0; i < sd_buffer_.size(); i++) {
    switch (type_) {
      case IMAGE_TYPE_RGB565: {
        // Motif coloré pour RGB565
        int pixel = i / 2;
        int x = pixel % width_;
        int y = pixel / width_;
        if (i % 2 == 0) {
          // Byte bas
          sd_buffer_[i] = ((x * 32 / width_) << 3) | (y * 8 / height_);
        } else {
          // Byte haut  
          sd_buffer_[i] = ((x * 32 / width_) << 3) | ((y * 64 / height_) >> 3);
        }
        break;
      }
      case IMAGE_TYPE_RGB: {
        // Motif RGB
        int pixel = i / 3;
        int x = pixel % width_;
        int y = pixel / width_;
        int component = i % 3;
        if (component == 0) {
          sd_buffer_[i] = (x * 255) / width_;  // Rouge
        } else if (component == 1) {
          sd_buffer_[i] = (y * 255) / height_; // Vert
        } else {
          sd_buffer_[i] = ((x + y) * 127) / (width_ + height_); // Bleu
        }
        break;
      }
      case IMAGE_TYPE_GRAYSCALE: {
        // Gradient en niveaux de gris
        int pixel = i;
        int x = pixel % width_;
        int y = pixel / width_;
        sd_buffer_[i] = ((x + y) * 255) / (width_ + height_);
        break;
      }
      case IMAGE_TYPE_BINARY: {
        // Motif en damier pour binaire
        int bit_pos = i * 8;
        uint8_t byte_val = 0;
        for (int b = 0; b < 8 && (bit_pos + b) < (width_ * height_); b++) {
          int pixel = bit_pos + b;
          int x = pixel % width_;
          int y = pixel / width_;
          if ((x / 8 + y / 8) % 2) {
            byte_val |= (0x80 >> b);
          }
        }
        sd_buffer_[i] = byte_val;
        break;
      }
      default:
        sd_buffer_[i] = (i * 123) % 256;
        break;
    }
  }
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

// Méthodes pour la configuration runtime depuis la SD
void Image::set_sd_path(const std::string &path) {
  sd_path_ = path;
  sd_buffer_.clear(); // Force le rechargement
}

void Image::set_sd_runtime(bool enable) {
  sd_runtime_ = enable;
  if (!enable) {
    sd_buffer_.clear(); // Libère la mémoire si pas utilisé
  }
}

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
