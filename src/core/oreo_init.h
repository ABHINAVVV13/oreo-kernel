// oreo_init.h — LEGACY REDIRECT. Use KernelContext::create() and KernelContext::initOCCT().
#ifndef OREO_INIT_H
#define OREO_INIT_H
#ifdef _MSC_VER
#pragma message("oreo_init.h is deprecated; include \"kernel_context.h\" directly.")
#elif defined(__GNUC__) || defined(__clang__)
#warning "oreo_init.h is deprecated; include \"kernel_context.h\" directly."
#endif
#include "kernel_context.h"
#endif
