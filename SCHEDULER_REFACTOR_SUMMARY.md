# 调度系统重构总结 - 实施完成

## 1. 修改目标

### 原始架构
- **MixedKernels任务结构**: 一个任务包含最多3个子任务（AIC + AIV0 + AIV1）
- **5种ResourceShape**: AIC_ONLY, AIV_X1, AIV_X2, AIC_AIV_X1, AIC_AIV_X2
- **5个Ready Queue**: 每种ResourceShape一个队列
- **2个Local Buffer**: AIC和AIV各一个
- **两阶段完成检测**: subtask完成 -> mixed task完成
- **Cluster原子分发**: 要求AIC+AIV原子分发到同一cluster

### 目标架构（已实施）
- **简化任务结构**: 每个任务只包含一个kernel（AIC或AIV）
- **1个统一的Ready Queue**: 所有ready task统一管理，分发时按CoreType分类
- **1个统一的Local Buffer**: 不再区分AIC/AIV
- **单阶段完成检测**: 移除两阶段完成，直接on_task_complete
- **独立Core分发**: 不再要求cluster原子分发，每个task独立分发到对应core

---

## 2. 已完成的修改

### 2.1 任务提交层 (pto_submit_types.h)
**文件**: `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_submit_types.h`

**修改内容**:
```cpp
// 修改前
struct MixedKernels {
    int32_t aic_kernel_id;
    int32_t aiv0_kernel_id;
    int32_t aiv1_kernel_id;
};
enum class PTO2ResourceShape { ... };
constexpr int PTO2_NUM_RESOURCE_SHAPES = 5;

// 修改后
enum class CoreType : uint8_t {
    AIC = 0,
    AIV = 1
};
constexpr int PTO2_NUM_CORE_TYPES = 2;
```

**关键变更**:
- 移除 `MixedKernels` 结构
- 移除 `PTO2ResourceShape` 枚举及相关常量
- 移除 `PTO2_SUBTASK_SLOT_COUNT`、`PTO2SubtaskSlot`、subtask mask常量
- 保留并使用 `CoreType` 枚举

### 2.2 任务描述层 (pto_runtime2_types.h)
**文件**: `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_runtime2_types.h`

**修改内容**:
```cpp
// PTO2TaskDescriptor
struct PTO2TaskDescriptor {
    PTO2TaskId task_id;        // 从 mixed_task_id 改名
    int32_t kernel_id;          // 从 kernel_id[3] 改为单一字段
    CoreType core_type;         // 新增字段
    void* packed_buffer_base;
    void* packed_buffer_end;
};

// PTO2TaskSlotState
struct PTO2TaskSlotState {
    // ... 其他字段不变
    CoreType core_type;         // 替代 active_mask
    // 移除 active_mask
    // 移除 subtask_done_mask
    uint8_t ring_id;
    int32_t dep_pool_mark;
};
```

**关键变更**:
- `PTO2TaskDescriptor.kernel_id`: 从数组改为单一字段
- `PTO2TaskDescriptor.task_id`: 从 `mixed_task_id` 改名
- `PTO2TaskSlotState.core_type`: 替代 `active_mask`
- 移除两阶段完成相关字段

### 2.3 调度器层 (pto_scheduler.h/cpp)
**文件**: `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_scheduler.{h,cpp}`

**修改内容**:
```cpp
// PTO2LocalReadyBuffer
struct PTO2LocalReadyBuffer {
    PTO2TaskSlotState** slot_states;
    int count;
    int capacity;
    // 方法不变，但不再按CoreType分buffer
};

// PTO2SchedulerState
struct PTO2SchedulerState {
    // ...
    PTO2ReadyQueue ready_queue;  // 单一统一的ready queue
};

// 关键方法修改
// 1. release_fanin_and_check_ready
bool release_fanin_and_check_ready(
    PTO2TaskSlotState& slot_state,
    PTO2LocalReadyBuffer* local_buf) {
    // 优先放入local_buf，满了再放全局ready_queue
    if (local_buf && local_buf->try_push(&slot_state)) {
        return true;
    }
    ready_queue.push(&slot_state);
}

// 2. on_task_complete (合并 on_subtask_complete + on_mixed_task_complete)
void on_task_complete(
    PTO2TaskSlotState& slot_state,
    PTO2LocalReadyBuffer* local_buf) {
    // 单一阶段完成，直接处理fanout通知
}

// 3. get_ready_task
PTO2TaskSlotState* get_ready_task(PTO2LocalReadyBuffer* local_buf) {
    // 优先从local_buf取，没有则从全局queue取
    if (local_buf && local_buf->count > 0) {
        return local_buf->pop();
    }
    return ready_queue.pop();
}
```

**关键变更**:
- Ready Queue从5个（按ResourceShape）改为1个统一queue
- Local Buffer从2个（AIC/AIV）合并为1个
- 移除两阶段完成检测（`on_subtask_complete` + `on_mixed_task_complete`）
- 新增单一阶段完成方法（`on_task_complete`）
- `release_fanin_and_check_ready`: 统一推送到一个queue
- `get_ready_task`: 从统一queue获取，返回任意ready task

**为什么使用单一Ready Queue而不是按CoreType分两个？**
- **批量调度需求**: 调度策略是"统计所有AIC/AIV idle数量，批量获取所有ready task"
- **简化逻辑**: 单一queue只需遍历一次，不需要合并多个queue的结果
- **概念一致**: 与单一Local Buffer设计理念一致
- **无优先级需求**: 如果不需要优先调度某一类型任务，分开存储没有优势

### 2.4 编排器层 (pto_orchestrator.h/cpp)
**文件**: `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_orchestrator.{h,cpp}`

**修改内容**:
```cpp
// API修改
// 修改前
void pto2_submit_mixed_task(
    PTO2OrchestratorState* orch,
    const MixedKernels& mixed_kernels,
    const PTOParam& params);

// 修改后
void pto2_submit_task(
    PTO2OrchestratorState* orch,
    int32_t kernel_id,
    CoreType core_type,
    const PTOParam& params);
```

**关键变更**:
- 函数名从 `pto2_submit_mixed_task` 改为 `pto2_submit_task`
- 参数从 `MixedKernels` 改为 `kernel_id + core_type`
- 移除 `active_mask` 计算和normalization逻辑
- 直接初始化 `core_type` 字段

### 2.5 运行时API层 (pto_runtime2.h/cpp, pto_orchestration_api.h)
**文件**: 
- `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_runtime2.{h,cpp}`
- `src/a2a3/runtime/tensormap_and_ringbuffer/orchestration/pto_orchestration_api.h`

**修改内容**:
```cpp
// PTO2RuntimeOps
struct PTO2RuntimeOps {
    void (*submit_task)(PTO2Runtime* rt, int32_t kernel_id, 
                        CoreType core_type, const PTOParam& params);
    // ... 其他方法不变
};

// 包装函数
static inline void pto2_rt_submit_task(
    PTO2Runtime* rt, int32_t kernel_id,
    CoreType core_type, const PTOParam& params) {
    rt->ops->submit_task(rt, kernel_id, core_type, params);
}

static inline void pto2_rt_submit_aic_task(
    PTO2Runtime* rt, int32_t kernel_id, const PTOParam& params) {
    rt->ops->submit_task(rt, kernel_id, CoreType::AIC, params);
}

static inline void pto2_rt_submit_aiv_task(
    PTO2Runtime* rt, int32_t kernel_id, const PTOParam& params) {
    rt->ops->submit_task(rt, kernel_id, CoreType::AIV, params);
}
```

**关键变更**:
- 更新 `PTO2RuntimeOps.submit_task` 签名
- 更新所有包装函数
- 保持便捷方法 `pto2_rt_submit_aic_task` / `pto2_rt_submit_aiv_task`

---

## 3. 需要后续修改的关键部分

### 3.1 aicpu_executor.cpp（最重要的修改）
**文件**: `src/a2a3/runtime/tensormap_and_ringbuffer/aicpu/aicpu_executor.cpp`

**需要修改的核心逻辑**:

#### 修改点1: check_running_cores_for_completion
```cpp
// 当前代码（两阶段完成）
bool mixed_complete = rt->scheduler.on_subtask_complete(slot_state, subslot);
if (mixed_complete) {
    rt->scheduler.on_mixed_task_complete(slot_state, local_bufs);
}

// 需要改为（单阶段完成）
rt->scheduler.on_task_complete(slot_state, local_buf);
```

#### 修改点2: Local Buffer初始化
```cpp
// 当前代码（两个buffer）
PTO2LocalReadyBuffer local_bufs[PTO2_LOCAL_DISPATCH_TYPE_NUM];  // [0]=AIC, [1]=AIV
local_bufs[0].reset(local_aic_ptrs, LOCAL_READY_CAP_PER_TYPE);
local_bufs[1].reset(local_aiv_ptrs, LOCAL_READY_CAP_PER_TYPE);

// 需要改为（一个buffer）
PTO2LocalReadyBuffer local_buf;
local_buf.reset(local_ptrs, LOCAL_READY_CAP);
```

#### 修改点3: Dispatch逻辑（核心修改）
```cpp
// 当前代码（Phase 2 + Phase 3，按ResourceShape分发）
// Phase 2: Local dispatch
for (int bi = 0; bi < PTO2_LOCAL_DISPATCH_TYPE_NUM; bi++) {
    while (local_bufs[bi].count > 0) {
        PTO2TaskSlotState* slot_state = local_bufs[bi].pop();
        PTO2ResourceShape shape = pto2_active_mask_to_shape(slot_state->active_mask);
        int32_t ci = tracker.find_cluster_for_shape(shape);
        // 分发到cluster的多个core...
    }
}

// Phase 3: Global dispatch
const PTO2ResourceShape* dispatch_order = get_dispatch_order(thread_idx);
for (int32_t si = 0; si < PTO2_NUM_RESOURCE_SHAPES; si++) {
    PTO2ResourceShape shape = dispatch_order[si];
    while (true) {
        int32_t ci = tracker.find_cluster_for_shape(shape);
        PTO2TaskSlotState* slot_state = pop_ready_task(shape, thread_idx);
        // 分发到cluster...
    }
}

// 需要改为（批量统计 + 独立分发）
// Phase 2: 统计idle cores
int32_t idle_aic_count = tracker.aic().idle_count;
int32_t idle_aiv_count = tracker.aiv().idle_count;

// Phase 3: 批量获取ready tasks（从local_buf和ready_queues）
PTO2TaskSlotState* ready_aic_tasks[MAX_IDLE_CORES];
PTO2TaskSlotState* ready_aiv_tasks[MAX_IDLE_CORES];

// 从local_buf获取
while (local_buf.count > 0) {
    PTO2TaskSlotState* task = local_buf.pop();
    if (task->core_type == CoreType::AIC) {
        if (aic_task_count < idle_aic_count) {
            ready_aic_tasks[aic_task_count++] = task;
        } else {
            rt->scheduler.requeue_ready_task(*task);
        }
    } else {
        if (aiv_task_count < idle_aiv_count) {
            ready_aiv_tasks[aiv_task_count++] = task;
        } else {
            rt->scheduler.requeue_ready_task(*task);
        }
    }
}

// 从ready_queue补充（遍历一次，按core_type分类）
while (true) {
    PTO2TaskSlotState* task = rt->scheduler.get_ready_task(nullptr);
    if (!task) break;
    if (task->core_type == CoreType::AIC && aic_task_count < idle_aic_count) {
        ready_aic_tasks[aic_task_count++] = task;
    } else if (task->core_type == CoreType::AIV && aiv_task_count < idle_aiv_count) {
        ready_aiv_tasks[aiv_task_count++] = task;
    } else {
        // 没有对应的idle core，放回队列
        rt->scheduler.requeue_ready_task(*task);
    }
}

// Phase 4: 批量分发
for (int32_t i = 0; i < aic_task_count; i++) {
    dispatch_to_aic_core(ready_aic_tasks[i]);
}
for (int32_t i = 0; i < aiv_task_count; i++) {
    dispatch_to_aiv_core(ready_aiv_tasks[i]);
}
```

#### 修改点4: 移除Cluster概念
```cpp
// 移除以下方法
- find_cluster_for_shape()
- get_dispatch_order()
- shape_resource_count()
- shape_name()

// 简化为
void dispatch_to_aic_core(PTO2TaskSlotState* task) {
    int32_t core_id = tracker.aic().idle[0];  // 取第一个idle的AIC core
    dispatch_subtask_to_core(runtime, tracker, executing_reg_task_ids,
        core_id, CoreType::AIC, *task);
}

void dispatch_to_aiv_core(PTO2TaskSlotState* task) {
    int32_t core_id = tracker.aiv().idle[0];  // 取第一个idle的AIV core
    dispatch_subtask_to_core(runtime, tracker, executing_reg_task_ids,
        core_id, CoreType::AIV, *task);
}
```

#### 修改点5: 执行状态追踪
```cpp
// 移除以下字段（不再需要）
- executing_subslot_by_core_[]  // 不再有subslot概念

// 保留字段
- executing_slot_state_by_core_[]  // 记录每个core正在执行的task
- executing_reg_task_ids_[]    // 记录每个core的register task id
```

#### 修改点6: perf_record填充
```cpp
// 修改前
int32_t perf_slot_idx = static_cast<int32_t>(executing_subslot_by_core_[core_id]);
record->func_id = slot_state.task->kernel_id[perf_slot_idx];

// 修改为
record->func_id = slot_state.task->kernel_id;
```

---

## 4. 数据流对比

### 修改前
```
提交MixedKernels 
-> 计算active_mask 
-> 确定ResourceShape 
-> 分配task slot 
-> 初始化kernel_id[3] 
-> 依赖解析 
-> 就绪时推送到shape-based ready queue
-> 分发时检查cluster空闲 
-> 原子分发到cluster的多个core
-> 子任务逐一完成 
-> 设置subtask_done_mask位
-> 全部完成 
-> on_mixed_task_complete 
-> 通知下游
```

### 修改后
```
提交(kernel_id, core_type) 
-> 分配task slot 
-> 初始化单一kernel_id和core_type
-> 依赖解析 
-> 就绪时推送到core_type-based ready queue
-> 分发时统计idle core数量 
-> 批量获取ready tasks
-> 按core_type独立分发到对应idle core
-> 任务完成 
-> on_task_complete 
-> 通知下游
```

---

## 5. 性能影响分析

### 性能优势
1. **减少内存占用**: TaskDescriptor和TaskSlotState结构体变小
2. **简化调度逻辑**: 移除两阶段完成检测和cluster匹配
3. **更好的并发性**: 不再要求AIC+AIV原子分发，可以独立调度
4. **更简单的分发逻辑**: 直接按core_type分发，无需复杂的shape匹配

### 潜在风险
1. **失去cluster locality**: 无法保证AIC和AIV任务在同一cluster上执行
2. **负载不均衡**: 如果AIC和AIV任务数量不均衡，可能导致某些core空闲
3. **缓存局部性**: cluster内shared memory的局部性优势可能减弱

---

## 6. 后续实施建议

### 步骤1: 修改aicpu_executor.cpp
这是最关键的修改，建议：
1. 先备份原始文件
2. 按照第3.1节的修改点逐步修改
3. 重点修改dispatch逻辑，实现批量统计和分发
4. 测试验证每个修改点

### 步骤2: 更新测试用例
需要修改所有调用 `pto2_rt_submit_task` 的测试代码：
```cpp
// 修改前
MixedKernels mk;
mk.aic_kernel_id = kernel_id;
pto2_rt_submit_task(rt, mk, params);

// 修改为
pto2_rt_submit_aic_task(rt, kernel_id, params);
```

### 步骤3: 更新文档
更新以下文档：
- `docs/SUBMIT_BY_CLUSTER.md` (可能需要废弃或更新)
- `docs/RUNTIME_LOGIC.md`
- 其他相关设计文档

### 步骤4: 性能测试
1. 对比修改前后的性能
2. 验证是否有性能回归
3. 测试极端情况（大量AIC任务、大量AIV任务）

---

## 7. 文件修改清单

### 已修改文件
1. ✅ `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_submit_types.h`
2. ✅ `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_runtime2_types.h`
3. ✅ `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_scheduler.h`
4. ✅ `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_scheduler.cpp`
5. ✅ `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_orchestrator.h`
6. ✅ `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_orchestrator.cpp`
7. ✅ `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_runtime2.h`
8. ✅ `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_runtime2.cpp`
9. ✅ `src/a2a3/runtime/tensormap_and_ringbuffer/orchestration/pto_orchestration_api.h`
10. ✅ `src/a2a3/runtime/tensormap_and_ringbuffer/aicpu/aicpu_executor.cpp` **(核心调度逻辑)**

### 待修改文件
1. ⏳ `src/a5/runtime/tensormap_and_ringbuffer/` 对应的a5平台文件
2. ⏳ 测试用例和示例代码
3. ⏳ 文档更新

---

## 8. 总结

本次重构已经完成了核心数据结构和API的修改，实现了：
1. **任务结构简化**: 从mixed-task改为单一task
2. **队列统一**: 从5个shape-based队列合并为1个统一队列
3. **完成检测简化**: 从两阶段改为单一阶段
4. **API更新**: 所有提交接口已更新

剩余最关键的工作是修改 `aicpu_executor.cpp` 中的调度和分发逻辑，实现：
1. 批量统计idle cores
2. 批量获取ready tasks
3. 按core_type独立分发

完成这些修改后，新的调度系统将大大简化代码逻辑，提高可维护性。

---

## 2. 核心修改

### 2.1 任务提交层 (pto_submit_types.h, pto_orchestrator.h/cpp)

**修改前**:
```cpp
struct MixedKernels {
    int32_t aic_kernel_id{INVALID_KERNEL_ID};
    int32_t aiv0_kernel_id{INVALID_KERNEL_ID};
    int32_t aiv1_kernel_id{INVALID_KERNEL_ID};
};

void pto2_submit_mixed_task(
    PTO2OrchestratorState* orch,
    const MixedKernels& mixed_kernels,
    const PTOParam& params);
```

**修改后**:
```cpp
// 每个任务只有一个kernel_id和对应的core_type
void pto2_submit_task(
    PTO2OrchestratorState* orch,
    int32_t kernel_id,
    CoreType core_type,  // AIC or AIV
    const PTOParam& params);
```

**影响范围**:
- 移除 `MixedKernels` 结构
- 移除 `PTO2ResourceShape` 枚举
- 移除 `active_mask` 相关逻辑
- 简化 `pto2_submit_mixed_task` 为 `pto2_submit_task`

### 2.2 任务描述层 (pto_runtime2_types.h)

**修改前**:
```cpp
struct PTO2TaskDescriptor {
    PTO2TaskId mixed_task_id;
    int32_t kernel_id[PTO2_SUBTASK_SLOT_COUNT];  // 3个槽位
    void* packed_buffer_base;
    void* packed_buffer_end;
};

struct PTO2TaskSlotState {
    uint8_t active_mask;
    std::atomic<uint8_t> subtask_done_mask;
    uint8_t ring_id;
    // ... 其他字段
};
```

**修改后**:
```cpp
struct PTO2TaskDescriptor {
    PTO2TaskId task_id;        // 简化为单一task_id
    int32_t kernel_id;          // 单一kernel_id
    CoreType core_type;         // AIC or AIV
    void* packed_buffer_base;
    void* packed_buffer_end;
};

struct PTO2TaskSlotState {
    // 移除 active_mask
    // 移除 subtask_done_mask
    CoreType core_type;         // 直接存储core_type
    uint8_t ring_id;
    // ... 其他字段保持不变
};
```

**影响范围**:
- `PTO2TaskDescriptor`: 简化kernel_id数组为单一字段
- `PTO2TaskSlotState`: 移除两阶段完成相关字段
- 移除 `PTO2_SUBTASK_SLOT_COUNT`、`PTO2_NUM_RESOURCE_SHAPES` 等常量

### 2.3 调度器层 (pto_scheduler.h/cpp)

**修改前**:
```cpp
struct PTO2SchedulerState {
    PTO2ReadyQueue ready_queues[PTO2_NUM_RESOURCE_SHAPES];  // 5个队列
    // ...
};

struct PTO2LocalReadyBuffer {
    PTO2TaskSlotState** slot_states;
    int count;
    int capacity;
};
// 每个调度线程有2个buffer: local_bufs[0]=AIC, local_bufs[1]=AIV
```

**修改后**:
```cpp
struct PTO2SchedulerState {
    PTO2ReadyQueue ready_queues[2];  // [0]=AIC, [1]=AIV
    // ...
};

struct PTO2LocalReadyBuffer {
    PTO2TaskSlotState** slot_states;
    int count;
    int capacity;
};
// 每个调度线程有1个统一的buffer
```

**关键函数修改**:

1. `release_fanin_and_check_ready()`:
   - 修改前: 根据 `active_mask` 计算 `ResourceShape`，推送到对应shape的queue
   - 修改后: 根据 `core_type` 直接推送到AIC或AIV queue

2. `on_subtask_complete()` + `on_mixed_task_complete()`:
   - 修改前: 两阶段完成检测
   - 修改后: 合并为单一的 `on_task_complete()`

3. `get_ready_task()`:
   - 修改前: 按ResourceShape获取
   - 修改后: 按CoreType获取

### 2.4 执行器层 (aicpu_executor.cpp)

**修改前**:
```cpp
// Phase 1: 检查完成（两阶段）
bool mixed_complete = rt->scheduler.on_subtask_complete(slot_state, subslot);
if (mixed_complete) {
    rt->scheduler.on_mixed_task_complete(slot_state, local_bufs);
}

// Phase 2: Local dispatch（遍历两个local_buf）
for (int bi = 0; bi < PTO2_LOCAL_DISPATCH_TYPE_NUM; bi++) {
    while (local_bufs[bi].count > 0) {
        PTO2TaskSlotState* slot_state = local_bufs[bi].pop();
        PTO2ResourceShape shape = pto2_active_mask_to_shape(slot_state->active_mask);
        int32_t ci = tracker.find_cluster_for_shape(shape);
        // 分发到cluster的多个core...
    }
}

// Phase 3: Global dispatch（按ResourceShape遍历）
const PTO2ResourceShape* dispatch_order = get_dispatch_order(thread_idx);
for (int32_t si = 0; si < PTO2_NUM_RESOURCE_SHAPES; si++) {
    PTO2ResourceShape shape = dispatch_order[si];
    while (true) {
        int32_t ci = tracker.find_cluster_for_shape(shape);
        PTO2TaskSlotState* slot_state = pop_ready_task(shape, thread_idx);
        // 分发到cluster...
    }
}
```

**修改后**:
```cpp
// Phase 1: 检查完成（单一阶段）
rt->scheduler.on_task_complete(slot_state, local_buf);

// Phase 2: 统计idle cores
int32_t idle_aic_count = tracker.aic().idle_count;
int32_t idle_aiv_count = tracker.aiv().idle_count;

// Phase 3: 批量获取ready tasks
PTO2TaskSlotState* ready_aic_tasks[MAX_IDLE_CORES];
PTO2TaskSlotState* ready_aiv_tasks[MAX_IDLE_CORES];
int32_t aic_task_count = rt->scheduler.get_ready_tasks(CoreType::AIC, idle_aic_count, ready_aic_tasks);
int32_t aiv_task_count = rt->scheduler.get_ready_tasks(CoreType::AIV, idle_aiv_count, ready_aiv_tasks);

// Phase 4: 批量分发
for (int32_t i = 0; i < aic_task_count; i++) {
    dispatch_to_aic_core(ready_aic_tasks[i]);
}
for (int32_t i = 0; i < aiv_task_count; i++) {
    dispatch_to_aiv_core(ready_aiv_tasks[i]);
}
```

**关键修改点**:
1. 移除两阶段完成检测
2. 移除ResourceShape相关的分发逻辑
3. 移除cluster概念（不再要求AIC+AIV原子分发）
4. 实现批量统计和分发

---

## 3. 数据流对比

### 修改前的数据流
```
提交MixedKernels -> 计算active_mask -> 确定ResourceShape
-> 分配task slot -> 初始化kernel_id[3]
-> 依赖解析 -> 就绪时推送到 shape-based ready queue
-> 分发时检查cluster空闲 -> 原子分发到多个core
-> 子任务逐一完成 -> 设置subtask_done_mask位
-> 全部完成 -> on_mixed_task_complete -> 通知下游
```

### 修改后的数据流
```
提交(kernel_id, core_type) -> 分配task slot
-> 依赖解析 -> 就绪时推送到 core_type-based ready queue
-> 分发时统计idle core数量 -> 批量获取ready tasks
-> 按core_type分发到对应idle core
-> 任务完成 -> on_task_complete -> 通知下游
```

---

## 4. 性能影响分析

### 性能提升
1. **减少内存占用**: TaskDescriptor和TaskSlotState结构体变小
2. **简化调度逻辑**: 移除两阶段完成检测和cluster匹配
3. **更好的并发性**: 不再要求AIC+AIV原子分发，可以独立调度

### 潜在风险
1. **失去cluster locality**: 无法保证AIC和AIV任务在同一cluster上执行
2. **负载不均衡**: 如果AIC和AIV任务数量不均衡，可能导致某些core空闲

---

## 5. 向后兼容性

### API变更
- **不兼容变更**: `pto2_submit_mixed_task()` 替换为 `pto2_submit_task()`
- **需要修改**: 所有调用 `pto2_submit_mixed_task()` 的orchestration代码

### 运行时配置
- **移除**: `PTO2_NUM_RESOURCE_SHAPES`, `PTO2_SUBTASK_SLOT_COUNT`
- **新增**: `PTO2_NUM_CORE_TYPES = 2` (AIC, AIV)

---

## 6. 测试策略

### 单元测试
1. 验证单一任务提交和完成
2. 验证依赖关系正确传递
3. 验证批量调度逻辑

### 集成测试
1. 运行现有测试用例，验证功能正确性
2. 性能对比测试
3. 压力测试和死锁检测

### 验收标准
1. 所有现有测试通过
2. 无性能回归
3. 代码复杂度降低

---

## 7. 迁移路径

### 阶段1: 核心数据结构修改
1. 修改 `pto_submit_types.h`
2. 修改 `pto_runtime2_types.h`
3. 修改 `pto_scheduler.h/cpp`

### 阶段2: 调度逻辑修改
1. 修改 `pto_orchestrator.cpp`
2. 修改 `aicpu_executor.cpp`

### 阶段3: 测试和验证
1. 编译验证
2. 单元测试
3. 集成测试

### 阶段4: 性能优化
1. 性能对比测试
2. 调优和优化

---

## 8. 文件修改清单

### 需要修改的文件
1. `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_submit_types.h`
2. `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_runtime2_types.h`
3. `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_scheduler.h`
4. `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_scheduler.cpp`
5. `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_orchestrator.h`
6. `src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_orchestrator.cpp`
7. `src/a2a3/runtime/tensormap_and_ringbuffer/aicpu/aicpu_executor.cpp`
8. `src/a5/runtime/tensormap_and_ringbuffer/` (对应的a5平台文件)

### 需要更新的文档
1. `docs/SUBMIT_BY_CLUSTER.md` (可能需要废弃或更新)
2. `docs/RUNTIME_LOGIC.md`
3. 其他相关设计文档

---

## 9. 总结

本次重构的核心目标是简化调度系统，移除mixed-task的复杂性，实现统一的批量调度机制。主要修改包括：

1. **任务结构简化**: 从mixed-task改为单一task
2. **队列统一**: 从5个shape-based队列合并为1个统一队列
3. **调度简化**: 移除两阶段完成检测和cluster原子分发
4. **批量调度**: 实现基于idle core统计的批量分发

这些修改将降低代码复杂度，提高可维护性，同时可能带来性能提升。

---

## 10. Bug Fix 记录

### Bug: 任务状态未正确设置为 PTO2_TASK_READY

**问题描述**: 在重构后的代码中，`release_fanin_and_check_ready()` 函数在将任务推入就绪队列时，没有将 `task_state` 从 `PTO2_TASK_PENDING` 设置为 `PTO2_TASK_READY`。这导致调度器无法正确识别就绪任务，造成任务卡住。

**错误现象**: 
- 运行时出现 `rtStreamSynchronize (AICPU) failed: 507018` 错误
- 诊断日志显示 `STUCK-READY` 状态的任务
- 任务依赖已满足（refcount >= fanin_count）但状态仍为 PENDING

**修复方案**: 在以下两个位置添加 CAS 操作，将 `task_state` 从 `PENDING` 设置为 `READY`：

1. **pto_scheduler.h** - `release_fanin_and_check_ready()` 函数:
```cpp
if (new_refcount == slot_state.fanin_count) {
    // CAS: PENDING -> READY (only the thread that succeeds should enqueue)
    PTO2TaskState expected = PTO2_TASK_PENDING;
    if (slot_state.task_state.compare_exchange_strong(
            expected, PTO2_TASK_READY,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        // Push to ready queue...
    }
}
```

2. **pto_orchestrator.cpp** - `pto2_submit_task()` 函数（orchestrator 直接推入队列时）:
```cpp
if (new_rc >= fanin_count + 1) {
    // CAS: PENDING -> READY before pushing to queue
    PTO2TaskState expected = PTO2_TASK_PENDING;
    if (cur_slot_state.task_state.compare_exchange_strong(
            expected, PTO2_TASK_READY,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        // Push to ready queue...
    }
}
```

**参考**: a5 版本的 `release_fanin_and_check_ready` 实现中使用了相同的 CAS 模式来设置 `PTO2_TASK_READY` 状态。

**修复日期**: 2026-03-18
