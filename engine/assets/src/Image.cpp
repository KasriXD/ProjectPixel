#include "px/assets/Image.hpp"

#include <climits>
#include <cstring>

#include <stb_image.h>

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

} // namespace px::assets
