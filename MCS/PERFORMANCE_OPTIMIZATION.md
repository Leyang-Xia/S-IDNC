# MCS控制器性能优化说明

## 优化目标
确保在100ms定时器调用场景下，MCS调控算法能在规定时间内完成执行。

## 主要性能瓶颈分析

### 1. **内存分配瓶颈** ⚠️ 高优先级
**问题**：`PavNonIncreasing`函数每次调用都使用`malloc/free`
- **影响**：动态内存分配/释放开销大（通常需要微秒级时间）
- **优化方案**：使用栈分配（`MCS_MAX_LEVELS=12`，完全在栈容量内）
- **性能提升**：消除malloc/free开销，预计节省 **5-20μs**

### 2. **重复计算瓶颈** ⚠️ 高优先级  
**问题**：`McsSelect`中多次调用`ComputeScore`，每次都会调用`CalcClusterSuccessAt`
- **影响**：`CalcClusterSuccessAt`内部每次都要排序（`InsertionSort`），相同簇在不同档位重复计算
- **优化方案**：预计算簇成功率并缓存，避免重复排序
- **性能提升**：减少排序次数，预计节省 **10-30μs**

### 3. **聚类算法复杂度** ⚠️ 中优先级
**问题**：`SelectActiveClusters`中k=3时的双重循环（O(n²)）
- **影响**：8用户时，最多需要计算 C(7,2) = 21 次SSE计算
- **优化方案**：优化SSE宏计算，减少重复读取数组
- **性能提升**：减少内存访问，预计节省 **2-5μs**

### 4. **定点数运算** ⚠️ 低优先级
**问题**：`CalcClusterSuccessAt`中使用了两次定点数乘法
- **影响**：`FixedMul(700, q80) + FixedMul(300, mean)` 需要两次乘法+两次除法
- **优化方案**：合并计算为 `(7*q80 + 3*mean)/10`，只需一次除法
- **性能提升**：减少一次除法和一次乘法，预计节省 **1-2μs**

## 优化实施详情

### 优化1：栈分配替代动态内存
```c
// 优化前：每次调用malloc/free
int32_t *y = (int32_t *)malloc(sizeof(int32_t) * n);
...
free(y);

// 优化后：栈分配
int32_t y[MCS_MAX_LEVELS];  // 最大12个元素，约48字节
```

### 优化2：预计算簇成功率
```c
// 优化前：每次ComputeScore都调用CalcClusterSuccessAt
int64_t scoreCurr = ComputeScore(state, cfg, &clusters, state->mCurr);
int64_t scoreUp = ComputeScore(state, cfg, &clusters, state->mCurr + 1);
int64_t scoreDown = ComputeScore(state, cfg, &clusters, state->mCurr - 1);

// 优化后：预计算并缓存
int32_t clusterSuccCache[MCS_MAX_CLUSTERS];
for (int c = 0; c < clusters.clusters; ++c) {
    clusterSuccCache[c] = CalcClusterSuccessAt(...);  // 只计算一次
}
int64_t scoreCurr = ComputeScore(state, cfg, &clusters, state->mCurr, clusterSuccCache);
```

### 优化3：优化SSE计算
```c
// 优化前：重复读取vSorted[i]
p2[i+1] = p2[i] + (int64_t)vSorted[i] * vSorted[i];

// 优化后：缓存到局部变量
int64_t v = vSorted[i];
p2[i+1] = p2[i] + v * v;
```

### 优化4：简化定点数运算
```c
// 优化前：两次定点数乘法
return FixedMul(700, q80) + FixedMul(300, mean);

// 优化后：合并为一次除法
return (int32_t)(((int64_t)7 * q80 + (int64_t)3 * mean) / 10);
```

## 性能估算

### 优化前（预估）
- `PavNonIncreasing`: ~15-30μs（含malloc/free）
- `SelectActiveClusters`: ~20-40μs
- `CalcClusterSuccessAt` (×3次): ~15-30μs
- `ComputeScore` (×3次): ~10-20μs
- **总计**: ~60-120μs

### 优化后（预估）
- `PavNonIncreasing`: ~5-10μs（栈分配）
- `SelectActiveClusters`: ~15-35μs（优化SSE计算）
- `CalcClusterSuccessAt` (×3次，但缓存): ~5-10μs（减少重复排序）
- `ComputeScore` (×3次): ~3-8μs（使用缓存）
- **总计**: ~28-63μs

### 性能提升
- **预计提升**: 约 **50-60%**
- **最坏情况**: 仍远低于100ms要求（<100μs）
- **安全余量**: 约 **99.9%** 的时间余量

## 进一步优化建议（如需要）

### 1. 条件编译优化
在发布版本中禁用调试日志：
```c
#ifndef MCS_DEBUG
#define MCS_DEBUG 0
#endif
```

### 2. 编译器优化选项
- `-O2` 或 `-O3`: 启用编译器优化
- `-ffast-math`: 如果精度要求不高
- `-march=native`: 针对目标CPU优化

### 3. 可能的缓存优化
- 如果用户数较少（<4），可以考虑直接展开部分循环
- 使用查找表替代部分计算（如果空间允许）

## 注意事项

1. **栈空间**：所有优化都使用栈分配，确保栈空间充足（通常至少8KB）
2. **缓存友好性**：数组访问模式已经优化（MCS主序），符合缓存局部性
3. **精度**：优化未改变算法精度，只是减少计算开销

## 测试建议

1. 使用`clock_gettime`或`QueryPerformanceCounter`测量实际执行时间
2. 在不同用户数（4, 6, 8）和档位数（8, 12）下测试
3. 验证在100ms定时器压力下的稳定性

