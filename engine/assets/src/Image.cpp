#include "px/assets/Image.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

#include <stb_image.h>
#include <stb_image_write.h>

namespace px::assets {

Result<Image> decodeImage(std::span<const std::uint8_t> bytes, std::string_view sourceName, ImageLimits limits) {
    if (bytes.empty()) {
        return makeError(ErrorCode::InvalidData, "{}: image file is empty", sourceName);
    }
    if (bytes.size() > static_cast<std::size_t>(INT_MAX)) {
        return makeError(ErrorCode::LimitExceeded, "{}: image file is too large", sourceName);
    }
    const auto size = static_cast<int>(bytes.size());

    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info_from_memory(bytes.data(), size, &width, &height, &channels) == 0) {
        return makeError(ErrorCode::InvalidData, "{}: not a supported image ({})", sourceName, stbi_failure_reason());
    }
    if (width <= 0 || height <= 0 || width > limits.maxWidth || height > limits.maxHeight) {
        return makeError(ErrorCode::LimitExceeded, "{}: image is {}x{}; the maximum is {}x{}", sourceName, width,
                         height, limits.maxWidth, limits.maxHeight);
    }

    stbi_uc* decoded = stbi_load_from_memory(bytes.data(), size, &width, &height, &channels, STBI_rgb_alpha);
    if (decoded == nullptr) {
        return makeError(ErrorCode::InvalidData, "{}: failed to decode image ({})", sourceName, stbi_failure_reason());
    }
    Image image;
    image.width = width;
    image.height = height;
    const std::size_t byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    image.pixels.resize(byteCount);
    std::memcpy(image.pixels.data(), decoded, byteCount);
    stbi_image_free(decoded);
    return image;
}

namespace {
void appendPng(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}
} // namespace

Result<std::vector<std::uint8_t>> encodePng(const Image& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.pixels.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u) {
        return makeError(ErrorCode::InvalidArgument, "an image of {}x{} needs {} bytes of pixels", image.width,
                         image.height, static_cast<std::size_t>(std::max(image.width, 0)) * std::max(image.height, 0) * 4u);
    }
    std::vector<std::uint8_t> png;
    if (stbi_write_png_to_func(appendPng, &png, image.width, image.height, 4, image.pixels.data(), image.width * 4) == 0 ||
        png.empty()) {
        return makeError(ErrorCode::InvalidData, "the PNG could not be written");
    }
    return png;
}

} // namespace px::assets
