# MCS 自适应控制模块 - 集成说明

## 一、模块概述

本模块实现多用户多播场景下的 MCS（调制编码方案）自适应选择，可替换现有工程中的固定 MCS=6 策略。

### 接口设计特点

模块采用**完全解耦**的接口设计，职责清晰分离：

1. **`McsGetExploreMcs()`** - 探索模块  
   功能：获取探索 MCS（基于 round-robin 策略）  
   调用时机：每轮传输前

2. **`McsUpdateWithRound()`** - 数据更新模块  
   功能：更新 EWMA 成功率和单调性修正  
   调用时机：每轮传输后、决策前

3. **`McsSelect()`** - 决策模块  
   功能：纯决策逻辑（聚类、评分、升降档判断）  
   调用时机：数据更新后

**设计优势：**
- ✅ 职责单一：每个函数只负责一件事
- ✅ 调用灵活：可以只更新数据不决策，或多次决策不重复更新
- ✅ 易于测试：可独立测试各模块功能
- ✅ 易于理解：调用顺序清晰直观

---

## 二、需要提供的输入数据

### 2.1 初始化时提供（一次性）

#### (1) 用户数和 MCS 档位数
```c
int user_count = 8;      // 您系统中的组播用户数
int mcs_levels = 12;     // 您系统支持的 MCS 档位数（0~11）
```

#### (2) 速率表
```c
// 每个 MCS 档位对应的理论速率（Mbps），按档位 0~11 排列
const double R_TABLE[12] = {
    72.059,   // MCS 0
    144.118,  // MCS 1
    216.176,  // MCS 2
    288.235,  // MCS 3
    432.353,  // MCS 4
    576.471,  // MCS 5
    648.529,  // MCS 6
    720.588,  // MCS 7
    864.706,  // MCS 8
    960.784,  // MCS 9
    1080.882, // MCS 10
    1200.980  // MCS 11
};
```
*根据实际系统参数（带宽、NSS、GI）调整数值*

#### (3) 算法参数（可使用推荐值）
```c
MCSConfig cfg = {
    .alpha = 0.25,          // EWMA 平滑系数（0~1），值越小对新数据反应越快
    .zeta = 0.2,            // 簇占比阈值（0~1），低于此比例的簇会被过滤
    .beta = 1.0,            // 簇权重指数，簇权重 = (簇大小)^beta
    .gamma = 1.2,           // 评分函数指数，Score = R * (成功率)^gamma
    .deltaUp = 0.05,        // 升档收益阈值（0~1），需要 Score(m+1) >= (1+deltaUp)*Score(m)
    .deltaDown = 0.05,      // 降档收益阈值（0~1）
    .holdTimeUp = 2,        // 升档需连续满足条件的轮数（防抖）
    .holdTimeDown = 2,      // 降档需连续满足条件的轮数（防抖）
    .noSampleDecay = 0.9,   // 未尝试档位的衰减系数
    .nMin = 30              // 升档所需最小样本数
};
```

---

### 2.2 每轮（周期）提供的反馈数据

#### (1) attemptsDelta - 本轮各用户在各档位的发送次数

**格式**：一维数组，长度 = `userCount * mcsLevels`
**存储顺序**：`index = userId * mcsLevels + mcsLevel`

```c
int attemptsDelta[8 * 12];  // 8 用户 × 12 档位 = 96 个元素

// 示例：用户 3 在 MCS 6 发送了 90 次
attemptsDelta[3 * 12 + 6] = 90;

// 用户 3 在 MCS 7 发送了 10 次（探索）
attemptsDelta[3 * 12 + 7] = 10;

// 其余未使用的档位填 0
```

#### (2) successRatio - 本轮各用户在各档位的成功率

**格式**：一维数组，长度 = `userCount * mcsLevels`
**存储顺序**：与 `attemptsDelta` 相同

```c
double successRatio[8 * 12];

// 示例：用户 3 在 MCS 6 的 90 次发送中有 85 次成功
successRatio[3 * 12 + 6] = 85.0 / 90.0;  // = 0.9444

// 用户 3 在 MCS 7 的 10 次发送中有 8 次成功
successRatio[3 * 12 + 7] = 8.0 / 10.0;   // = 0.8

// 未使用的档位（attemptsDelta=0）可填任意值，算法会忽略
```

**注意：探索 MCS 由模块内部管理**

探索逻辑（按 +1, +2, -1, -2 偏移 round-robin 轮转）已集成在模块内，通过 `McsGetExploreMcs()` 获取。

---

## 三、模块输出的结果

### 3.1 探索 MCS 档位

```c
int exploreMcs = McsGetExploreMcs(&state);
// 返回值：基于当前 mCurr 计算的探索档位，按 +1,+2,-1,-2 轮转
// 调用时机：每轮传输前
```

### 3.2 更新统计数据

```c
McsUpdateWithRound(&state, &cfg, attemptsDelta, successRatio);
// 功能：更新 EWMA 成功率并进行单调性修正
// 调用时机：每轮传输后、决策前
```

### 3.3 选择主用 MCS（决策）

```c
int recommendedMcs = McsSelect(&cfg, &state, NULL);
// 返回值：0~11 之间的整数，下一轮应该使用的 MCS 档位
// 调用时机：McsUpdateWithRound 之后
```

### 3.4 详细决策信息（可选，用于调试/日志）

```c
MCSDecisionInfo info;
int recommendedMcs = McsSelect(&cfg, &state, &info);

// 可获取的信息：
printf("上一轮 MCS: %d\n", info.prevMcs);
printf("本轮推荐 MCS: %d\n", info.mCurr);
printf("Score(当前): %.2f\n", info.scoreCurr);
printf("Score(升档): %.2f\n", info.scoreUp);
printf("Score(降档): %.2f\n", info.scoreDown);
printf("升档计数器: %d/%d\n", info.holdCounterUp, cfg.holdTimeUp);
printf("降档计数器: %d/%d\n", info.holdCounterDown, cfg.holdTimeDown);

// 聚类信息
for (int c = 0; c < info.clusterCount; c++) {
    printf("簇 %d: 大小=%d, 权重=%.3f, 成功率=%.3f, 成员=",
           c, info.clusterSizes[c], info.clusterWeights[c], info.clusterSuccess[c]);
    for (int i = 0; i < info.clusterSizes[c]; i++) {
        printf(" %d", info.clusterMembers[c][i]);
    }
    printf("\n");
}
```

---

## 四、集成步骤

### 步骤 1：将源文件添加到工程

```
您的工程目录/
├── mcs_controller.h      ← 复制此文件
├── mcs_controller.c      ← 复制此文件
└── 您的主程序.c
```

在编译时加入 `mcs_controller.c`，并在需要的地方 `#include "mcs_controller.h"`。

### 步骤 2：初始化模块（程序启动时执行一次）

```c
#include "mcs_controller.h"

// 全局变量或结构体成员
MCSConfig cfg;
MCSState state;

void your_init_function(void) {
    // 2.1 配置参数
    cfg.alpha = 0.25;
    cfg.zeta = 0.2;
    cfg.beta = 1.0;
    cfg.gamma = 1.2;
    cfg.deltaUp = 0.05;
    cfg.deltaDown = 0.05;
    cfg.holdTimeUp = 2;
    cfg.holdTimeDown = 2;
    cfg.noSampleDecay = 0.9;
    cfg.nMin = 30;

    // 2.2 初始化状态（8 用户、12 档位）
    McsInitState(&state, 8, 12, NULL);
    
    // 2.3 设置速率表
    static const double R_TABLE[12] = {
        72.059, 144.118, 216.176, 288.235,
        432.353, 576.471, 648.529, 720.588,
        864.706, 960.784, 1080.882, 1200.980
    };
    state.rTable = R_TABLE;
}
```

### 步骤 3：在周期性调度点调用模块

假设您现有工程每 100ms 或每完成一批发送后进行一次调度：

```c
void your_periodic_task(void) {
    // 3.1 获取本轮的探索 MCS（基于当前 mCurr）
    int mCurr = state.mCurr;                      // 当前主用 MCS
    int exploreMcs = McsGetExploreMcs(&state);    // 获取探索 MCS
    
    // 3.2 执行传输（主用 + 探索）
    your_send_with_mcs(mCurr, 90);                // 主用 MCS 发送 90 次
    your_send_with_mcs(exploreMcs, 10);           // 探索 MCS 发送 10 次
    
    // 3.3 收集统计数据
    int attemptsDelta[8 * 12] = {0};
    double successRatio[8 * 12] = {0};
    
    for (int user = 0; user < 8; user++) {
        for (int mcs = 0; mcs < 12; mcs++) {
            int idx = user * 12 + mcs;
            
            // 从您的工程中获取：
            // - 用户 user 在 MCS mcs 上发送了多少次
            // - 成功了多少次（收到 ACK）
            attemptsDelta[idx] = your_get_tx_count(user, mcs);
            
            if (attemptsDelta[idx] > 0) {
                int successCount = your_get_ack_count(user, mcs);
                successRatio[idx] = (double)successCount / (double)attemptsDelta[idx];
            }
        }
    }
    
    // 3.4 更新统计数据
    McsUpdateWithRound(&state, &cfg, attemptsDelta, successRatio);
    
    // 3.5 基于更新后的状态选择下一轮的主用 MCS
    MCSDecisionInfo info;
    int recommendedMcs = McsSelect(&cfg, &state, &info);
    
    // 3.6 （可选）打印日志
    printf("[MCS] 轮次=%d, MCS %d->%d, explore=%d, Score=%.2f\n",
           your_round_counter, info.prevMcs, recommendedMcs, exploreMcs, info.scoreCurr);
}
```

**关键流程：**
1. 获取探索 MCS（`McsGetExploreMcs`）
2. 执行传输（主用 + 探索）
3. 收集统计数据
4. 更新统计数据（`McsUpdateWithRound`）
5. 基于更新后的状态决策（`McsSelect`）

---

## 五、数据格式详解

### 5.1 二维数据的一维存储

由于 C 语言接口使用一维数组传递二维数据（用户×档位），需要遵循以下索引规则：

```
索引 = userId * mcsLevels + mcsLevel

示例（8 用户 × 12 档位）：
┌─────────────────────────────────────────┐
│ 用户0                                    │  索引 0~11
│ MCS0 MCS1 MCS2 ... MCS11               │
├─────────────────────────────────────────┤
│ 用户1                                    │  索引 12~23
│ MCS0 MCS1 MCS2 ... MCS11               │
├─────────────────────────────────────────┤
│ ...                                     │
├─────────────────────────────────────────┤
│ 用户7                                    │  索引 84~95
│ MCS0 MCS1 MCS2 ... MCS11               │
└─────────────────────────────────────────┘
```

### 5.2 成功率的定义

```c
successRatio = (收到 ACK 的数据包数) / (发送的数据包总数)

// 等价于：
successRatio = 1.0 - PER  // PER = Packet Error Rate
```

---

## 六、与现有固定 MCS=6 的对比

### 现有方案
```c
void your_current_transmit(void) {
    int mcs = 6;  // 固定使用 MCS 6
    for (int user = 0; user < 8; user++) {
        send_packet_to_user(user, mcs);
    }
}
```

### 集成后方案
```c
void your_new_transmit(void) {
    // 1. 获取探索 MCS（模块内部管理 round-robin）
    int mcs = state.mCurr;
    int exploreMcs = McsGetExploreMcs(&state);
    
    // 2. 主用发送（90%）
    for (int i = 0; i < 90; i++) {
        for (int user = 0; user < 8; user++) {
            send_packet_to_user(user, mcs);
        }
    }
    
    // 3. 探索发送（10%）
    for (int i = 0; i < 10; i++) {
        for (int user = 0; user < 8; user++) {
            send_packet_to_user(user, exploreMcs);
        }
    }
}
```

---

## 七、最小集成示例

```c
#include "mcs_controller.h"

// 全局状态
static MCSConfig cfg;
static MCSState state;
static const double R_TABLE[12] = {
    72.059, 144.118, 216.176, 288.235,
    432.353, 576.471, 648.529, 720.588,
    864.706, 960.784, 1080.882, 1200.980
};

// 初始化（程序启动时调用一次）
void mcs_module_init(void) {
    cfg = (MCSConfig){
        .alpha = 0.25, .zeta = 0.2, .beta = 1.0, .gamma = 1.2,
        .deltaUp = 0.05, .deltaDown = 0.05,
        .holdTimeUp = 2, .holdTimeDown = 2,
        .noSampleDecay = 0.9, .nMin = 30
    };
    McsInitState(&state, 8, 12, NULL);
    state.rTable = R_TABLE;
}

// 获取探索 MCS
int mcs_module_get_explore(void) {
    return McsGetExploreMcs(&state);
}

// 每轮调用（周期性或事件触发）
int mcs_module_update(int *txCounts, double *ackRatios) {
    McsUpdateWithRound(&state, &cfg, txCounts, ackRatios);
    return McsSelect(&cfg, &state, NULL);
}

// 使用示例
void main_loop(void) {
    mcs_module_init();
    
    for (int round = 0; round < 100; round++) {
        // 1. 获取本轮的主用和探索 MCS
        int mCurr = state.mCurr;
        int mExplore = mcs_module_get_explore();
        
        // 2. 您的代码：执行本轮发送
        // ... 用 mCurr 和 mExplore 发送 ...
        
        // 3. 您的代码：统计结果
        int txCounts[96] = {0};
        double ackRatios[96] = {0};
        // ... 填充 txCounts 和 ackRatios ...
        
        // 4. 更新 MCS
        int nextMcs = mcs_module_update(txCounts, ackRatios);
        printf("Round %d: MCS %d, explore %d\n", round, nextMcs, mExplore);
    }
}
```

---

## 八、常见问题

### Q1: 如果我不想做探索，只用主用 MCS？
A: 可以，但会影响算法的自适应能力。在发送时忽略 `McsGetExploreMcs` 的返回值，全部用 `state.mCurr` 即可。注意：必须仍然调用 `McsGetExploreMcs` 来更新内部状态。

### Q2: 用户数或档位数不是 8/12 怎么办？
A: 修改 `mcs_controller.h` 中的 `MCS_MAX_USERS` 和 `MCS_MAX_LEVELS` 常量，然后重新编译。

### Q3: 我的系统没法统计每个用户的 ACK？
A: 如果只能统计总体成功率，可以给所有用户填相同的值：
```c
double overallRatio = totalAcks / totalAttempts;
for (int i = 0; i < 8 * 12; i++) {
    successRatio[i] = overallRatio;
}
```
但这会降低算法对异构用户的适应性。

### Q4: 多久调用一次 McsSelect？
A: 推荐每发送 100~1000 个数据包后调用一次，或每 100ms~1s 调用一次，根据信道变化速率调整。

---

## 九、性能要求

- **内存占用**：约 2KB（MCSState + MCSConfig）
- **计算复杂度**：O(N·M + N²)，N=用户数，M=档位数，8 用户时约 1000 条指令
- **实时性**：单次调用耗时 < 1ms（现代 CPU）

---

## 十、调试建议

1. **打印决策过程**
   ```c
   MCSDecisionInfo info;
   McsUpdateWithRound(&state, &cfg, attemptsDelta, successRatio);
   McsSelect(&cfg, &state, &info);
   printf("MCS %d->%d, Up=%d/%d, Down=%d/%d, Score=%.2f/%.2f/%.2f\n",
          info.prevMcs, info.mCurr,
          info.holdCounterUp, cfg.holdTimeUp,
          info.holdCounterDown, cfg.holdTimeDown,
          info.scoreDown, info.scoreCurr, info.scoreUp);
   ```

2. **验证输入数据**
   ```c
   int total = 0;
   for (int i = 0; i < 8 * 12; i++) total += attemptsDelta[i];
   printf("本轮总发送次数: %d\n", total);  // 应该与实际发送数匹配
   ```

3. **验证调用顺序**
   ```c
   // 正确顺序：
   // 1. McsGetExploreMcs()       <- 获取探索 MCS
   // 2. 执行传输并收集统计
   // 3. McsUpdateWithRound()     <- 更新统计数据
   // 4. McsSelect()              <- 基于更新后的状态决策
   ```

4. **检查速率表设置**
   ```c
   if (state.rTable == NULL) {
       fprintf(stderr, "错误：未设置速率表！\n");
   }
   ```

---

