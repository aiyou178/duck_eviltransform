#pragma once

#include "coord.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace eviltransform {
namespace detail {

class WKBTransformer {
public:
    WKBTransformer(const char *data, std::size_t size, Crs from, Crs to) : from(from), to(to) {
        if (!IsValidCrs(from) || !IsValidCrs(to)) {
            throw std::invalid_argument("Unsupported coordinate reference system");
        }
        if (data == nullptr || size < 5) {
            Fail("Truncated WKB geometry header");
        }
        output.assign(data, size);
    }

    std::string Run() {
        Geometry(1, 0, 0);
        if (position != output.size()) {
            Fail("Unexpected trailing bytes after WKB geometry");
        }
        return std::move(output);
    }

private:
    struct GeometryType {
        std::uint32_t base;
        unsigned dimensions;
        unsigned dimension_flags;
        bool has_srid;
    };

    std::string output;
    std::size_t position = 0;
    Crs from;
    Crs to;

    [[noreturn]] static void Fail(const char *message) {
        throw std::invalid_argument(message);
    }

    void Require(std::size_t count) const {
        if (count > output.size() - position) {
            Fail("Truncated WKB geometry payload");
        }
    }

    std::uint8_t ReadByte() {
        Require(1);
        return static_cast<std::uint8_t>(output[position++]);
    }

    std::uint32_t ReadUInt32(bool little_endian) {
        Require(4);
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const auto byte = static_cast<std::uint8_t>(output[position + i]);
            const unsigned shift = little_endian ? i * 8 : (3 - i) * 8;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        position += 4;
        return value;
    }

    void WriteUInt32(std::size_t offset, std::uint32_t value, bool little_endian) {
        for (unsigned i = 0; i < 4; ++i) {
            const unsigned shift = little_endian ? i * 8 : (3 - i) * 8;
            output[offset + i] = static_cast<char>((value >> shift) & 0xffU);
        }
    }

    double ReadDouble(std::size_t offset, bool little_endian) const {
        std::uint64_t bits = 0;
        for (unsigned i = 0; i < 8; ++i) {
            const auto byte = static_cast<std::uint8_t>(output[offset + i]);
            const unsigned shift = little_endian ? i * 8 : (7 - i) * 8;
            bits |= static_cast<std::uint64_t>(byte) << shift;
        }
        double value;
        static_assert(sizeof(value) == sizeof(bits), "WKB requires 64-bit doubles");
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void WriteDouble(std::size_t offset, double value, bool little_endian) {
        std::uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        for (unsigned i = 0; i < 8; ++i) {
            const unsigned shift = little_endian ? i * 8 : (7 - i) * 8;
            output[offset + i] = static_cast<char>((bits >> shift) & 0xffU);
        }
    }

    static GeometryType DecodeType(std::uint32_t raw_type) {
        constexpr std::uint32_t EWKB_Z = 0x80000000U;
        constexpr std::uint32_t EWKB_M = 0x40000000U;
        constexpr std::uint32_t EWKB_SRID = 0x20000000U;
        const auto numeric_type = raw_type & ~(EWKB_Z | EWKB_M | EWKB_SRID);
        const auto iso_dimensions = numeric_type / 1000U;
        const auto base = numeric_type % 1000U;
        if (iso_dimensions > 3 || base < 1 || base > 7) {
            Fail("Unsupported or invalid WKB geometry type");
        }

        // Flags use bit 0 for Z and bit 1 for M, matching ISO offsets.
        const unsigned ewkb_dimensions = ((raw_type & EWKB_Z) ? 1U : 0U) | ((raw_type & EWKB_M) ? 2U : 0U);
        if (iso_dimensions != 0 && ewkb_dimensions != 0 && iso_dimensions != ewkb_dimensions) {
            Fail("Conflicting ISO WKB and EWKB dimensionality");
        }
        const unsigned dimension_flags = iso_dimensions != 0 ? iso_dimensions : ewkb_dimensions;
        const unsigned dimensions = 2 + ((dimension_flags & 1U) ? 1U : 0U) +
                                    ((dimension_flags & 2U) ? 1U : 0U);
        return {base, dimensions, dimension_flags, (raw_type & EWKB_SRID) != 0};
    }

    std::uint32_t TargetSrid() const {
        switch (to) {
        case Crs::Wgs84:
            return 4326;
        case Crs::Gcj02:
            return 990001;
        case Crs::Bd09:
            return 990002;
        }
        Fail("Unsupported coordinate reference system");
    }

    void Coordinate(unsigned dimensions, bool little_endian) {
        const std::size_t stride = static_cast<std::size_t>(dimensions) * 8;
        Require(stride);
        double x = ReadDouble(position, little_endian);
        double y = ReadDouble(position + 8, little_endian);
        // WKB represents POINT EMPTY with NaN coordinates. Keep the original
        // NaN payloads and all untouched Z/M bytes, including signaling NaNs.
        if (from != to && !(std::isnan(x) && std::isnan(y))) {
            TransformXY(x, y, from, to);
            WriteDouble(position, x, little_endian);
            WriteDouble(position + 8, y, little_endian);
        }
        position += stride;
    }

    void CoordinateSequence(unsigned dimensions, bool little_endian) {
        const auto count = ReadUInt32(little_endian);
        const std::size_t stride = static_cast<std::size_t>(dimensions) * 8;
        // Dividing first also prevents count * stride from overflowing.
        if (count > (output.size() - position) / stride) {
            Fail("WKB coordinate count exceeds the available payload");
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            Coordinate(dimensions, little_endian);
        }
    }

    void Geometry(unsigned depth, std::uint32_t expected_base, unsigned expected_dimension_flags) {
        if (depth > 128) {
            Fail("WKB nesting depth exceeds 128 geometries");
        }
        const auto byte_order = ReadByte();
        if (byte_order > 1) {
            Fail("Invalid WKB byte-order marker");
        }
        const bool little_endian = byte_order == 1;
        const auto type = DecodeType(ReadUInt32(little_endian));
        if (expected_base != 0 && type.base != expected_base) {
            Fail("Invalid WKB multi-geometry member type");
        }
        if (expected_base != 0 && type.dimension_flags != expected_dimension_flags) {
            Fail("Inconsistent WKB multi-geometry member dimensionality");
        }
        if (type.has_srid) {
            const auto srid_position = position;
            ReadUInt32(little_endian);
            WriteUInt32(srid_position, TargetSrid(), little_endian);
        }

        switch (type.base) {
        case 1: // Point
            Coordinate(type.dimensions, little_endian);
            return;
        case 2: // LineString
            CoordinateSequence(type.dimensions, little_endian);
            return;
        case 3: { // Polygon
            const auto ring_count = ReadUInt32(little_endian);
            if (ring_count > (output.size() - position) / 4) {
                Fail("WKB ring count exceeds the available payload");
            }
            for (std::uint32_t i = 0; i < ring_count; ++i) {
                CoordinateSequence(type.dimensions, little_endian);
            }
            return;
        }
        default: { // MultiPoint, MultiLineString, MultiPolygon, GeometryCollection
            const auto member_count = ReadUInt32(little_endian);
            if (member_count > (output.size() - position) / 5) {
                Fail("WKB geometry count exceeds the available payload");
            }
            const auto child_base = type.base == 7 ? 0U : type.base - 3;
            for (std::uint32_t i = 0; i < member_count; ++i) {
                Geometry(depth + 1, child_base, type.dimension_flags);
            }
            return;
        }
        }
    }
};

} // namespace detail

inline std::string TransformWKB(const char *data, std::size_t size, Crs from, Crs to) {
    return detail::WKBTransformer(data, size, from, to).Run();
}

} // namespace eviltransform
