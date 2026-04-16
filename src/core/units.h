// units.h — Explicit unit system for oreo-kernel.
//
// All dimensions in the kernel have explicit units. The UnitSystem
// defines the document units (what users see) and kernel units
// (what OCCT uses internally), with conversion functions.
//
// OCCT uses millimeters for length and radians for angles internally.
// The document may use inches, degrees, etc.

#ifndef OREO_UNITS_H
#define OREO_UNITS_H

#include "thread_safety.h"

#include <cmath>
#include <optional>
#include <string>

namespace oreo {

// ─── Unit enums ──────────────────────────────────────────────

enum class LengthUnit {
    Meter,
    Millimeter,
    Centimeter,
    Micrometer,
    Inch,
    Foot,
};

enum class AngleUnit {
    Radian,
    Degree,
};

// ─── Conversion helpers (compile-time where possible) ────────

namespace unit_convert {

// Length → millimeters (OCCT internal unit)
constexpr double lengthToMM(double value, LengthUnit from) {
    switch (from) {
        case LengthUnit::Meter:      return value * 1000.0;
        case LengthUnit::Millimeter: return value;
        case LengthUnit::Centimeter: return value * 10.0;
        case LengthUnit::Micrometer: return value * 0.001;
        case LengthUnit::Inch:       return value * 25.4;
        case LengthUnit::Foot:       return value * 304.8;
    }
    // Unreachable: all enum values handled above.
    // If a new enum value is added, this fail-open return
    // will be caught by -Wswitch compiler warnings.
    return value;  // NOLINT: fail-open safeguard
}

// Millimeters → target unit
constexpr double mmToLength(double valueMM, LengthUnit to) {
    switch (to) {
        case LengthUnit::Meter:      return valueMM * 0.001;
        case LengthUnit::Millimeter: return valueMM;
        case LengthUnit::Centimeter: return valueMM * 0.1;
        case LengthUnit::Micrometer: return valueMM * 1000.0;
        case LengthUnit::Inch:       return valueMM / 25.4;
        case LengthUnit::Foot:       return valueMM / 304.8;
    }
    // Unreachable: all enum values handled above.
    // If a new enum value is added, this fail-open return
    // will be caught by -Wswitch compiler warnings.
    return valueMM;  // NOLINT: fail-open safeguard
}

// Angle → radians (OCCT internal unit)
constexpr double angleToRad(double value, AngleUnit from) {
    switch (from) {
        case AngleUnit::Radian: return value;
        case AngleUnit::Degree: return value * 3.14159265358979323846 / 180.0;
    }
    // Unreachable: all enum values handled above.
    // If a new enum value is added, this fail-open return
    // will be caught by -Wswitch compiler warnings.
    return value;  // NOLINT: fail-open safeguard
}

// Radians → target unit
constexpr double radToAngle(double valueRad, AngleUnit to) {
    switch (to) {
        case AngleUnit::Radian: return valueRad;
        case AngleUnit::Degree: return valueRad * 180.0 / 3.14159265358979323846;
    }
    // Unreachable: all enum values handled above.
    // If a new enum value is added, this fail-open return
    // will be caught by -Wswitch compiler warnings.
    return valueRad;  // NOLINT: fail-open safeguard
}

// Length-to-length conversion
constexpr double convertLength(double value, LengthUnit from, LengthUnit to) {
    if (from == to) return value;
    return mmToLength(lengthToMM(value, from), to);
}

// Angle-to-angle conversion
constexpr double convertAngle(double value, AngleUnit from, AngleUnit to) {
    if (from == to) return value;
    return radToAngle(angleToRad(value, from), to);
}

// Unit name strings
const char* lengthUnitName(LengthUnit unit);
const char* angleUnitName(AngleUnit unit);

// Parse from string — FAIL-CLOSED: throws std::invalid_argument on unknown
LengthUnit parseLengthUnit(const std::string& name);
AngleUnit parseAngleUnit(const std::string& name);

// Try-parse variants — return nullopt on unknown (no throw)
std::optional<LengthUnit> tryParseLengthUnit(const std::string& name);
std::optional<AngleUnit> tryParseAngleUnit(const std::string& name);

} // namespace unit_convert

// ─── UnitSystem ──────────────────────────────────────────────

struct OREO_IMMUTABLE UnitSystem {
    // Document units — what the user sees and inputs
    LengthUnit documentLength = LengthUnit::Millimeter;
    AngleUnit documentAngle = AngleUnit::Degree;

    // Kernel internal units — what OCCT uses
    // These are fixed: OCCT always uses MM + radians.
    static constexpr LengthUnit kernelLength = LengthUnit::Millimeter;
    static constexpr AngleUnit kernelAngle = AngleUnit::Radian;

    // Convert document length to kernel length
    double toKernelLength(double docValue) const {
        return unit_convert::convertLength(docValue, documentLength, kernelLength);
    }

    // Convert kernel length to document length
    double fromKernelLength(double kernelValue) const {
        return unit_convert::convertLength(kernelValue, kernelLength, documentLength);
    }

    // Convert document angle to kernel angle (radians)
    double toKernelAngle(double docValue) const {
        return unit_convert::convertAngle(docValue, documentAngle, kernelAngle);
    }

    // Convert kernel angle (radians) to document angle
    double fromKernelAngle(double kernelValue) const {
        return unit_convert::convertAngle(kernelValue, kernelAngle, documentAngle);
    }

    // STEP unit conversion (STEP files may have different units)
    double toStepLength(double kernelValue, LengthUnit stepUnit) const {
        return unit_convert::convertLength(kernelValue, kernelLength, stepUnit);
    }
    double fromStepLength(double stepValue, LengthUnit stepUnit) const {
        return unit_convert::convertLength(stepValue, stepUnit, kernelLength);
    }

    // Checked conversion — rejects NaN/Inf (throws std::invalid_argument).
    // Use these in subsystems that don't validate via requirePositive first.
    double toKernelLengthChecked(double docValue) const;
    double toKernelAngleChecked(double docValue) const;
};

} // namespace oreo

#endif // OREO_UNITS_H
