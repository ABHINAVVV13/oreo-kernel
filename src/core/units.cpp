// units.cpp — Unit name parsing and string conversion.
// Fail-closed: unknown unit strings throw, not silently default.

#include "units.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace oreo {
namespace unit_convert {

// UN-2: ::tolower consults the current C locale. Under Turkish locales
// (tr_TR, az_AZ), the uppercase 'I' lowercases to 'ı' (U+0131) — which
// then fails every equality check against ASCII string literals below.
// A case-folder used for parsing must be locale-independent.
static char asciiTolower(char c) {
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

const char* lengthUnitName(LengthUnit unit) {
    switch (unit) {
        case LengthUnit::Meter:      return "m";
        case LengthUnit::Millimeter: return "mm";
        case LengthUnit::Centimeter: return "cm";
        case LengthUnit::Micrometer: return "um";
        case LengthUnit::Inch:       return "in";
        case LengthUnit::Foot:       return "ft";
    }
    // UN-3: fail-closed on casted-int garbage rather than silently
    // naming it "mm".
    throw std::invalid_argument("unknown unit enum");
}

const char* angleUnitName(AngleUnit unit) {
    switch (unit) {
        case AngleUnit::Radian: return "rad";
        case AngleUnit::Degree: return "deg";
    }
    throw std::invalid_argument("unknown unit enum");
}

// Fail-closed: returns nullopt for unknown strings.
std::optional<LengthUnit> tryParseLengthUnit(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), asciiTolower);
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
    std::transform(lower.begin(), lower.end(), lower.begin(), asciiTolower);
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

// ── Validating UnitSystem factory (UN-5) ────────────────────

// Validates that each enum field is one of the documented values.
// Casts from int (e.g. through a deserializer) can produce values
// outside the declared range; switch fall-through would then silently
// pick the default branch.
bool UnitSystem::isValid(const UnitSystem& u) noexcept {
    switch (u.documentLength) {
        case LengthUnit::Meter:
        case LengthUnit::Millimeter:
        case LengthUnit::Centimeter:
        case LengthUnit::Micrometer:
        case LengthUnit::Inch:
        case LengthUnit::Foot:
            break;
        default: return false;
    }
    switch (u.documentAngle) {
        case AngleUnit::Radian:
        case AngleUnit::Degree:
            break;
        default: return false;
    }
    switch (u.documentMass) {
        case MassUnit::Gram:
        case MassUnit::Kilogram:
        case MassUnit::Tonne:
        case MassUnit::Pound:
        case MassUnit::Ounce:
            break;
        default: return false;
    }
    switch (u.documentVolume) {
        case VolumeUnit::Millimeter3:
        case VolumeUnit::Centimeter3:
        case VolumeUnit::Meter3:
        case VolumeUnit::Inch3:
        case VolumeUnit::Liter:
            break;
        default: return false;
    }
    switch (u.documentDensity) {
        case DensityUnit::KgPerM3:
        case DensityUnit::GPerCm3:
            break;
        default: return false;
    }
    switch (u.documentStress) {
        case StressUnit::Pascal:
        case StressUnit::Kilopascal:
        case StressUnit::Megapascal:
        case StressUnit::PSI:
            break;
        default: return false;
    }
    switch (u.documentTorque) {
        case TorqueUnit::NewtonMeter:
        case TorqueUnit::PoundFoot:
            break;
        default: return false;
    }
    return true;
}

UnitSystem UnitSystem::make(LengthUnit len, AngleUnit ang) {
    UnitSystem u;
    u.documentLength = len;
    u.documentAngle = ang;
    if (!isValid(u)) throw std::invalid_argument("invalid UnitSystem");
    return u;
}

UnitSystem UnitSystem::make(LengthUnit len,
                            AngleUnit ang,
                            MassUnit mass,
                            VolumeUnit volume,
                            DensityUnit density,
                            StressUnit stress,
                            TorqueUnit torque) {
    UnitSystem u;
    u.documentLength = len;
    u.documentAngle = ang;
    u.documentMass = mass;
    u.documentVolume = volume;
    u.documentDensity = density;
    u.documentStress = stress;
    u.documentTorque = torque;
    if (!isValid(u)) throw std::invalid_argument("invalid UnitSystem");
    return u;
}

} // namespace oreo
