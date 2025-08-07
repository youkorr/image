#include "image.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#ifdef USE_SD_MMC_CARD
#include "../sd_mmc_card/sd_mmc_card.h"
#include "esphome/core/log.h"

static const char *const TAG = "image.sd_card";
#endif

namespace esphome {
namespace image {

void Image::draw(int x, int y, display::Display *display, Color color_on, Color color_off) {
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
          const uint8_t gray = progmem_read_byte(this->data_start_ + pos);
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

#ifdef USE_LVGL
lv_img_dsc_t *Image::get_lv_img_dsc() {
  // lazily construct lvgl image_dsc.
  if (this->dsc_.data != this->data_start_) {
    this->dsc_.data = this->data_start_;
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

bool Image::get_binary_pixel_(int x, int y) const {
  const uint32_t width_8 = ((this->width_ + 7u) / 8u) * 8u;
  const uint32_t pos = x + y * width_8;
  return progmem_read_byte(this->data_start_ + (pos / 8u)) & (0x80 >> (pos % 8u));
}

Color Image::get_rgb_pixel_(int x, int y) const {
  const uint32_t pos = (x + y * this->width_) * this->bpp_ / 8;
  Color color = Color(progmem_read_byte(this->data_start_ + pos + 0), progmem_read_byte(this->data_start_ + pos + 1),
                      progmem_read_byte(this->data_start_ + pos + 2), 0xFF);

  switch (this->transparency_) {
    case TRANSPARENCY_CHROMA_KEY:
      if (color.g == 1 && color.r == 0 && color.b == 0) {
        // (0, 1, 0) has been defined as transparent color for non-alpha images.
        color.w = 0;
      }
      break;
    case TRANSPARENCY_ALPHA_CHANNEL:
      color.w = progmem_read_byte(this->data_start_ + (pos + 3));
      break;
    default:
      break;
  }
  return color;
}

Color Image::get_rgb565_pixel_(int x, int y) const {
  const uint8_t *pos = this->data_start_ + (x + y * this->width_) * this->bpp_ / 8;
  uint16_t rgb565 = encode_uint16(progmem_read_byte(pos), progmem_read_byte(pos + 1));
  auto r = (rgb565 & 0xF800) >> 11;
  auto g = (rgb565 & 0x07E0) >> 5;
  auto b = rgb565 & 0x001F;
  auto a = 0xFF;
  switch (this->transparency_) {
    case TRANSPARENCY_ALPHA_CHANNEL:
      a = progmem_read_byte(pos + 2);
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
  const uint8_t gray = progmem_read_byte(this->data_start_ + pos);
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

Image::Image(const uint8_t *data_start, int width, int height, ImageType type, TransparencyType transparency)
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

#ifdef USE_SD_MMC_CARD

SDCardImage::SDCardImage(const std::string &path, ImageType type, TransparencyType transparency)
    : Image(nullptr, 0, 0, type, transparency), path_(path), image_data_(nullptr), loaded_(false) {
  // Trouver le composant SD card
  this->sd_card_ = sd_mmc_card::global_sd_mmc_card;
  if (this->sd_card_ == nullptr) {
    ESP_LOGE(TAG, "SD card component not found");
    return;
  }
}

SDCardImage::~SDCardImage() {
  this->cleanup_();
}

void SDCardImage::cleanup_() {
  if (this->image_data_ != nullptr) {
    free(this->image_data_);
    this->image_data_ = nullptr;
  }
  this->loaded_ = false;
}

bool SDCardImage::load_image_() {
  if (this->loaded_) {
    return true;
  }

  if (this->sd_card_ == nullptr || !this->sd_card_->is_ready()) {
    ESP_LOGW(TAG, "SD card not ready");
    return false;
  }

  // Construire le chemin complet avec le point de montage
  std::string full_path = this->sd_card_->get_mount_path() + "/" + this->path_;
  
  // Ouvrir le fichier
  FILE *file = fopen(full_path.c_str(), "rb");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open image file: %s", full_path.c_str());
    return false;
  }

  // Pour simplifier, on assume que le fichier est au format BMP simple
  // Vous devrez adapter selon vos besoins (PNG, JPEG, etc.)
  
  // Lire l'en-tête BMP (54 bytes minimum)
  uint8_t header[54];
  if (fread(header, 1, 54, file) != 54) {
    ESP_LOGE(TAG, "Failed to read BMP header");
    fclose(file);
    return false;
  }

  // Vérifier la signature BMP
  if (header[0] != 'B' || header[1] != 'M') {
    ESP_LOGE(TAG, "Not a valid BMP file");
    fclose(file);
    return false;
  }

  // Extraire les dimensions
  int width = *(int32_t*)&header[18];
  int height = abs(*(int32_t*)&header[22]); // abs() pour gérer les BMP inversés
  int bpp = *(int16_t*)&header[28];

  ESP_LOGD(TAG, "BMP: %dx%d, %d bpp", width, height, bpp);

  // Appliquer le redimensionnement si configuré
  if (this->resize_width_ > 0 && this->resize_height_ > 0) {
    width = this->resize_width_;
    height = this->resize_height_;
  }

  // Calculer la taille des données selon le type d'image
  size_t data_size;
  switch (this->type_) {
    case IMAGE_TYPE_BINARY:
      data_size = ((width + 7) / 8) * height;
      this->bpp_ = 1;
      break;
    case IMAGE_TYPE_GRAYSCALE:
      data_size = width * height;
      this->bpp_ = 8;
      break;
    case IMAGE_TYPE_RGB565:
      data_size = width * height * (this->transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 3 : 2);
      this->bpp_ = this->transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 24 : 16;
      break;
    case IMAGE_TYPE_RGB:
      data_size = width * height * (this->transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 4 : 3);
      this->bpp_ = this->transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 32 : 24;
      break;
    default:
      ESP_LOGE(TAG, "Unsupported image type");
      fclose(file);
      return false;
  }

  // Allouer la mémoire
  this->cleanup_();
  this->image_data_ = (uint8_t*)malloc(data_size);
  if (this->image_data_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate memory for image data (%zu bytes)", data_size);
    fclose(file);
    return false;
  }

  // Lire et traiter les données d'image
  uint32_t data_offset = *(uint32_t*)&header[10];
  fseek(file, data_offset, SEEK_SET);
  
  // Lecture simplifiée - à adapter selon le format source et cible
  // Cette implémentation basique lit les données brutes
  size_t bytes_read = fread(this->image_data_, 1, data_size, file);
  if (bytes_read != data_size) {
    ESP_LOGW(TAG, "Read %zu bytes, expected %zu", bytes_read, data_size);
  }

  fclose(file);

  // Traitement post-lecture
  this->process_image_data_();

  // Mettre à jour les propriétés de l'image
  this->width_ = width;
  this->height_ = height;
  this->data_start_ = this->image_data_;
  this->loaded_ = true;

  ESP_LOGD(TAG, "Successfully loaded image: %s (%dx%d)", this->path_.c_str(), width, height);
  return true;
}

void SDCardImage::process_image_data_() {
  if (this->image_data_ == nullptr) return;

  // Appliquer l'inversion alpha si configurée
  if (this->invert_alpha_) {
    size_t pixel_count = this->width_ * this->height_;
    
    switch (this->type_) {
      case IMAGE_TYPE_BINARY:
        // Inverser tous les bits
        for (size_t i = 0; i < ((this->width_ + 7) / 8) * this->height_; i++) {
          this->image_data_[i] ^= 0xFF;
        }
        break;
        
      case IMAGE_TYPE_GRAYSCALE:
        // Inverser les valeurs de gris
        for (size_t i = 0; i < pixel_count; i++) {
          this->image_data_[i] ^= 0xFF;
        }
        break;
        
      case IMAGE_TYPE_RGB:
        if (this->transparency_ == TRANSPARENCY_ALPHA_CHANNEL) {
          // Inverser seulement le canal alpha
          for (size_t i = 0; i < pixel_count; i++) {
            this->image_data_[i * 4 + 3] ^= 0xFF;
          }
        }
        break;
        
      case IMAGE_TYPE_RGB565:
        if (this->transparency_ == TRANSPARENCY_ALPHA_CHANNEL) {
          // Inverser seulement le canal alpha
          for (size_t i = 0; i < pixel_count; i++) {
            this->image_data_[i * 3 + 2] ^= 0xFF;
          }
        }
        break;
    }
  }

  // Appliquer l'ordre des bytes si configuré pour RGB565
  if (this->type_ == IMAGE_TYPE_RGB565 && !this->big_endian_) {
    size_t stride = this->transparency_ == TRANSPARENCY_ALPHA_CHANNEL ? 3 : 2;
    for (size_t i = 0; i < this->width_ * this->height_ * stride; i += stride) {
      // Échanger les bytes pour RGB565
      uint8_t temp = this->image_data_[i];
      this->image_data_[i] = this->image_data_[i + 1];
      this->image_data_[i + 1] = temp;
    }
  }
}

void SDCardImage::draw(int x, int y, display::Display *display, Color color_on, Color color_off) {
  if (!this->load_image_()) {
    ESP_LOGW(TAG, "Failed to load image for drawing");
    return;
  }
  
  // Utiliser l'implémentation de la classe parent
  Image::draw(x, y, display, color_on, color_off);
}

Color SDCardImage::get_pixel(int x, int y, Color color_on, Color color_off) const {
  if (!const_cast<SDCardImage*>(this)->load_image_()) {
    return color_off;
  }
  
  // Utiliser l'implémentation de la classe parent
  return Image::get_pixel(x, y, color_on, color_off);
}

#endif  // USE_SD_MMC_CARD

}  // namespace image
}  // namespace esphome
