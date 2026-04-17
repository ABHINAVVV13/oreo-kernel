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
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace oreo {

// ─── Pi (UN-7) ───────────────────────────────────────────────
// C++17 does not have std::numbers::pi. Centralize the literal so
// no call site ever drifts from this value. Matches M_PI to the
// last representable bit of a double.
inline constexpr double kPi = 3.14159265358979323846;

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

// ─── Extra unit enums (UN-6) ─────────────────────────────────
// Minimal coverage for mass properties, stress analysis, torque.
// Kernel-internal "canonical" values are SI base units
// (kg, m^3, kg/m^3, Pa, N*m).

enum class MassUnit : uint8_t {
    Gram,
    Kilogram,
    Tonne,
    Pound,
    Ounce,
};

enum class VolumeUnit : uint8_t {
    Millimeter3,
    Centimeter3,
    Meter3,
    Inch3,
    Liter,
};

enum class DensityUnit : uint8_t {
    KgPerM3,
    GPerCm3,
};

enum class StressUnit : uint8_t {
    Pascal,
    Kilopascal,
    Megapascal,
    PSI,
};

enum class TorqueUnit : uint8_t {
    NewtonMeter,
    PoundFoot,
};

// ─── Conversion helpers (compile-time where possible) ────────

namespace unit_convert {

// ─── Overflow-safe multiply (UN-1) ───────────────────────────
// Multiplication of a finite value by a finite factor can produce
// Inf (e.g. lengthToMM(1e308, Meter) = 1e311 → +Inf). Silent Inf is
// a security/precision hazard — detect and throw instead.
// NOTE: not constexpr because std::isfinite is not constexpr until C++23.
inline double safeMultiply(double value, double factor) {
    double r = value * factor;
    if (!std::isfinite(r) && std::isfinite(value) && std::isfinite(factor)) {
        throw std::overflow_error("length/angle conversion overflow");
    }
    return r;
}

// Length → millimeters (OCCT internal unit)
inline double lengthToMM(double value, LengthUnit from) {
    switch (from) {
        case LengthUnit::Meter:      return safeMultiply(value, 1000.0);
        case LengthUnit::Millimeter: return value;
        case LengthUnit::Centimeter: return safeMultiply(value, 10.0);
        case LengthUnit::Micrometer: return safeMultiply(value, 0.001);
        case LengthUnit::Inch:       return safeMultiply(value, 25.4);
        case LengthUnit::Foot:       return safeMultiply(value, 304.8);
    }
    // UN-3: fail-closed on casted-int garbage.
    throw std::invalid_argument("unknown unit enum");
}

// Millimeters → target unit  (alias kept for back-compat callers)
inline double mmToLength(double valueMM, LengthUnit to) {
    switch (to) {
        case LengthUnit::Meter:      return safeMultiply(valueMM, 0.001);
        case LengthUnit::Millimeter: return valueMM;
        case LengthUnit::Centimeter: return safeMultiply(valueMM, 0.1);
        case LengthUnit::Micrometer: return safeMultiply(valueMM, 1000.0);
        case LengthUnit::Inch:       return safeMultiply(valueMM, 1.0 / 25.4);
        case LengthUnit::Foot:       return safeMultiply(valueMM, 1.0 / 304.8);
    }
    // UN-3: fail-closed on casted-int garbage.
    throw std::invalid_argument("unknown unit enum");
}

// UN-1: explicit "lengthFromMM" name pair with lengthToMM for symmetry.
// Identical to mmToLength but named consistently with lengthToMM.
inline double lengthFromMM(double valueMM, LengthUnit to) {
    return mmToLength(valueMM, to);
}

// Angle → radians (OCCT internal unit)
inline double angleToRad(double value, AngleUnit from) {
    switch (from) {
        case AngleUnit::Radian: return value;
        case AngleUnit::Degree: return safeMultiply(value, kPi / 180.0);
    }
    throw std::invalid_argument("unknown unit enum");
}

// Radians → target unit
inline double radToAngle(double valueRad, AngleUnit to) {
    switch (to) {
        case AngleUnit::Radian: return valueRad;
        case AngleUnit::Degree: return safeMultiply(valueRad, 180.0 / kPi);
    }
    throw std::invalid_argument("unknown unit enum");
}

// Symmetric alias for radToAngle — matches the "*FromRad" family name.
inline double angleFromRad(double valueRad, AngleUnit to) {
    return radToAngle(valueRad, to);
}

// Length-to-length conversion
inline double convertLength(double value, LengthUnit from, LengthUnit to) {
    if (from == to) return value;
    return mmToLength(lengthToMM(value, from), to);
}

// Angle-to-angle conversion
inline double convertAngle(double value, AngleUnit from, AngleUnit to) {
    if (from == to) return value;
    return radToAngle(angleToRad(value, from), to);
}

// ─── Extra-unit conversions (UN-6) ───────────────────────────
// Canonical "to SI base" converters. From-SI variants are derived
// via inverse factors; call sites can compose or use the bidi helpers.

// Mass → kilograms
inline double massToKg(double value, MassUnit from) {
    switch (from) {
        case MassUnit::Gram:     return safeMultiply(value, 0.001);
        case MassUnit::Kilogram: return value;
        case MassUnit::Tonne:    return safeMultiply(value, 1000.0);
        case MassUnit::Pound:    return safeMultiply(value, 0.45359237);
        case MassUnit::Ounce:    return safeMultiply(value, 0.028349523125);
    }
    throw std::invalid_argument("unknown unit enum");
}

// Kilograms → target mass unit
inline double kgToMass(double valueKg, MassUnit to) {
    switch (to) {
        case MassUnit::Gram:     return safeMultiply(valueKg, 1000.0);
        case MassUnit::Kilogram: return valueKg;
        case MassUnit::Tonne:    return safeMultiply(valueKg, 0.001);
        case MassUnit::Pound:    return safeMultiply(valueKg, 1.0 / 0.45359237);
        case MassUnit::Ounce:    return safeMultiply(valueKg, 1.0 / 0.028349523125);
    }
    throw std::invalid_argument("unknown unit enum");
}

// Volume → cubic meters
inline double volumeToM3(double value, VolumeUnit from) {
    switch (from) {
        case VolumeUnit::Millimeter3: return safeMultiply(value, 1e-9);
        case VolumeUnit::Centimeter3: return safeMultiply(value, 1e-6);
        case VolumeUnit::Meter3:      return value;
        case VolumeUnit::Inch3:       return safeMultiply(value, 1.6387064e-5);
        case VolumeUnit::Liter:       return safeMultiply(value, 1e-3);
    }
    throw std::invalid_argument("unknown unit enum");
}

inline double m3ToVolume(double valueM3, VolumeUnit to) {
    switch (to) {
        case VolumeUnit::Millimeter3: return safeMultiply(valueM3, 1e9);
        case VolumeUnit::Centimeter3: return safeMultiply(valueM3, 1e6);
        case VolumeUnit::Meter3:      return valueM3;
        case VolumeUnit::Inch3:       return safeMultiply(valueM3, 1.0 / 1.6387064e-5);
        case VolumeUnit::Liter:       return safeMultiply(valueM3, 1e3);
    }
    throw std::invalid_argument("unknown unit enum");
}

// Density → kg/m^3
inline double densityToKgPerM3(double value, DensityUnit from) {
    switch (from) {
        case DensityUnit::KgPerM3:  return value;
        case DensityUnit::GPerCm3:  return safeMultiply(value, 1000.0);
    }
    throw std::invalid_argument("unknown unit enum");
}

inline double kgPerM3ToDensity(double valueKgM3, DensityUnit to) {
    switch (to) {
        case DensityUnit::KgPerM3:  return valueKgM3;
        case DensityUnit::GPerCm3:  return safeMultiply(valueKgM3, 0.001);
    }
    throw std::invalid_argument("unknown unit enum");
}

// Stress → Pascal
inline double stressToPa(double value, StressUnit from) {
    switch (from) {
        case StressUnit::Pascal:     return value;
        case StressUnit::Kilopascal: return safeMultiply(value, 1e3);
        case StressUnit::Megapascal: return safeMultiply(value, 1e6);
        case StressUnit::PSI:        return safeMultiply(value, 6894.757293168361);
    }
    throw std::invalid_argument("unknown unit enum");
}

inline double paToStress(double valuePa, StressUnit to) {
    switch (to) {
        case StressUnit::Pascal:     return valuePa;
        case StressUnit::Kilopascal: return safeMultiply(valuePa, 1e-3);
        case StressUnit::Megapascal: return safeMultiply(valuePa, 1e-6);
        case StressUnit::PSI:        return safeMultiply(valuePa, 1.0 / 6894.757293168361);
    }
    throw std::invalid_argument("unknown unit enum");
}

// Torque → Newton-meter
inline double torqueToNm(double value, TorqueUnit from) {
    switch (from) {
        case TorqueUnit::NewtonMeter: return value;
        case TorqueUnit::PoundFoot:   return safeMultiply(value, 1.3558179483314004);
    }
    throw std::invalid_argument("unknown unit enum");
}

inline double nmToTorque(double valueNm, TorqueUnit to) {
    switch (to) {
        case TorqueUnit::NewtonMeter: return valueNm;
        case TorqueUnit::PoundFoot:   return safeMultiply(valueNm, 1.0 / 1.3558179483314004);
    }
    throw std::invalid_argument("unknown unit enum");
}

// Unit name strings
const char* lengthUnitName(LengthUnit unit);
const char* angleUnitName(AngleUnit unit);

// Parse from string — FAIL-CLOSED: throws std::invalid_argument on unknown
LengthUnit parseLengthUnit(const std::string& name);
AngleUnit parseAngleUnit(const std::string& name);

// Try-parse variants (UN-4) — return nullopt on unknown (no throw).
// Prefer these in code paths that hold a KernelContext and can funnel
// unknown-unit failures into the diagnostics collector with context
// instead of unwinding through exceptions.
std::optional<LengthUnit> tryParseLengthUnit(const std::string& name);
std::optional<AngleUnit> tryParseAngleUnit(const std::string& name);

} // namespace unit_convert

// ─── UnitSystem ──────────────────────────────────────────────

struct OREO_IMMUTABLE UnitSystem {
    // Document units — what the user sees and inputs
    LengthUnit documentLength = LengthUnit::Millimeter;
    AngleUnit documentAngle = AngleUnit::Degree;

    // Default document-side units for mass-properties / analysis (UN-6).
    // Kernel-internal canonical values are always SI base units (kg, m^3, ...).
    MassUnit documentMass = MassUnit::Kilogram;
    VolumeUnit documentVolume = VolumeUnit::Meter3;
    DensityUnit documentDensity = DensityUnit::KgPerM3;
    StressUnit documentStress = StressUnit::Pascal;
    TorqueUnit documentTorque = TorqueUnit::NewtonMeter;

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

    // ─── Validating factory (UN-5) ───────────────────────────
    // Rejects casted-int garbage enum values. Use instead of struct
    // aggregate initialization when inputs come from untrusted sources
    // (deserializers, FFI boundaries, etc.).
    static UnitSystem make(LengthUnit len, AngleUnit ang);
    static UnitSystem make(LengthUnit len,
                           AngleUnit ang,
                           MassUnit mass,
                           VolumeUnit volume,
                           DensityUnit density,
                           StressUnit stress,
                           TorqueUnit torque);
    static bool isValid(const UnitSystem& u) noexcept;
};

} // namespace oreo

#endif // OREO_UNITS_H
