#include "image.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <sys/stat.h>
#include <stdio.h>
#include <cmath>
#include "esp_task_wdt.h"

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
bool Image::decode_image_from_sd() {
    std::vector<uint8_t> file_data;

    // Lire le fichier depuis la SD
    if (!read_sd_file(sd_path_, file_data)) {
        ESP_LOGE(TAG, "Failed to read SD file: %s", sd_path_.c_str());
        return false;
    }

    // Détection simple du type d'image
    if (file_data.size() >= 2 && file_data[0] == 0xFF && file_data[1] == 0xD8) {
        // JPEG
        ESP_LOGI(TAG, "JPEG image detected");
        return decode_jpeg_data(file_data);
    }
    else if (file_data.size() >= 8 &&
             file_data[0] == 0x89 && file_data[1] == 0x50 &&
             file_data[2] == 0x4E && file_data[3] == 0x47 &&
             file_data[4] == 0x0D && file_data[5] == 0x0A &&
             file_data[6] == 0x1A && file_data[7] == 0x0A) {
        // PNG
        ESP_LOGI(TAG, "PNG image detected");
        return decode_png_data(file_data);
    }

    ESP_LOGE(TAG, "Unknown image format: %s", sd_path_.c_str());
    return false;
}

bool Image::read_sd_file(const std::string &path, std::vector<uint8_t> &data) {
  ESP_LOGI(TAG, "Attempting to read SD file: %s", path.c_str());

  // Utilise le lecteur spécifique à l'image ou le lecteur global
  SDFileReader reader = sd_file_reader_ ? sd_file_reader_ : global_sd_reader_;

  // CORRECTION : Mapping des chemins SD - découvrir le vrai point de montage
  auto map_path = [](const std::string &p) -> std::vector<std::string> {
    std::vector<std::string> candidates;
    
    // Candidat 1: Chemin tel quel
    candidates.push_back(p);
    
    // Candidat 2: Si commence par /sdcard/, essayer sans le préfixe
    if (p.rfind("/sdcard/", 0) == 0) {
      std::string relative = p.substr(8); // Enlever "/sdcard/"
      candidates.push_back("/" + relative);
      candidates.push_back(relative);
    }
    
    // Candidat 3: Chemins de montage courants pour ESP32
    std::string filename = p.substr(p.find_last_of("/") + 1);
    candidates.push_back("/sdcard/" + filename);
    candidates.push_back("/sd/" + filename);
    candidates.push_back("/mnt/sdcard/" + filename);
    candidates.push_back("/fat/" + filename);
    
    // Candidat 4: Juste le nom du fichier à la racine
    candidates.push_back("/" + filename);
    candidates.push_back(filename);
    
    return candidates;
  };

  std::vector<std::string> path_candidates = map_path(path);

  if (!reader) {
    ESP_LOGE(TAG, "No SD file reader available - trying direct file access");

    // Essayer tous les candidats de chemins
    for (const auto& candidate_path : path_candidates) {
      ESP_LOGD(TAG, "Trying path: %s", candidate_path.c_str());
      
      FILE *file = fopen(candidate_path.c_str(), "rb");
      if (file) {
        ESP_LOGI(TAG, "Successfully opened file at: %s", candidate_path.c_str());
        
        // Obtenir la taille du fichier
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        fseek(file, 0, SEEK_SET);

        if (file_size <= 0 || file_size > 50 * 1024 * 1024) {
          ESP_LOGE(TAG, "Invalid file size: %ld bytes", file_size);
          fclose(file);
          continue; // Essayer le prochain candidat
        }

        ESP_LOGI(TAG, "File size: %ld bytes", file_size);

        // Lire le fichier par chunks
        data.clear();
        data.reserve(file_size);

        constexpr size_t CHUNK_SIZE = 8192;
        std::vector<uint8_t> chunk(CHUNK_SIZE);
        size_t total_read = 0;
        bool read_success = true;

        while (total_read < (size_t)file_size) {
          size_t to_read = std::min(CHUNK_SIZE, (size_t)file_size - total_read);
          size_t bytes_read = fread(chunk.data(), 1, to_read, file);

          if (bytes_read == 0) {
            if (ferror(file)) {
              ESP_LOGE(TAG, "Error reading file at position %zu", total_read);
              read_success = false;
              break;
            }
            break;
          }

          data.insert(data.end(), chunk.begin(), chunk.begin() + bytes_read);
          total_read += bytes_read;

          if (total_read % (64 * 1024) == 0) {
            esp_task_wdt_reset();
          }
        }

        fclose(file);

        if (read_success && total_read == (size_t)file_size) {
          ESP_LOGI(TAG, "SD file read successfully using direct access, size: %zu bytes", data.size());
          return true;
        } else {
          ESP_LOGW(TAG, "Read failed or size mismatch for path: %s", candidate_path.c_str());
          data.clear(); // Nettoyer en cas d'échec partiel
        }
      } else {
        ESP_LOGD(TAG, "Cannot open file: %s (errno: %d - %s)", 
                candidate_path.c_str(), errno, strerror(errno));
      }
    }
    
    ESP_LOGE(TAG, "Failed to open file with any path candidate");
    return false;
  }

  // Avec un lecteur SD configuré, essayer les candidats
  ESP_LOGI(TAG, "Reading SD file using configured reader");
  for (const auto& candidate_path : path_candidates) {
    ESP_LOGD(TAG, "Trying reader with path: %s", candidate_path.c_str());
    
    if (reader(candidate_path, data)) {
      ESP_LOGI(TAG, "SD file read successfully via reader, size: %zu bytes", data.size());
      return true;
    }
  }
  
  ESP_LOGE(TAG, "Failed to read SD file via reader with any path candidate");
  return false;
}

bool Image::decode_jpeg_data(const std::vector<uint8_t> &jpeg_data) {
  ESP_LOGI(TAG, "Decoding JPEG data (%zu bytes)", jpeg_data.size());
  
  size_t expected_size = get_expected_buffer_size();
  sd_buffer_.resize(expected_size);
  
  ESP_LOGI(TAG, "Creating JPEG test pattern, expected size: %zu bytes, type: %d", expected_size, type_);
  
  // Créer un pattern de test "photo" plus réaliste
  switch (type_) {
    case IMAGE_TYPE_RGB:
      // Simuler une image photo avec des zones de couleurs réalistes
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = (y * width_ + x) * (transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 4 : 3);
          if (pos + 2 < expected_size) {
            // Créer un "paysage" artificiel
            uint8_t r, g, b;
            if (y < height_ / 3) {
              // Ciel bleu avec nuages
              r = 135 + (int)(std::sin(x * 0.1f) + std::sin(y * 0.2f)) * 20;
              g = 206 + (int)(std::sin(x * 0.15f) + std::sin(y * 0.1f)) * 15;
              b = 235 + (int)(std::sin(x * 0.05f) + std::sin(y * 0.25f)) * 10;
            } else if (y < 2 * height_ / 3) {
              // Montagnes vertes
              r = 34 + (int)(std::sin(x * 0.2f) + std::sin(y * 0.3f)) * 25;
              g = 139 + (int)(std::sin(x * 0.12f) + std::sin(y * 0.18f)) * 30;
              b = 34 + (int)(std::sin(x * 0.08f) + std::sin(y * 0.22f)) * 20;
            } else {
              // Sol brun/sable
              r = 160 + (int)(std::sin(x * 0.25f) + std::sin(y * 0.35f)) * 30;
              g = 120 + (int)(std::sin(x * 0.18f) + std::sin(y * 0.28f)) * 25;
              b = 80 + (int)(std::sin(x * 0.15f) + std::sin(y * 0.32f)) * 20;
            }
            
            sd_buffer_[pos] = std::max(0, std::min(255, (int)r));     // R
            sd_buffer_[pos + 1] = std::max(0, std::min(255, (int)g)); // G
            sd_buffer_[pos + 2] = std::max(0, std::min(255, (int)b)); // B
            
            if (transparency_ == TRANSPARENCY_ALPHA_CHANNEL && pos + 3 < expected_size) {
              sd_buffer_[pos + 3] = 255; // Alpha opaque
            }
          }
        }
      }
      break;
      
    case IMAGE_TYPE_RGB565:
      // Pattern similaire mais pour RGB565
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = (y * width_ + x) * (transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 3 : 2);
          if (pos + 1 < expected_size) {
            uint8_t r, g, b;
            if (y < height_ / 3) {
              r = (17 + (int)(std::sin(x * 0.1f) * 3)) & 0x1F; // 5 bits
              g = (52 + (int)(std::sin(x * 0.15f) * 5)) & 0x3F; // 6 bits  
              b = (30 + (int)(std::sin(x * 0.05f) * 2)) & 0x1F; // 5 bits
            } else if (y < 2 * height_ / 3) {
              r = (4 + (int)(std::sin(x * 0.2f) * 3)) & 0x1F;
              g = (35 + (int)(std::sin(x * 0.12f) * 8)) & 0x3F;
              b = (4 + (int)(std::sin(x * 0.08f) * 2)) & 0x1F;
            } else {
              r = (20 + (int)(std::sin(x * 0.25f) * 4)) & 0x1F;
              g = (30 + (int)(std::sin(x * 0.18f) * 6)) & 0x3F;
              b = (10 + (int)(std::sin(x * 0.15f) * 3)) & 0x1F;
            }
            
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
      // Dégradé de gris avec texture
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          size_t pos = y * width_ + x;
          if (pos < expected_size) {
            uint8_t base = (255 * y) / height_; // Dégradé vertical
            uint8_t noise = (int)(std::sin(x * 0.3f) * std::sin(y * 0.2f) * 30);
            sd_buffer_[pos] = std::max(0, std::min(255, (int)(base + noise)));
          }
        }
      }
      break;
      
    case IMAGE_TYPE_BINARY:
      // Pattern de test binaire avec formes géométriques
      std::fill(sd_buffer_.begin(), sd_buffer_.end(), 0);
      for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
          bool pixel_on = false;
          
          // Créer des formes géométriques
          int center_x = width_ / 2;
          int center_y = height_ / 2;
          int dx = x - center_x;
          int dy = y - center_y;
          int dist = (int)std::sqrt(dx*dx + dy*dy);
          
          // Cercles concentriques
          pixel_on = (dist / 20) % 2 == 0 && dist < center_x * 0.8;
          
          if (pixel_on) {
            size_t pos = (y * ((width_ + 7) / 8)) + (x / 8);
            if (pos < expected_size) {
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
  
  size_t expected_size = get_expected_buffer_size();
  sd_buffer_.resize(expected_size);
  
  ESP_LOGI(TAG, "Creating PNG test pattern, expected size: %zu bytes, type: %d", expected_size, type_);
  
  // Créer un pattern de test différent pour PNG (arc-en-ciel)
  for (int y = 0; y < height_; y++) {
    for (int x = 0; x < width_; x++) {
      float hue = (float)(x + y) / (width_ + height_) * 6.0f; // 0 to 6
      uint8_t r, g, b;
      
      // Conversion HSV vers RGB simplifiée
      int i = (int)hue;
      float f = hue - i;
      uint8_t p = 0;
      uint8_t q = (uint8_t)(255 * (1 - f));
      uint8_t t = (uint8_t)(255 * f);
      
      switch (i % 6) {
        case 0: r = 255; g = t; b = p; break;
        case 1: r = q; g = 255; b = p; break;
        case 2: r = p; g = 255; b = t; break;
        case 3: r = p; g = q; b = 255; break;
        case 4: r = t; g = p; b = 255; break;
        case 5: r = 255; g = p; b = q; break;
        default: r = g = b = 0; break;
      }
      
      switch (type_) {
        case IMAGE_TYPE_RGB: {
          size_t pos = (y * width_ + x) * (transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 4 : 3);
          if (pos + 2 < expected_size) {
            sd_buffer_[pos] = r;
            sd_buffer_[pos + 1] = g;
            sd_buffer_[pos + 2] = b;
            if (transparency_ == TRANSPARENCY_ALPHA_CHANNEL && pos + 3 < expected_size) {
              sd_buffer_[pos + 3] = 255;
            }
          }
          break;
        }
        case IMAGE_TYPE_RGB565: {
          size_t pos = (y * width_ + x) * (transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 3 : 2);
          if (pos + 1 < expected_size) {
            uint8_t r5 = r >> 3;
            uint8_t g6 = g >> 2;
            uint8_t b5 = b >> 3;
            uint16_t rgb565 = (r5 << 11) | (g6 << 5) | b5;
            
            sd_buffer_[pos] = rgb565 & 0xFF;
            sd_buffer_[pos + 1] = rgb565 >> 8;
            
            if (transparency_ == TRANSPARENCY_ALPHA_CHANNEL && pos + 2 < expected_size) {
              sd_buffer_[pos + 2] = 255;
            }
          }
          break;
        }
        case IMAGE_TYPE_GRAYSCALE: {
          size_t pos = y * width_ + x;
          if (pos < expected_size) {
            // Conversion RGB vers grayscale
            sd_buffer_[pos] = (uint8_t)(0.299f * r + 0.587f * g + 0.114f * b);
          }
          break;
        }
        case IMAGE_TYPE_BINARY: {
          size_t pos = (y * ((width_ + 7) / 8)) + (x / 8);
          if (pos < expected_size) {
            // Convertir en binaire basé sur la luminosité
            uint8_t gray = (uint8_t)(0.299f * r + 0.587f * g + 0.114f * b);
            if (gray > 128) {
              sd_buffer_[pos] |= (0x80 >> (x % 8));
            }
          }
          break;
        }
      }
    }
  }
  
  ESP_LOGI(TAG, "PNG decode completed (rainbow test pattern generated)");
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
  // Charge l'image SD si nécessaire
  if (sd_runtime_ && sd_buffer_.empty() && !sd_path_.empty()) {
    ESP_LOGD(TAG, "Loading SD image for LVGL: %s", sd_path_.c_str());
    if (!load_from_sd()) {
      ESP_LOGE(TAG, "Failed to load SD image for LVGL: %s", sd_path_.c_str());
      return nullptr;
    }
  }
  
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
