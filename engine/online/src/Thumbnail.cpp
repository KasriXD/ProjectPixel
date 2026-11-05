#include "px/online/Thumbnail.hpp"

#include "px/assets/Image.hpp"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>

namespace px::online {

namespace {
void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}
} // namespace

Result<std::vector<std::uint8_t>> makeThumbnail(std::span<const std::uint8_t> picture) {
    if (picture.size() > kMaxThumbnailUploadBytes) {
        return makeError(ErrorCode::LimitExceeded, "The picture is larger than 4 MiB");
    }
    auto image = assets::decodeImage(picture, "thumbnail", assets::ImageLimits{4096, 4096});
    if (!image) return image.error();
    if (image->width < 16 || image->height < 9) return makeError(ErrorCode::InvalidData, "The picture is too small");

    // The largest 16:9 rectangle in the middle of the picture.
    const double aspect = static_cast<double>(kThumbnailWidth) / kThumbnailHeight;
    double cropW = image->width;
    double cropH = cropW / aspect;
    if (cropH > image->height) {
        cropH = image->height;
        cropW = cropH * aspect;
    }
    const double x0 = (image->width - cropW) * 0.5;
    const double y0 = (image->height - cropH) * 0.5;
    const double scale = cropW / kThumbnailWidth; // source pixels per thumbnail pixel

    std::vector<std::uint8_t> out(static_cast<std::size_t>(kThumbnailWidth) * kThumbnailHeight * 4);
    const auto pixel = [&](int x, int y, int c) {
        x = std::clamp(x, 0, image->width - 1);
        y = std::clamp(y, 0, image->height - 1);
        return image->pixels[(static_cast<std::size_t>(y) * image->width + x) * 4 + c];
    };
    for (int ty = 0; ty < kThumbnailHeight; ++ty) {
        for (int tx = 0; tx < kThumbnailWidth; ++tx) {
            std::uint8_t* dst = &out[(static_cast<std::size_t>(ty) * kThumbnailWidth + tx) * 4];
            if (scale <= 1.0 || std::abs(scale - std::round(scale)) < 1e-6) {
                // Enlarging, or shrinking by a whole factor (pixel art): the
                // nearest pixel keeps edges sharp.
                const int sx = static_cast<int>(x0 + (tx + 0.5) * scale);
                const int sy = static_cast<int>(y0 + (ty + 0.5) * scale);
                for (int c = 0; c < 4; ++c) dst[c] = pixel(sx, sy, c);
                continue;
            }
            // Shrinking by other factors: the average of the covered pixels.
            const int sx0 = static_cast<int>(x0 + tx * scale);
            const int sy0 = static_cast<int>(y0 + ty * scale);
            const int sx1 = std::max(sx0 + 1, static_cast<int>(x0 + (tx + 1) * scale));
            const int sy1 = std::max(sy0 + 1, static_cast<int>(y0 + (ty + 1) * scale));
            unsigned sum[4] = {0, 0, 0, 0};
            unsigned n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    for (int c = 0; c < 4; ++c) sum[c] += pixel(sx, sy, c);
                    ++n;
                }
            }
            for (int c = 0; c < 4; ++c) dst[c] = static_cast<std::uint8_t>(sum[c] / n);
        }
    }
    std::vector<std::uint8_t> png;
    if (stbi_write_png_to_func(appendBytes, &png, kThumbnailWidth, kThumbnailHeight, 4, out.data(), kThumbnailWidth * 4) ==
            0 ||
        png.empty()) {
        return makeError(ErrorCode::InvalidData, "The thumbnail could not be written");
    }
    return png;
}

} // namespace px::online
