# Submit by Cluster - Requirements and Main-Branch-Aligned Design

## 1. Goal

Define a single, main-branch-aligned specification for PTO2 cluster submission that combines:

1. Product requirements (what must be true).
2. Runtime design (how it is implemented on current main baseline).

The target model is: one submitted graph node is one `MixedTask`, and dispatch/completion is mixed-task-granular.

## 2. Background and Motivation

Future Ascend hardware is expected to provide stronger locality within an AICore cluster (`1 AIC + 2 AIV`).
The runtime therefore needs a "submit together, run together" model for related AIC/AIV kernels.

Legacy per-task submit (`kernel_id + worker_type`) cannot express atomic co-dispatch of multiple kernels to one cluster.

## 3. Scope

### In Scope

1. New orchestration-facing submit API for cluster-aware mixed submission.
2. Runtime/backend scheduler and executor changes to treat a mixed submit as one atomic scheduling unit.
3. Dependency gating, readiness, dispatch, completion, and reclamation at mixed-task granularity.
4. AIV slot equivalence (`AIV0` and `AIV1` are equivalent execution targets).

### Out of Scope

1. User-facing cluster pinning (`allocate_cluster/free_cluster`-style APIs).
2. New worker types beyond AIC/AIV.
3. Cross-cluster user placement policies.
4. Hardware topology changes beyond `1 AIC + 2 AIV` per cluster.

## 4. Main-Branch Baseline Constraints

Design must preserve the current main runtime architecture:

1. Multi-orchestrator runtime wiring (`orchestrators[]`, `orch_count`, thread-local `pto2_current_orch_idx`).
2. Executor threading split (orchestrator threads vs scheduler threads), and post-orchestrator transition (`transition_requested_` + `reassign_cores_for_all_threads()`).
3. Shared-memory hot/cold split (`PTO2TaskDescriptor` hot + `PTO2TaskPayload` cold).

## 5. Terminology

1. `cluster`: one physical unit with `1 AIC + 2 AIV`.
2. `Task`: one runtime graph node created by one submit call (simplified from MixedTask).
3. `kernel_id`: single kernel identifier for this task.
4. `core_type`: target core type (AIC or AIV) for this task.

## 6. API Contract

```cpp
inline constexpr int32_t INVALID_KERNEL_ID = -1;

// Submit task to specific core type
static inline void pto2_rt_submit_task(PTO2Runtime* rt,
                                       int32_t kernel_id,
                                       CoreType core_type,
                                       const PTOParam& params);

// Convenience wrappers
static inline void pto2_rt_submit_aic_task(PTO2Runtime* rt,
                                           int32_t kernel_id,
                                           const PTOParam& params);

static inline void pto2_rt_submit_aiv_task(PTO2Runtime* rt,
                                           int32_t kernel_id,
                                           const PTOParam& params);
```

Rules:

1. One submit call creates one `Task` with a single `kernel_id`.
2. Each task targets exactly one core type (AIC or AIV).
3. Wrappers are orchestration sugar only (inline in orchestration API); no dedicated runtime ops entries.
4. Submit-contract types are defined once in a shared header-only submit-types surface consumed by orchestration and runtime headers.
5. Invalid submits follow existing PTO2 behavior (`always_assert`), not a new recoverable return-code API.

## 7. Data Model (Requirements + Design)

`PTO2TaskDescriptor` (hot path) carries task identity/state:

1. `task_id` (single task ID)
2. `kernel_id` (single kernel ID, not array)
3. `core_type` (AIC or AIV)
4. dependency heads/counters and packed-buffer metadata

`PTO2TaskPayload` (cold path) carries:

1. shared params/tensors/scalars copied once per submit
2. fanin task IDs
3. other cold-path submit metadata

Producer identity in TensorMap is task ID end-to-end.

## 8. Scheduling Model

### 8.1 Ready Queue

Runtime uses a **single unified ready queue** (simplified from previous shape-based queues):

### 8.2 Independent Core Dispatch

1. Each task dispatches to a single core based on its `core_type`.
2. Batch scheduling: collect all ready tasks, dispatch by core type availability.
3. Local Buffer optimization: thread-local task buffer for local-first dispatch.
4. Overflow to global unified Ready Queue when local buffer is full.

### 8.3 Dependency and Completion

1. Fanin release/readiness remains dependency-correct and graph-level.
2. Single-stage completion: `on_task_complete(task_id)` handles fanout notification, fanin release, and self-consumption check in one pass.
3. Downstream release is triggered once per task completion.

## 9. Executor Ownership and Numbering

### 9.1 Canonical Flattened Numbering (Unchanged)

Given `block_dim` clusters:

1. AIC IDs: `[0, block_dim)`
2. AIV IDs: `[block_dim, 3 * block_dim)`
3. Cluster `i`: `{i, block_dim + i, 2 * block_dim + i}`

This project-defined flattened numbering is kept unchanged.

### 9.2 Cluster Ownership

1. One cluster must be owned by one scheduler domain/thread at a time.
2. No split-cluster ownership in either:
   - initial `assign_cores_to_threads()`
   - post-orchestrator `reassign_cores_for_all_threads()`
3. Lane occupancy bookkeeping must remain consistent with ownership after reassignment.

## 10. Functional Requirements

### 10.1 Valid Task Types

1. AIC task: `core_type = CoreType::AIC`
2. AIV task: `core_type = CoreType::AIV`

### 10.2 Runtime Behavior per Submit

1. Validate submit arguments.
2. Allocate task ID and initialize descriptor/payload once.
3. Build fanin/fanout at task granularity.
4. Enqueue to unified ready queue when ready.
5. Dispatch to available core matching core_type.
6. Single-stage completion triggers downstream release once.

## 11. Non-Functional Requirements

1. Correctness: no dependency violation, no partial mixed-task dispatch.
2. Determinism: dependency-correct ordering preserved; AIV lane choice may vary but remains semantically equivalent.
3. Fairness: resource-aware polling heuristic is allowed; strict starvation-free guarantee across all shapes is not required.
4. Performance: no obvious regression for non-cluster workflows.
5. Observability: lifecycle visibility for submit/ready/dispatch/block/complete.

## 12. Acceptance Criteria

Feature is accepted when:

1. Orchestration compiles and submits via `MixedKernels` API/wrappers.
2. Scheduler dispatches each mixed task as one cluster scheduling decision.
3. Dependencies gate mixed-task readiness correctly.
4. AIV execution remains cluster-local and semantically equivalent across lanes.
5. Existing non-cluster workflows continue to pass without behavior regression.
6. Cluster ownership is never split across scheduler domains before/after transition.

## 13. Verification Matrix

Recommended validation coverage:

1. Mapping correctness for cluster-to-core ID relation.
2. Dispatch to correct core type (AIC/AIV).
3. Dependency gating and single-stage completion.
4. Unified ready queue behavior.
5. Multi-orchestrator and core-transition ownership stability.
6. Invalid submit handling (`always_assert` path).
7. Regression coverage for existing examples/tests.

Milestone command (device):

```bash
python examples/scripts/run_example.py \
  -k tests/device_tests/tensormap_and_ringbuffer/batch_paged_attention/kernels \
  -g tests/device_tests/tensormap_and_ringbuffer/batch_paged_attention/golden.py \
  -p a2a3 -d 9
```

Final validation:

```bash
./ci.sh
```

## 14. Resolved Decisions

1. Legacy orchestration-facing single-task submit is replaced by mixed submit contract.
2. Invalid mixed submits fail with existing submit-time assert behavior.
3. Per-cluster concurrent capacity is lane-occupancy-driven, not a fixed constant.
4. Submit-contract types live in one shared header-only surface.
5. Resource-aware dispatch heuristics are allowed without a strict starvation-free guarantee.

