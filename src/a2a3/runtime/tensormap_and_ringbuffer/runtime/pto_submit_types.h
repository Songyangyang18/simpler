/**
 * PTO Submit Types - Shared submit-contract definitions
 *
 * Header-only definitions shared by orchestration-facing and runtime-facing
 * headers. Keeps orchestration slim (no dependency on pto_runtime2_types.h).
 */

#ifndef PTO_SUBMIT_TYPES_H
#define PTO_SUBMIT_TYPES_H

#include <stdint.h>

inline constexpr int32_t INVALID_KERNEL_ID = -1;

/**
 * Subtask slot count: AIC, AIV0, AIV1
 */
inline constexpr int32_t PTO2_SUBTASK_SLOT_COUNT = 3;

/**
 * Subtask slot indices
 */
enum class PTO2SubtaskSlot : uint8_t {
    AIC  = 0,
    AIV0 = 1,
    AIV1 = 2,
};

/**
 * Subtask mask bits (for active_mask / subtask_done_mask)
 */
inline constexpr uint8_t PTO2_SUBTASK_MASK_AIC  = (1u << 0);  // 0x1
inline constexpr uint8_t PTO2_SUBTASK_MASK_AIV0 = (1u << 1);  // 0x2
inline constexpr uint8_t PTO2_SUBTASK_MASK_AIV1 = (1u << 2);  // 0x4

/**
 * Test whether a subtask slot is active in a given mask
 */
static inline bool pto2_subtask_active(uint8_t mask, PTO2SubtaskSlot slot) {
    return (mask & (1u << static_cast<uint8_t>(slot))) != 0;
}

/**
 * Mixed-task submit contract.
 *
 * Each field holds either a valid kernel ID or INVALID_KERNEL_ID (inactive).
 * At least one slot must be valid.
 */
struct MixedKernels {
    int32_t aic_kernel_id{INVALID_KERNEL_ID};
    int32_t aiv0_kernel_id{INVALID_KERNEL_ID};
    int32_t aiv1_kernel_id{INVALID_KERNEL_ID};
};

/**
 * Queue type for new warp-based scheduling.
 * 4 types: AIC, AIV, 1C1V warp, 1C2V warp
 */
enum class PTO2QueueType : uint8_t {
    AIC         = 0,   // Standard AIC task
    AIV         = 1,   // Standard AIV task (single AIV)
    AIV_X2      = 2,   // Dual AIV task (AIV0 + AIV1)
    WRAP_1C1V   = 3,   // 1C1V Warp task
    WRAP_1C2V   = 4    // 1C2V Warp task
};

inline constexpr int32_t PTO2_NUM_QUEUE_TYPES = 5;

/**
 * Warp type classification
 */
enum class PTO2WarpType : uint8_t {
    NONE   = 0,   // Not a warp task
    C1V1   = 1,   // 1 AIC + 1 AIV
    C1V2   = 2    // 1 AIC + 2 AIV
};

inline constexpr int32_t PTO2_MAX_WRAPS = 64;
inline constexpr int32_t PTO2_MAX_AICORES = 24;
inline constexpr int32_t PTO2_MAX_AIVCORES = 48;

/**
 * Derive queue type from active_mask and warp_id.
 * warp_id >= 0 indicates a warp task, otherwise standard task.
 */
static inline PTO2QueueType pto2_get_queue_type(uint8_t active_mask, int8_t warp_id) {
    (void)warp_id;
    bool has_aic = (active_mask & PTO2_SUBTASK_MASK_AIC) != 0;
    bool has_aiv0 = (active_mask & PTO2_SUBTASK_MASK_AIV0) != 0;
    bool has_aiv1 = (active_mask & PTO2_SUBTASK_MASK_AIV1) != 0;
    
    if (has_aic && has_aiv1) return PTO2QueueType::WRAP_1C2V;
    if (has_aic && has_aiv0) return PTO2QueueType::WRAP_1C1V;
    if (has_aiv0 && has_aiv1) return PTO2QueueType::AIV_X2;
    if (has_aic) return PTO2QueueType::AIC;
    return PTO2QueueType::AIV;
}

/**
 * Derive warp type from active_mask.
 */
static inline PTO2WarpType pto2_get_warp_type(uint8_t active_mask) {
    bool has_aic = (active_mask & PTO2_SUBTASK_MASK_AIC) != 0;
    int aiv_count = ((active_mask & PTO2_SUBTASK_MASK_AIV0) != 0)
                  + ((active_mask & PTO2_SUBTASK_MASK_AIV1) != 0);
    
    if (!has_aic || aiv_count == 0) return PTO2WarpType::NONE;
    return (aiv_count == 1) ? PTO2WarpType::C1V1 : PTO2WarpType::C1V2;
}

/**
 * Compute active_mask from MixedKernels.
 */
static inline uint8_t pto2_mixed_kernels_to_active_mask(const MixedKernels& mk) {
    uint8_t mask = 0;
    if (mk.aic_kernel_id  != INVALID_KERNEL_ID) mask |= PTO2_SUBTASK_MASK_AIC;
    if (mk.aiv0_kernel_id != INVALID_KERNEL_ID) mask |= PTO2_SUBTASK_MASK_AIV0;
    if (mk.aiv1_kernel_id != INVALID_KERNEL_ID) mask |= PTO2_SUBTASK_MASK_AIV1;
    return mask;
}

#endif // PTO_SUBMIT_TYPES_H
