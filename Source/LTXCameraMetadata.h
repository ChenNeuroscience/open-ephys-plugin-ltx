#pragma once

// Include after ProcessorHeaders.h or RecordingLib.h. Shared by the viewer and
// writer so metadata validation and crop geometry have one interpretation.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

namespace LTX {
struct CameraMetadata {
    bool present = false;
    bool hasWindow = false, hasPpm = false, hasCrop = false, hasFrame = false;
    double ppm = std::numeric_limits<double>::quiet_NaN();
    std::array<int32, 4> window{};
    std::array<int32, 2> frame{};
    std::string cropText;
    enum class Shape { None, Circle, Rectangle, Unknown };
    Shape shape = Shape::Unknown;
    double cx = 0, cy = 0, width = 0, height = 0, angleDegrees = 0;

    bool validWindow() const {
        return hasWindow && window[2] > window[0] && window[3] > window[1];
    }
    bool calibrated() const { return hasPpm && std::isfinite(ppm) && ppm > 0; }

    void parseCrop() {
        shape = Shape::Unknown;
        std::istringstream in(cropText);
        std::string format, extra;
        in >> format;
        if (format == "none()") {
            if (!(in >> extra)) shape = Shape::None;
            return;
        }
        if (format == "circle(cx,cy,r)") {
            double radius;
            if (!(in >> cx >> cy >> radius)) return;
            width = height = radius * 2;
            angleDegrees = 0;
        } else if (format == "square(cx,cy,w,deg)") {
            if (!(in >> cx >> cy >> width >> angleDegrees)) return;
            height = width;
        } else if (format == "rectangle(cx,cy,w,h,deg)") {
            if (!(in >> cx >> cy >> width >> height >> angleDegrees)) return;
        } else return;
        if ((in >> extra) || !std::isfinite(cx) || !std::isfinite(cy)
            || !std::isfinite(width) || !std::isfinite(height) || !std::isfinite(angleDegrees)
            || width <= 0 || height <= 0) return;
        shape = format == "circle(cx,cy,r)" ? Shape::Circle : Shape::Rectangle;
    }

    bool contains(double x, double y) const {
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        if (validWindow() && (x < window[0] || y < window[1] || x >= window[2] || y >= window[3])) return false;
        if (shape == Shape::Circle) return std::hypot(x - cx, y - cy) <= width / 2;
        if (shape == Shape::Rectangle) {
            const double radians = angleDegrees * 3.14159265358979323846 / 180;
            const double dx = x - cx, dy = y - cy;
            return std::abs(dx * std::cos(radians) + dy * std::sin(radians)) <= width / 2
                && std::abs(-dx * std::sin(radians) + dy * std::cos(radians)) <= height / 2;
        }
        return true;
    }
};

inline CameraMetadata readCameraMetadata(const DataStream* stream) {
    CameraMetadata result;
    if (!stream) return result;
    const int ppm = stream->findMetadata(MetadataDescriptor::DOUBLE, 1, "camera_tracker.pixels_per_metre");
    const int window = stream->findMetadata(MetadataDescriptor::INT32, 4, "camera_tracker.window");
    const int crop = stream->findMetadata(MetadataDescriptor::CHAR, 128, "camera_tracker.crop");
    const int frame = stream->findMetadata(MetadataDescriptor::INT32, 2, "camera_tracker.frame_size");
    result.present = ppm >= 0 || window >= 0 || crop >= 0 || frame >= 0;
    if (ppm >= 0 && stream->getMetadataValue(ppm)) {
        result.hasPpm = true;
        stream->getMetadataValue(ppm)->getValue<double>(result.ppm);
    }
    if (window >= 0 && stream->getMetadataValue(window)) {
        result.hasWindow = true;
        stream->getMetadataValue(window)->getValue<int32>(result.window.data());
    }
    if (crop >= 0 && stream->getMetadataValue(crop)) {
        // OE allocates length + 1 bytes for CHAR metadata and its pointer
        // overload copies that entire allocation (129 bytes for CHAR[128]).
        // The String overload reads at most the declared length instead.
        String text;
        stream->getMetadataValue(crop)->getValue(text);
        // Metadata belongs in a single header line, even for an unknown format.
        result.cropText = text.toStdString();
        for (char& c : result.cropText) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
        result.hasCrop = true;
        result.parseCrop();
    }
    if (frame >= 0 && stream->getMetadataValue(frame)) {
        result.hasFrame = true;
        stream->getMetadataValue(frame)->getValue<int32>(result.frame.data());
    }
    return result;
}

struct PositionEncoding {
    double originX = 0, originY = 0, scale = 1;
    bool transformed = false;

    explicit PositionEncoding(const CameraMetadata& metadata = {}) {
        if (metadata.validWindow()) {
            transformed = true;
            originX = metadata.window[0];
            originY = metadata.window[1];
            const double extent = std::max(double(metadata.window[2]) - originX,
                                           double(metadata.window[3]) - originY);
            scale = std::min(1.0, 1022.0 / extent);
        }
    }

    std::uint16_t coordinate(float value, bool x) const {
        if (!std::isfinite(value)) return 1023;
        const double output = (double(value) - (x ? originX : originY)) * scale;
        const double maximum = transformed ? 1022 : 65535;
        if (output < 0 || output > maximum) return 1023;
        // Retain legacy truncation for the old stream. New coordinates are
        // rounded after scaling, with 1023 reserved solely for missing data.
        return static_cast<std::uint16_t>(transformed ? std::llround(output) : output);
    }

    std::uint16_t area(float value) const {
        if (!std::isfinite(value) || value <= 0) return 0;
        // Areas stay actual camera pixels. uint16 saturation prevents wrapping.
        return static_cast<std::uint16_t>(std::min(65535.0, std::max(1.0, std::floor(double(value)))));
    }
};
}
