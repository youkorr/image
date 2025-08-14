#include "image.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <sys/stat.h>
#include <stdio.h>

namespace esphome {
namespace image {

static const char *const TAG = "image";

// Lecteur de fichier SD global
SDFileReader Image::global_sd_reader_ = nullptr;

void Image::draw(int x, int y, display::Display *display, Color color_on, Color color_off) {
  // Charge l'image depuis la SD si nécessaire
  if (sd_runtime_ && sd_buffer_.empty() && !sd_path_.empty()) {
    ESP_LOGI(TAG, "Attempting to load SD image: %s", sd_path_.c_str());
    if (!load_from_sd()) {
      ESP_LOGE(TAG, "Failed to load SD image: %s", sd_path_.c_str());
      // Fallback: dessiner un rectangle rouge pour indiquer l'erreur
      for (int dx = 0; dx < std::min(50, width_); dx++) {
        for (int dy = 0; dy < std::min(50, height_); dy++) {
          display->draw_pixel_at(x + dx, y + dy, Color(255, 0, 0));
        }
      }
      return;
    }
    ESP_LOGI(TAG, "SD image loaded successfully, buffer size: %zu bytes", sd_buffer_.size());
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

  ESP_LOGD(TAG, "Drawing image type %d, size %dx%d at (%d,%d), buffer empty: %s", 
           type_, width_, height_, x, y, sd_buffer_.empty() ? "yes" : "no");

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
    ESP_LOGW(TAG, "Accessing SD buffer beyond bounds: %zu >= %zu", pos, sd_buffer_.size());
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

  ESP_LOGI(TAG, "Loading image from SD: %s", sd_path_.c_str());
  
  return decode_image_from_sd();
}

bool Image::read_sd_file(const std::string &path, std::vector<uint8_t> &data) {
  // Utilise le lecteur spécifique à l'image ou le lecteur global
  SDFileReader reader = sd_file_reader_ ? sd_file_reader_ : global_sd_reader_;
  
  if (!reader) {
    ESP_LOGE(TAG, "No SD file reader available");
    return false;
  }
  
  ESP_LOGI(TAG, "Reading SD file: %s", path.c_str());
  bool result = reader(path, data);
  if (result) {
    ESP_LOGI(TAG, "SD file read successfully, size: %zu bytes", data.size());
  } else {
    ESP_LOGE(TAG, "Failed to read SD file: %s", path.c_str());
  }
  
  return result;
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
  // Affiche les premiers bytes pour debug
  if (file_data.size() >= 8) {
    ESP_LOGE(TAG, "File header: %02X %02X %02X %02X %02X %02X %02X %02X", 
             file_data[0], file_data[1], file_data[2], file_data[3],
             file_data[4], file_data[5], file_data[6], file_data[7]);
  }
  return false;
}

bool Image::decode_jpeg_data(const std::vector<uint8_t> &jpeg_data) {
  ESP_LOGI(TAG, "Decoding JPEG data (%zu bytes)", jpeg_data.size());
  
  // TODO: Intégrez ici une bibliothèque de décodage JPEG comme TJpgDec
  // Pour l'instant, on crée une image de test VISIBLE (pas noire)
  
  size_t expected_size = get_expected_buffer_size();
  sd_buffer_.resize(expected_size);
  
  ESP_LOGI(TAG, "Creating test pattern, expected size: %zu bytes, type: %d", expected_size, type_);
  
  // Créer un pattern de test visible selon le type d'image
  switch (type_) {
    case IMAGE_TYPE_RGB:
      // Pattern dégradé rouge->vert->bleu
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = (y * width_ + x) * (transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 4 : 3);
          if (pos + 2 < expected_size) {
            // Créer un dégradé coloré
            sd_buffer_[pos] = (255 * x) / width_;        // R
            sd_buffer_[pos + 1] = (255 * y) / height_;   // G  
            sd_buffer_[pos + 2] = 128;                   // B fixe
            if (transparency_ == TRANSPARENCY_ALPHA_CHANNEL && pos + 3 < expected_size) {
              sd_buffer_[pos + 3] = 255; // Alpha opaque
            }
          }
        }
      }
      break;
      
    case IMAGE_TYPE_RGB565:
      // Pattern dégradé en RGB565
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = (y * width_ + x) * (transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 3 : 2);
          if (pos + 1 < expected_size) {
            uint8_t r = (31 * x) / width_;
            uint8_t g = (63 * y) / height_;
            uint8_t b = 16; // Bleu fixe
            uint16_t rgb565 = (r << 11) | (g << 5) | b;
            
            // Little endian
            sd_buffer_[pos] = rgb565 & 0xFF;
            sd_buffer_[pos + 1] = rgb565 >> 8;
            
            if (transparency_ == TRANSPARENCY_ALPHA_CHANNEL && pos + 2 < expected_size) {
              sd_buffer_[pos + 2] = 255; // Alpha opaque
            }
          }
        }
      }
      break;
      
    case IMAGE_TYPE_GRAYSCALE:
      // Dégradé de gris
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = y * width_ + x;
          if (pos < expected_size) {
            sd_buffer_[pos] = (255 * (x + y)) / (width_ + height_);
          }
        }
      }
      break;
      
    case IMAGE_TYPE_BINARY:
      // Pattern de damier
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = (y * ((width_ + 7) / 8)) + (x / 8);
          if (pos < expected_size) {
            bool pixel_on = ((x / 16) + (y / 16)) % 2 == 0;
            if (pixel_on) {
              sd_buffer_[pos] |= (0x80 >> (x % 8));
            }
          }
        }
      }
      break;
  }
  
  ESP_LOGI(TAG, "JPEG decode completed (test pattern generated), first few bytes: %02X %02X %02X %02X", 
           sd_buffer_[0], sd_buffer_[1], sd_buffer_[2], sd_buffer_[3]);
  return true;
}

bool Image::decode_png_data(const std::vector<uint8_t> &png_data) {
  ESP_LOGI(TAG, "Decoding PNG data (%zu bytes)", png_data.size());
  
  // TODO: Intégrez ici une bibliothèque de décodage PNG
  // Pour l'instant, on crée une image de test VISIBLE (pas noire)
  
  size_t expected_size = get_expected_buffer_size();
  sd_buffer_.resize(expected_size);
  
  ESP_LOGI(TAG, "Creating test pattern, expected size: %zu bytes, type: %d", expected_size, type_);
  
  // Créer un pattern de test différent pour PNG (cercles concentriques)
  int center_x = width_ / 2;
  int center_y = height_ / 2;
  
  switch (type_) {
    case IMAGE_TYPE_RGB:
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = (y * width_ + x) * (transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 4 : 3);
          if (pos + 2 < expected_size) {
            int dx = x - center_x;
            int dy = y - center_y;
            int dist = sqrt(dx*dx + dy*dy);
            sd_buffer_[pos] = 0;                          // R
            sd_buffer_[pos + 1] = (dist * 255) / center_x; // G
            sd_buffer_[pos + 2] = 255 - ((dist * 255) / center_x); // B
            if (transparency_ == TRANSPARENCY_ALPHA_CHANNEL && pos + 3 < expected_size) {
              sd_buffer_[pos + 3] = 255; // Alpha opaque
            }
          }
        }
      }
      break;
      
    case IMAGE_TYPE_RGB565:
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = (y * width_ + x) * (transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 3 : 2);
          if (pos + 1 < expected_size) {
            int dx = x - center_x;
            int dy = y - center_y;
            int dist = sqrt(dx*dx + dy*dy);
            
            uint8_t r = 0;
            uint8_t g = (31 * dist) / center_x;
            uint8_t b = 31 - ((31 * dist) / center_x);
            uint16_t rgb565 = (r << 11) | (g << 5) | b;
            
            // Little endian
            sd_buffer_[pos] = rgb565 & 0xFF;
            sd_buffer_[pos + 1] = rgb565 >> 8;
            
            if (transparency_ == TRANSPARENCY_ALPHA_CHANNEL && pos + 2 < expected_size) {
              sd_buffer_[pos + 2] = 255; // Alpha opaque
            }
          }
        }
      }
      break;
      
    case IMAGE_TYPE_GRAYSCALE:
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = y * width_ + x;
          if (pos < expected_size) {
            int dx = x - center_x;
            int dy = y - center_y;
            int dist = sqrt(dx*dx + dy*dy);
            sd_buffer_[pos] = 255 - ((dist * 255) / center_x);
          }
        }
      }
      break;
      
    case IMAGE_TYPE_BINARY:
      // Pattern de cercles concentriques
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = (y * ((width_ + 7) / 8)) + (x / 8);
          if (pos < expected_size) {
            int dx = x - center_x;
            int dy = y - center_y;
            int dist = sqrt(dx*dx + dy*dy);
            bool pixel_on = (dist / 20) % 2 == 0;
            if (pixel_on) {
              sd_buffer_[pos] |= (0x80 >> (x % 8));
            }
          }
        }
      }
      break;
  }
  
  ESP_LOGI(TAG, "PNG decode completed (test pattern generated), first few bytes: %02X %02X %02X %02X", 
           sd_buffer_[0], sd_buffer_[1], sd_buffer_[2], sd_buffer_[3]);
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
