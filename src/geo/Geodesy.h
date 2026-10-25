#pragma once
// WGS84 ellipsoid helpers. All world positions are ECEF metres in double precision;
// rendering subtracts the camera position before converting to float.
#include <glm/glm.hpp>
#include <cmath>

namespace geo {

constexpr double kA = 6378137.0;                 // semi-major axis
constexpr double kF = 1.0 / 298.257223563;       // flattening
constexpr double kE2 = kF * (2.0 - kF);          // first eccentricity squared
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

struct LatLon {
    double lat = 0;  // degrees
    double lon = 0;  // degrees
};

inline glm::dvec3 toEcef(double latDeg, double lonDeg, double h) {
    double la = latDeg * kDeg, lo = lonDeg * kDeg;
    double s = std::sin(la), c = std::cos(la);
    double n = kA / std::sqrt(1.0 - kE2 * s * s);
    return {(n + h) * c * std::cos(lo), (n + h) * c * std::sin(lo), (n * (1.0 - kE2) + h) * s};
}

// Bowring's iteration; accurate to well below a millimetre near the surface.
inline void fromEcef(const glm::dvec3& p, double& latDeg, double& lonDeg, double& h) {
    double lon = std::atan2(p.y, p.x);
    double r = std::sqrt(p.x * p.x + p.y * p.y);
    double lat = std::atan2(p.z, r * (1.0 - kE2));
    for (int i = 0; i < 4; ++i) {
        double s = std::sin(lat);
        double n = kA / std::sqrt(1.0 - kE2 * s * s);
        h = r / std::cos(lat) - n;
        lat = std::atan2(p.z, r * (1.0 - kE2 * n / (n + h)));
    }
    double s = std::sin(lat);
    double n = kA / std::sqrt(1.0 - kE2 * s * s);
    h = (std::abs(lat) < 1.4) ? r / std::cos(lat) - n : p.z / s - n * (1.0 - kE2);
    latDeg = lat / kDeg;
    lonDeg = lon / kDeg;
}

// Local East-North-Up frame (columns) at a geodetic position.
struct Enu {
    glm::dvec3 east, north, up;
};

inline Enu enuFrame(double latDeg, double lonDeg) {
    double la = latDeg * kDeg, lo = lonDeg * kDeg;
    double sl = std::sin(la), cl = std::cos(la), so = std::sin(lo), co = std::cos(lo);
    return {{-so, co, 0.0}, {-sl * co, -sl * so, cl}, {cl * co, cl * so, sl}};
}

// Metres per degree at a latitude (for sizing tiles / sampling grids).
inline double metersPerDegLat(double latDeg) {
    double s = std::sin(latDeg * kDeg);
    return kPi / 180.0 * kA * (1.0 - kE2) / std::pow(1.0 - kE2 * s * s, 1.5);
}
inline double metersPerDegLon(double latDeg) {
    double s = std::sin(latDeg * kDeg);
    return kPi / 180.0 * kA * std::cos(latDeg * kDeg) / std::sqrt(1.0 - kE2 * s * s);
}

inline double wrapLon(double lon) {
    while (lon < -180.0) lon += 360.0;
    while (lon >= 180.0) lon -= 360.0;
    return lon;
}

}  // namespace geo
