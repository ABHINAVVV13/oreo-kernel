// units.cpp — Unit name parsing and string conversion.
// Fail-closed: unknown unit strings throw, not silently default.

#include "units.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace oreo {
namespace unit_convert {

const char* lengthUnitName(LengthUnit unit) {
    switch (unit) {
        case LengthUnit::Meter:      return "m";
        case LengthUnit::Millimeter: return "mm";
        case LengthUnit::Centimeter: return "cm";
        case LengthUnit::Micrometer: return "um";
        case LengthUnit::Inch:       return "in";
        case LengthUnit::Foot:       return "ft";
    }
    return "mm";
}

const char* angleUnitName(AngleUnit unit) {
    switch (unit) {
        case AngleUnit::Radian: return "rad";
        case AngleUnit::Degree: return "deg";
    }
    return "rad";
}

// Fail-closed: returns nullopt for unknown strings.
std::optional<LengthUnit> tryParseLengthUnit(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "m" || lower == "meter" || lower == "meters") return LengthUnit::Meter;
    if (lower == "mm" || lower == "millimeter" || lower == "millimeters") return LengthUnit::Millimeter;
    if (lower == "cm" || lower == "centimeter" || lower == "centimeters") return LengthUnit::Centimeter;
    if (lower == "um" || lower == "micrometer" || lower == "micrometers") return LengthUnit::Micrometer;
    if (lower == "in" || lower == "inch" || lower == "inches") return LengthUnit::Inch;
    if (lower == "ft" || lower == "foot" || lower == "feet") return LengthUnit::Foot;
    return std::nullopt;
}

std::optional<AngleUnit> tryParseAngleUnit(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "rad" || lower == "radian" || lower == "radians") return AngleUnit::Radian;
    if (lower == "deg" || lower == "degree" || lower == "degrees") return AngleUnit::Degree;
    return std::nullopt;
}

LengthUnit parseLengthUnit(const std::string& name) {
    auto result = tryParseLengthUnit(name);
    if (!result.has_value()) {
        throw std::invalid_argument("Unknown length unit: '" + name + "'");
    }
    return *result;
}

AngleUnit parseAngleUnit(const std::string& name) {
    auto result = tryParseAngleUnit(name);
    if (!result.has_value()) {
        throw std::invalid_argument("Unknown angle unit: '" + name + "'");
    }
    return *result;
}

} // namespace unit_convert

// ── Checked unit conversions (reject NaN/Inf) ───────────────

double UnitSystem::toKernelLengthChecked(double docValue) const {
    if (!std::isfinite(docValue))
        throw std::invalid_argument("NaN or Inf in length conversion");
    return toKernelLength(docValue);
}

double UnitSystem::toKernelAngleChecked(double docValue) const {
    if (!std::isfinite(docValue))
        throw std::invalid_argument("NaN or Inf in angle conversion");
    return toKernelAngle(docValue);
}

} // namespace oreo
