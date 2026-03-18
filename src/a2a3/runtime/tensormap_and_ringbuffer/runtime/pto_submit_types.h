/**
 * PTO Submit Types - Shared submit-contract definitions
 *
 * Header-only definitions shared by orchestration-facing and runtime-facing
 * headers. Keeps orchestration slim (no dependency on pto_runtime2_types.h).
 *
 * SIMPLIFIED VERSION: No more mixed tasks. Each task executes one kernel on one core type.
 */

#ifndef PTO_SUBMIT_TYPES_H
#define PTO_SUBMIT_TYPES_H

#include <stdint.h>
#include "common/core_type.h"

inline constexpr int32_t INVALID_KERNEL_ID = -1;

inline constexpr int32_t PTO2_NUM_CORE_TYPES = 2;

#endif // PTO_SUBMIT_TYPES_H
