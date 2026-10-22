#pragma once

#include <cmath>
#include <stdexcept>
#include <utility>

namespace eviltransform {

enum class Crs { Wgs84, Gcj02, Bd09 };

namespace detail {

constexpr double PI = 3.141592653589793238462643383279502884;
constexpr double EARTH_R = 6378137.0;
constexpr double EE = 0.00669342162296594323;
constexpr double X_PI = PI * 3000.0 / 180.0;

inline bool IsValidCrs(Crs crs) {
    return crs == Crs::Wgs84 || crs == Crs::Gcj02 || crs == Crs::Bd09;
}

inline bool OutOfChina(double lat, double lng) {
    return lng < 72.004 || lng > 137.8347 || lat < 0.8293 || lat > 55.8271;
}

// The pair order in these internal functions follows the original PostgreSQL
// implementation: latitude first, longitude second.
inline std::pair<double, double> TransformDelta(double x, double y) {
    const double xy = x * y;
    const double abs_x = std::sqrt(std::abs(x));
    const double x_pi = x * PI;
    const double y_pi = y * PI;

    double d = 20.0 * std::sin(6.0 * x_pi) + 20.0 * std::sin(2.0 * x_pi);
    double lat = d;
    double lng = d;

    lat += 20.0 * std::sin(y_pi) + 40.0 * std::sin(y_pi / 3.0);
    lng += 20.0 * std::sin(x_pi) + 40.0 * std::sin(x_pi / 3.0);

    lat += 160.0 * std::sin(y_pi / 12.0) + 320.0 * std::sin(y_pi / 30.0);
    lng += 150.0 * std::sin(x_pi / 12.0) + 300.0 * std::sin(x_pi / 30.0);

    d = 2.0 / 3.0;
    lat *= d;
    lng *= d;

    lat += -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * xy + 0.2 * abs_x;
    lng += 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * xy + 0.1 * abs_x;
    return {lat, lng};
}

inline std::pair<double, double> Delta(double lat, double lng) {
    auto delta = TransformDelta(lng - 105.0, lat - 35.0);
    const double rad_lat = lat * (PI / 180.0);
    const double sin_lat = std::sin(rad_lat);
    const double magic = 1.0 - EE * sin_lat * sin_lat;
    const double sqrt_magic = std::sqrt(magic);

    delta.first = (delta.first * 180.0) / (((EARTH_R * (1.0 - EE)) / (magic * sqrt_magic)) * PI);
    delta.second = (delta.second * 180.0) / ((EARTH_R / sqrt_magic) * std::cos(rad_lat) * PI);
    return delta;
}

inline std::pair<double, double> Wgs2Gcj(double lat, double lng) {
    if (OutOfChina(lat, lng)) {
        return {lat, lng};
    }
    const auto delta = Delta(lat, lng);
    return {lat + delta.first, lng + delta.second};
}

inline std::pair<double, double> Gcj2Wgs(double lat, double lng) {
    if (OutOfChina(lat, lng)) {
        return {lat, lng};
    }
    const auto delta = Delta(lat, lng);
    return {lat - delta.first, lng - delta.second};
}

inline std::pair<double, double> Gcj2Bd(double lat, double lng) {
    if (OutOfChina(lat, lng)) {
        return {lat, lng};
    }
    const double z = std::hypot(lng, lat) + 0.00002 * std::sin(lat * X_PI);
    const double theta = std::atan2(lat, lng) + 0.000003 * std::cos(lng * X_PI);
    return {z * std::sin(theta) + 0.006, z * std::cos(theta) + 0.0065};
}

inline std::pair<double, double> Bd2Gcj(double lat, double lng) {
    if (OutOfChina(lat, lng)) {
        return {lat, lng};
    }
    const double x = lng - 0.0065;
    const double y = lat - 0.006;
    const double z = std::hypot(x, y) - 0.00002 * std::sin(y * X_PI);
    const double theta = std::atan2(y, x) - 0.000003 * std::cos(x * X_PI);
    return {z * std::sin(theta), z * std::cos(theta)};
}

} // namespace detail

// Public coordinates follow WKB and DuckDB: x is longitude, y is latitude.
// GCJ-02 -> WGS-84 intentionally retains the original approximate inverse.
inline void TransformXY(double &x, double &y, Crs from, Crs to) {
    if (!detail::IsValidCrs(from) || !detail::IsValidCrs(to)) {
        throw std::invalid_argument("Unsupported coordinate reference system");
    }
    if (from == to) {
        return;
    }

    std::pair<double, double> result;
    if (from == Crs::Wgs84) {
        result = detail::Wgs2Gcj(y, x);
        if (to == Crs::Bd09) {
            result = detail::Gcj2Bd(result.first, result.second);
        }
    } else if (from == Crs::Gcj02) {
        result = to == Crs::Wgs84 ? detail::Gcj2Wgs(y, x) : detail::Gcj2Bd(y, x);
    } else {
        result = detail::Bd2Gcj(y, x);
        if (to == Crs::Wgs84) {
            result = detail::Gcj2Wgs(result.first, result.second);
        }
    }
    x = result.second;
    y = result.first;
}

} // namespace eviltransform
