from __future__ import annotations

import hashlib
import io
import logging
from pathlib import Path
import re

from PIL import Image, UnidentifiedImageError

from esphome import core, external_files
import esphome.codegen as cg
from esphome.components.const import CONF_BYTE_ORDER
import esphome.config_validation as cv
from esphome.const import (
    CONF_DEFAULTS,
    CONF_DITHER,
    CONF_FILE,
    CONF_ICON,
    CONF_ID,
    CONF_PATH,
    CONF_RAW_DATA_ID,
    CONF_RESIZE,
    CONF_SOURCE,
    CONF_TYPE,
    CONF_URL,
)
from esphome.core import CORE, HexInt

_LOGGER = logging.getLogger(__name__)

DOMAIN = "image"
DEPENDENCIES = ["display"]

image_ns = cg.esphome_ns.namespace("image")

ImageType = image_ns.enum("ImageType")
TransparencyType = image_ns.enum("TransparencyType")

CONF_OPAQUE = "opaque"
CONF_CHROMA_KEY = "chroma_key"
CONF_ALPHA_CHANNEL = "alpha_channel"
CONF_INVERT_ALPHA = "invert_alpha"
CONF_IMAGES = "images"

TRANSPARENCY_TYPES = (
    CONF_OPAQUE,
    CONF_CHROMA_KEY,
    CONF_ALPHA_CHANNEL,
)


def get_image_type_enum(type):
    return getattr(ImageType, f"IMAGE_TYPE_{type.upper()}")


def get_transparency_enum(transparency):
    return getattr(TransparencyType, f"TRANSPARENCY_{transparency.upper()}")


class ImageEncoder:
    """
    Superclass of image type encoders
    """

    # Control which transparency options are available for a given type
    allow_config = {CONF_ALPHA_CHANNEL, CONF_CHROMA_KEY, CONF_OPAQUE}

    # All imageencoder types are valid
    @staticmethod
    def validate(value):
        return value

    def __init__(self, width, height, transparency, dither, invert_alpha):
        """
        :param width:  The image width in pixels
        :param height:  The image height in pixels
        :param transparency: Transparency type
        :param dither: Dither method
        :param invert_alpha: True if the alpha channel should be inverted; for monochrome formats inverts the colours.
        """
        self.transparency = transparency
        self.width = width
        self.height = height
        self.data = [0 for _ in range(width * height)]
        self.dither = dither
        self.index = 0
        self.invert_alpha = invert_alpha
        self.path = ""

    def convert(self, image, path):
        """
        Convert the image format
        :param image:  Input image
        :param path:  Path to the image file
        :return: converted image
        """
        return image

    def encode(self, pixel):
        """
        Encode a single pixel
        """

    def end_row(self):
        """
        Marks the end of a pixel row
        :return:
        """


def is_alpha_only(image: Image):
    """
    Check if an image (assumed to be RGBA) is only alpha
    """
    # Any alpha data?
    if image.split()[-1].getextrema()[0] == 0xFF:
        return False
    return all(b.getextrema()[1] == 0 for b in image.split()[:-1])


class ImageBinary(ImageEncoder):
    allow_config = {CONF_OPAQUE, CONF_INVERT_ALPHA, CONF_CHROMA_KEY}

    def __init__(self, width, height, transparency, dither, invert_alpha):
        self.width8 = (width + 7) // 8
        super().__init__(self.width8, height, transparency, dither, invert_alpha)
        self.bitno = 0

    def convert(self, image, path):
        if is_alpha_only(image):
            image = image.split()[-1]
        return image.convert("1", dither=self.dither)

    def encode(self, pixel):
        if self.invert_alpha:
            pixel = not pixel
        if pixel:
            self.data[self.index] |= 0x80 >> (self.bitno % 8)
        self.bitno += 1
        if self.bitno == 8:
            self.bitno = 0
            self.index += 1

    def end_row(self):
        """
        Pad rows to a byte boundary
        """
        if self.bitno != 0:
            self.bitno = 0
            self.index += 1


class ImageGrayscale(ImageEncoder):
    allow_config = {CONF_ALPHA_CHANNEL, CONF_CHROMA_KEY, CONF_INVERT_ALPHA, CONF_OPAQUE}

    def convert(self, image, path):
        if is_alpha_only(image):
            if self.transparency != CONF_ALPHA_CHANNEL:
                _LOGGER.warning(
                    "Grayscale image %s is alpha only, but transparency is set to %s",
                    path,
                    self.transparency,
                )
                self.transparency = CONF_ALPHA_CHANNEL
            image = image.split()[-1]
        return image.convert("LA")

    def encode(self, pixel):
        b, a = pixel
        if self.transparency == CONF_CHROMA_KEY:
            if b == 1:
                b = 0
            if a != 0xFF:
                b = 1
        if self.invert_alpha:
            b ^= 0xFF
        if self.transparency == CONF_ALPHA_CHANNEL:
            if a != 0xFF:
                b = a
        self.data[self.index] = b
        self.index += 1


class ImageRGB565(ImageEncoder):
    def __init__(self, width, height, transparency, dither, invert_alpha):
        stride = 3 if transparency == CONF_ALPHA_CHANNEL else 2
        super().__init__(
            width * stride,
            height,
            transparency,
            dither,
            invert_alpha,
        )
        self.big_endian = True

    def set_big_endian(self, big_endian: bool) -> None:
        self.big_endian = big_endian

    def convert(self, image, path):
        return image.convert("RGBA")

    def encode(self, pixel):
        r, g, b, a = pixel
        r = r >> 3
        g = g >> 2
        b = b >> 3
        if self.transparency == CONF_CHROMA_KEY:
            if r == 0 and g == 1 and b == 0:
                g = 0
            elif a < 128:
                r = 0
                g = 1
                b = 0
        rgb = (r << 11) | (g << 5) | b
        if self.big_endian:
            self.data[self.index] = rgb >> 8
            self.index += 1
            self.data[self.index] = rgb & 0xFF
            self.index += 1
        else:
            self.data[self.index] = rgb & 0xFF
            self.index += 1
            self.data[self.index] = rgb >> 8
            self.index += 1
        if self.transparency == CONF_ALPHA_CHANNEL:
            if self.invert_alpha:
                a ^= 0xFF
            self.data[self.index] = a
            self.index += 1


class ImageRGB(ImageEncoder):
    def __init__(self, width, height, transparency, dither, invert_alpha):
        stride = 4 if transparency == CONF_ALPHA_CHANNEL else 3
        super().__init__(
            width * stride,
            height,
            transparency,
            dither,
            invert_alpha,
        )

    def convert(self, image, path):
        return image.convert("RGBA")

    def encode(self, pixel):
        r, g, b, a = pixel
        if self.transparency == CONF_CHROMA_KEY:
            if r == 0 and g == 1 and b == 0:
                g = 0
            elif a < 128:
                r = 0
                g = 1
                b = 0
        self.data[self.index] = r
        self.index += 1
        self.data[self.index] = g
        self.index += 1
        self.data[self.index] = b
        self.index += 1
        if self.transparency == CONF_ALPHA_CHANNEL:
            if self.invert_alpha:
                a ^= 0xFF
            self.data[self.index] = a
            self.index += 1


class ReplaceWith:
    """
    Placeholder class to provide feedback on deprecated features
    """

    allow_config = {CONF_ALPHA_CHANNEL, CONF_CHROMA_KEY, CONF_OPAQUE}

    def __init__(self, replace_with):
        self.replace_with = replace_with

    def validate(self, value):
        raise cv.Invalid(
            f"Image type {value} is removed; replace with {self.replace_with}"
        )


IMAGE_TYPE = {
    "BINARY": ImageBinary,
    "GRAYSCALE": ImageGrayscale,
    "RGB565": ImageRGB565,
    "RGB": ImageRGB,
    "TRANSPARENT_BINARY": ReplaceWith("'type: BINARY' and 'transparency: chroma_key'"),
    "RGB24": ReplaceWith("'type: RGB'"),
    "RGBA": ReplaceWith("'type: RGB' and 'transparency: alpha_channel'"),
}

CONF_TRANSPARENCY = "transparency"

# If the MDI file cannot be downloaded within this time, abort.
IMAGE_DOWNLOAD_TIMEOUT = 30  # seconds

SOURCE_LOCAL = "local"
SOURCE_WEB = "web"
SOURCE_SD_CARD = "sd_card"
SOURCE_MDI = "mdi"
SOURCE_MDIL = "mdil"
SOURCE_MEMORY = "memory"

MDI_SOURCES = {
    SOURCE_MDI: "https://raw.githubusercontent.com/Templarian/MaterialDesign/master/svg/",
    SOURCE_MDIL: "https://raw.githubusercontent.com/Pictogrammers/MaterialDesignLight/refs/heads/master/svg/",
    SOURCE_MEMORY: "https://raw.githubusercontent.com/Pictogrammers/Memory/refs/heads/main/src/svg/",
}

Image_ = image_ns.class_("Image")
SDCardImage_ = image_ns.class_("SDCardImage", Image_)

INSTANCE_TYPE = Image_


def compute_local_image_path(value) -> Path:
    url = value[CONF_URL] if isinstance(value, dict) else value
    h = hashlib.new("sha256")
    h.update(url.encode())
    key = h.hexdigest()[:8]
    base_dir = external_files.compute_local_file_dir(DOMAIN)
    return base_dir / key


def local_path(value):
    value = value[CONF_PATH] if isinstance(value, dict) else value
    return str(CORE.relative_config_path(value))


def download_file(url, path):
    external_files.download_content(url, path, IMAGE_DOWNLOAD_TIMEOUT)
    return str(path)


def download_gh_svg(value, source):
    mdi_id = value[CONF_ICON] if isinstance(value, dict) else value
    base_dir = external_files.compute_local_file_dir(DOMAIN) / source
    path = base_dir / f"{mdi_id}.svg"

    url = MDI_SOURCES[source] + mdi_id + ".svg"
    return download_file(url, path)


def download_image(value):
    value = value[CONF_URL] if isinstance(value, dict) else value
    return download_file(value, compute_local_image_path(value))


def is_svg_file(file):
    if not file:
        return False
    with open(file, "rb") as f:
        return "<svg" in str(f.read(1024))


def validate_cairosvg_installed():
    try:
        import cairosvg
    except ImportError as err:
        raise cv.Invalid(
            "Please install the cairosvg python package to use this feature. "
            "(pip install cairosvg)"
        ) from err

    major, minor, _ = cairosvg.__version__.split(".")
    if major < "2" or major == "2" and minor < "2":
        raise cv.Invalid(
            "Please update your cairosvg installation to at least 2.2.0. "
            "(pip install -U cairosvg)"
        )


def validate_file_shorthand(value):
    value = cv.string_strict(value)
    parts = value.strip().split(":")
    if len(parts) == 2 and parts[0] in MDI_SOURCES:
        match = re.match(r"^[a-zA-Z0-9\-]+$", parts[1])
        if match is None:
            raise cv.Invalid(f"Could not parse mdi icon name from '{value}'.")
        return download_gh_svg(parts[1], parts[0])

    if value.startswith("http://") or value.startswith("https://"):
        return download_image(value)

    value = cv.file_(value)
    return local_path(value)


LOCAL_SCHEMA = cv.All(
    {
        cv.Required(CONF_PATH): cv.file_,
    },
    local_path,
)


def mdi_schema(source):
    def validate_mdi(value):
        return download_gh_svg(value, source)

    return cv.All(
        cv.Schema(
            {
                cv.Required(CONF_ICON): cv.string,
            }
        ),
        validate_mdi,
    )


WEB_SCHEMA = cv.All(
    {
        cv.Required(CONF_URL): cv.string,
    },
    download_image,
)


SD_CARD_SCHEMA = cv.All(
    {
        cv.Required(CONF_PATH): cv.string,
    },
    lambda value: value[CONF_PATH],
)

TYPED_FILE_SCHEMA = cv.typed_schema(
    {
        SOURCE_LOCAL: LOCAL_SCHEMA,
        SOURCE_WEB: WEB_SCHEMA,
        SOURCE_SD_CARD: SD_CARD_SCHEMA,
    }
    | {source: mdi_schema(source) for source in MDI_SOURCES},
    key=CONF_SOURCE,
)

def validate_transparency(choices=TRANSPARENCY_TYPES):
    def validate(value):
        if isinstance(value, bool):
            value = str(value)
        return cv.one_of(*choices, lower=True)(value)

    return validate


def validate_type(image_types):
    def validate(value):
        value = cv.one_of(*image_types, upper=True)(value)
        return IMAGE_TYPE[value].validate(value)

    return validate


def validate_image_config(value):
    """
    Validate a single image configuration after schema validation
    """
    # Skip validation for SD card images as they are processed at runtime
    if isinstance(value, dict) and value.get(CONF_SOURCE) == SOURCE_SD_CARD:
        return value
    
    # Ensure we're working with a dictionary
    if not isinstance(value, dict):
        return value  # Let schema validation handle this
    
    # Check that required keys are present
    if CONF_TYPE not in value:
        return value  # Let schema validation handle this
    
    conf_type = value[CONF_TYPE]
    if conf_type not in IMAGE_TYPE:
        return value  # Let schema validation handle this
        
    type_class = IMAGE_TYPE[conf_type]
    
    # Use default value if transparency is not defined
    transparency = value.get(CONF_TRANSPARENCY, CONF_OPAQUE)
    if isinstance(transparency, str):
        transparency = transparency.lower()
    
    if transparency not in type_class.allow_config:
        raise cv.Invalid(
            f"Image format '{conf_type}' cannot have transparency: {transparency}"
        )
    
    invert_alpha = value.get(CONF_INVERT_ALPHA, False)
    if (
        invert_alpha
        and transparency != CONF_ALPHA_CHANNEL
        and CONF_INVERT_ALPHA not in type_class.allow_config
    ):
        raise cv.Invalid("No alpha channel to invert")
    
    if value.get(CONF_BYTE_ORDER) is not None and not callable(
        getattr(type_class, "set_big_endian", None)
    ):
        raise cv.Invalid(
            f"Image format '{conf_type}' does not support byte order configuration"
        )
    
    if file := value.get(CONF_FILE):
        file = Path(file)
        if is_svg_file(file):
            validate_cairosvg_installed()
        else:
            try:
                Image.open(file)
            except UnidentifiedImageError as exc:
                raise cv.Invalid(
                    f"File can't be opened as image: {file.absolute()}"
                ) from exc
    return value


# Schema for individual image
IMAGE_SCHEMA = cv.All(
    cv.Schema({
        cv.Required(CONF_ID): cv.declare_id(Image_),
        cv.Required(CONF_FILE): cv.Any(validate_file_shorthand, TYPED_FILE_SCHEMA),
        cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
        cv.Optional(CONF_TYPE): validate_type(IMAGE_TYPE),
        cv.Optional(CONF_RESIZE): cv.dimensions,
        cv.Optional(CONF_DITHER, default="NONE"): cv.one_of("NONE", "FLOYDSTEINBERG", upper=True),
        cv.Optional(CONF_INVERT_ALPHA, default=False): cv.boolean,
        cv.Optional(CONF_BYTE_ORDER): cv.one_of("BIG_ENDIAN", "LITTLE_ENDIAN", upper=True),
        cv.Optional(CONF_TRANSPARENCY, default=CONF_OPAQUE): validate_transparency(),
    }),
    validate_image_config,
)

# Schema for defaults
DEFAULTS_SCHEMA = cv.Schema({
    cv.Optional(CONF_TYPE): validate_type(IMAGE_TYPE),
    cv.Optional(CONF_RESIZE): cv.dimensions,
    cv.Optional(CONF_DITHER, default="NONE"): cv.one_of("NONE", "FLOYDSTEINBERG", upper=True),
    cv.Optional(CONF_INVERT_ALPHA, default=False): cv.boolean,
    cv.Optional(CONF_BYTE_ORDER): cv.one_of("BIG_ENDIAN", "LITTLE_ENDIAN", upper=True),
    cv.Optional(CONF_TRANSPARENCY, default=CONF_OPAQUE): validate_transparency(),
})


def apply_defaults_to_image(defaults, image_config):
    """
    Apply default values to an image configuration
    """
    result = {}
    
    # Apply defaults first
    for key, value in defaults.items():
        result[key] = value
    
    # Then apply specific image config
    for key, value in image_config.items():
        result[key] = value
    
    return result


def process_image_config(config):
    """
    Process image configuration by applying defaults and validating
    """
    if isinstance(config, dict) and CONF_DEFAULTS in config and CONF_IMAGES in config:
        # Format with defaults and images
        defaults = config[CONF_DEFAULTS]
        processed_images = []
        
        for image in config[CONF_IMAGES]:
            # Create new configuration by merging defaults and image
            final_config = apply_defaults_to_image(defaults, image)
            
            # Ensure that required values are present
            if CONF_TYPE not in final_config:
                raise cv.Invalid("Type is required either in defaults or in image config")
            
            processed_images.append(final_config)
        
        return processed_images
    else:
        # Simple format - list of images or single image
        if isinstance(config, list):
            return config
        else:
            return [config]


# Main configuration schema
CONFIG_SCHEMA = cv.Any(
    # Format with defaults and images
    cv.Schema({
        cv.Required(CONF_DEFAULTS): DEFAULTS_SCHEMA,
        cv.Required(CONF_IMAGES): cv.ensure_list(IMAGE_SCHEMA),
    }),
    # Simple format - list of images
    cv.ensure_list(cv.All(
        IMAGE_SCHEMA.extend({
            cv.Required(CONF_TYPE): validate_type(IMAGE_TYPE),
        }),
        validate_image_config
    )),
    # Single image
    cv.All(
        IMAGE_SCHEMA.extend({
            cv.Required(CONF_TYPE): validate_type(IMAGE_TYPE),
        }),
        validate_image_config
    )
)


async def write_image(config, all_frames=False):
    # Special case: source = sd_card → image read at runtime, not at compile time
    if config.get(CONF_SOURCE) == SOURCE_SD_CARD:
        _LOGGER.info(f"Skipping compile-time processing for SD card image: {config[CONF_FILE]}")
        return None, None, None, None, None, None

    path = Path(config[CONF_FILE])
    if not path.is_file():
        raise core.EsphomeError(f"Could not load image file {path}")

    resize = config.get(CONF_RESIZE)
    if is_svg_file(path):
        # Local import so use of non-SVG files needn't require cairosvg installed
        from cairosvg import svg2png

        if not resize:
            resize = (None, None)
        with open(path, "rb") as file:
            image = svg2png(
                file_obj=file,
                output_width=resize[0],
                output_height=resize[1],
            )
        image = Image.open(io.BytesIO(image))
        width, height = image.size
    else:
        image = Image.open(path)
        width, height = image.size
        if resize:
            # Preserve aspect ratio
            new_width_max = min(width, resize[0])
            new_height_max = min(height, resize[1])
            ratio = min(new_width_max / width, new_height_max / height)
            width, height = int(width * ratio), int(height * ratio)

    if not resize and (width > 500 or height > 500):
        _LOGGER.warning(
            'The image "%s" you requested is very big. Please consider'
            " using the resize parameter.",
            path,
        )

    dither = (
        Image.Dither.NONE
        if config.get(CONF_DITHER, "NONE") == "NONE"
        else Image.Dither.FLOYDSTEINBERG
    )
    type = config[CONF_TYPE]
    transparency = config.get(CONF_TRANSPARENCY, CONF_OPAQUE)
    invert_alpha = config.get(CONF_INVERT_ALPHA, False)
    frame_count = 1
    if all_frames:
        try:
            frame_count = image.n_frames
        except AttributeError:
            pass
        if frame_count <= 1:
            _LOGGER.warning("Image file %s has no animation frames", path)

    total_rows = height * frame_count
    encoder = IMAGE_TYPE[type](width, total_rows, transparency, dither, invert_alpha)
    if byte_order := config.get(CONF_BYTE_ORDER):
        encoder.set_big_endian(byte_order == "BIG_ENDIAN")

    for frame_index in range(frame_count):
        image.seek(frame_index)
        pixels = encoder.convert(image.resize((width, height)), path).getdata()
        for row in range(height):
            for col in range(width):
                encoder.encode(pixels[row * width + col])
            encoder.end_row()

    rhs = [HexInt(x) for x in encoder.data]
    prog_arr = cg.progmem_array(config[CONF_RAW_DATA_ID], rhs)
    image_type = get_image_type_enum(type)
    trans_value = get_transparency_enum(encoder.transparency)

    return prog_arr, width, height, image_type, trans_value, frame_count


async def to_code(config):
    # Process configuration
    processed_images = process_image_config(config)
    
    for entry in processed_images:
        # Check if it's an SD card image
        if entry.get(CONF_SOURCE) == SOURCE_SD_CARD:
            # Add dependency to sd_mmc_card component
            cg.add_define("USE_SD_MMC_CARD")
            
            # Create SDCardImage instance
            var = cg.new_Pvariable(
                entry[CONF_ID],
                entry[CONF_FILE],  # path on SD
                get_image_type_enum(entry[CONF_TYPE]),
                get_transparency_enum(entry.get(CONF_TRANSPARENCY, CONF_OPAQUE))
            )
            
            # Configure options if present
            if CONF_RESIZE in entry:
                cg.add(var.set_resize(entry[CONF_RESIZE][0], entry[CONF_RESIZE][1]))
            if entry.get(CONF_DITHER) == "FLOYDSTEINBERG":
                cg.add(var.set_dither(True))
            if entry.get(CONF_INVERT_ALPHA, False):
                cg.add(var.set_invert_alpha(True))
            if byte_order := entry.get(CONF_BYTE_ORDER):
                cg.add(var.set_big_endian(byte_order == "BIG_ENDIAN"))
        else:
            # Normal image (local, web, mdi, etc.)
            prog_arr, width, height, image_type, trans_value, _ = await write_image(entry)
            cg.new_Pvariable(
                entry[CONF_ID], prog_arr, width, height, image_type, trans_value
            )
