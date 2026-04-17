// oreo_error.h — LEGACY REDIRECT. ErrorCode is now in diagnostic.h.
// This file exists only for backward compatibility with external code.
#ifndef OREO_ERROR_H
#define OREO_ERROR_H
#ifdef _MSC_VER
#pragma message("oreo_error.h is deprecated; include \"diagnostic.h\" directly.")
#elif defined(__GNUC__) || defined(__clang__)
#warning "oreo_error.h is deprecated; include \"diagnostic.h\" directly."
#endif
#include "diagnostic.h"
#endif
