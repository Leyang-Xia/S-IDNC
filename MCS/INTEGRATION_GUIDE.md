# MCS 自适应控制模块 - 集成指南

## 一、模块概述

本模块实现多用户多播场景下的 MCS（调制编码方案）自适应选择，可替换现有工程中的固定 MCS=6 策略。

---

## 二、您需要提供的输入数据

### 2.1 初始化时提供（一次性）

#### (1) 用户数和 MCS 档位数
```c
int user_count = 8;      // 您系统中的组播用户数
int mcs_levels = 12;     // 您系统支持的 MCS 档位数（0~11）
```

#### (2) 速率表（可选，推荐提供）
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
    .delta_up = 0.05,       // 升档收益阈值（0~1），需要 Score(m+1) >= (1+delta_up)*Score(m)
    .delta_down = 0.05,     // 降档收益阈值（0~1）
    .hold_time_up = 2,      // 升档需连续满足条件的轮数（防抖）
    .hold_time_down = 2,    // 降档需连续满足条件的轮数（防抖）
    .no_sample_decay = 0.9, // 未尝试档位的衰减系数
    .n_min = 30             // 升档所需最小样本数
};
```

---

### 2.2 每轮（周期）提供的反馈数据

#### (1) attempts_delta - 本轮各用户在各档位的发送次数

**格式**：一维数组，长度 = `user_count * mcs_levels`
**存储顺序**：`index = user_id * mcs_levels + mcs_level`

```c
int attempts_delta[8 * 12];  // 8 用户 × 12 档位 = 96 个元素

// 示例：用户 3 在 MCS 6 发送了 90 次
attempts_delta[3 * 12 + 6] = 90;

// 用户 3 在 MCS 7 发送了 10 次（探索）
attempts_delta[3 * 12 + 7] = 10;

// 其余未使用的档位填 0
```

#### (2) success_ratio - 本轮各用户在各档位的成功率

**格式**：一维数组，长度 = `user_count * mcs_levels`
**存储顺序**：与 `attempts_delta` 相同

```c
double success_ratio[8 * 12];

// 示例：用户 3 在 MCS 6 的 90 次发送中有 85 次成功
success_ratio[3 * 12 + 6] = 85.0 / 90.0;  // = 0.9444

// 用户 3 在 MCS 7 的 10 次发送中有 8 次成功
success_ratio[3 * 12 + 7] = 8.0 / 10.0;   // = 0.8

// 未使用的档位（attempts_delta=0）可填任意值，算法会忽略
```

**注意：探索 MCS 由模块内部管理**

探索逻辑（按 +1, +2, -1, -2 偏移 round-robin 轮转）已集成在模块内，通过 `mcs_get_explore_mcs()` 获取。

---

## 三、模块输出的结果

### 3.1 主要输出：推荐的 MCS 档位

```c
int recommended_mcs = mcs_select(&cfg, &state, attempts_delta, success_ratio, NULL);
// 返回值：0~11 之间的整数，下一轮应该使用的 MCS 档位
```

### 3.2 探索 MCS 档位

```c
int explore_mcs = mcs_get_explore_mcs(&state);
// 返回值：基于当前 m_curr 计算的探索档位，按 +1,+2,-1,-2 轮转
// 注意：应在执行传输前调用，在 mcs_select 之前
```

### 3.3 详细决策信息（可选，用于调试/日志）

```c
MCSDecisionInfo info;
int recommended_mcs = mcs_select(&cfg, &state, attempts_delta, success_ratio, &info);

// 可获取的信息：
printf("上一轮 MCS: %d\n", info.prev_mcs);
printf("本轮推荐 MCS: %d\n", info.m_curr);
printf("Score(当前): %.2f\n", info.score_curr);
printf("Score(升档): %.2f\n", info.score_up);
printf("Score(降档): %.2f\n", info.score_down);
printf("升档计数器: %d/%d\n", info.hold_counter_up, cfg.hold_time_up);
printf("降档计数器: %d/%d\n", info.hold_counter_down, cfg.hold_time_down);

// 聚类信息
for (int c = 0; c < info.cluster_count; c++) {
    printf("簇 %d: 大小=%d, 权重=%.3f, 成功率=%.3f, 成员=",
           c, info.cluster_sizes[c], info.cluster_weights[c], info.cluster_success[c]);
    for (int i = 0; i < info.cluster_sizes[c]; i++) {
        printf(" %d", info.cluster_members[c][i]);
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
    cfg.delta_up = 0.05;
    cfg.delta_down = 0.05;
    cfg.hold_time_up = 2;
    cfg.hold_time_down = 2;
    cfg.no_sample_decay = 0.9;
    cfg.n_min = 30;

    // 2.2 初始化状态（8 用户、12 档位）
    mcs_init_state(&state, 8, 12, NULL);
    
    // 2.3 设置速率表
    static const double R_TABLE[12] = {
        72.059, 144.118, 216.176, 288.235,
        432.353, 576.471, 648.529, 720.588,
        864.706, 960.784, 1080.882, 1200.980
    };
    state.r_table = R_TABLE;
}
```

### 步骤 3：在周期性调度点调用模块

假设您现有工程每 100ms 或每完成一批发送后进行一次调度：

```c
void your_periodic_task(void) {
    // 3.1 获取本轮的探索 MCS（基于当前 m_curr）
    int m_curr = state.m_curr;           // 当前主用 MCS
    int explore_mcs = mcs_get_explore_mcs(&state);  // 获取探索 MCS
    
    // 3.2 执行传输（主用 + 探索）
    your_send_with_mcs(m_curr, 90);       // 主用 MCS 发送 90 次
    your_send_with_mcs(explore_mcs, 10);  // 探索 MCS 发送 10 次
    
    // 3.3 收集统计数据
    int attempts_delta[8 * 12] = {0};
    double success_ratio[8 * 12] = {0};
    
    for (int user = 0; user < 8; user++) {
        for (int mcs = 0; mcs < 12; mcs++) {
            int idx = user * 12 + mcs;
            
            // 从您的工程中获取：
            // - 用户 user 在 MCS mcs 上发送了多少次
            // - 成功了多少次（收到 ACK）
            attempts_delta[idx] = your_get_tx_count(user, mcs);
            
            if (attempts_delta[idx] > 0) {
                int success_count = your_get_ack_count(user, mcs);
                success_ratio[idx] = (double)success_count / (double)attempts_delta[idx];
            }
        }
    }
    
    // 3.4 基于统计数据选择下一轮的主用 MCS
    MCSDecisionInfo info;
    int recommended_mcs = mcs_select(&cfg, &state, attempts_delta, success_ratio, &info);
    
    // 3.5 （可选）打印日志
    printf("[MCS] 轮次=%d, MCS %d->%d, explore=%d, Score=%.2f\n",
           your_round_counter, info.prev_mcs, recommended_mcs, explore_mcs, info.score_curr);
}
```

**关键流程：**
1. 获取探索 MCS（`mcs_get_explore_mcs`）
2. 执行传输（主用 + 探索）
3. 收集统计数据
4. 调用 `mcs_select` 更新状态并决策下一轮 MCS

---

## 五、数据格式详解

### 5.1 二维数据的一维存储

由于 C 语言接口使用一维数组传递二维数据（用户×档位），需要遵循以下索引规则：

```
索引 = user_id * mcs_levels + mcs_level

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
success_ratio = (收到 ACK 的数据包数) / (发送的数据包总数)

// 等价于：
success_ratio = 1.0 - PER  // PER = Packet Error Rate
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
    int mcs = state.m_curr;
    int explore_mcs = mcs_get_explore_mcs(&state);
    
    // 2. 主用发送（90%）
    for (int i = 0; i < 90; i++) {
        for (int user = 0; user < 8; user++) {
            send_packet_to_user(user, mcs);
        }
    }
    
    // 3. 探索发送（10%）
    for (int i = 0; i < 10; i++) {
        for (int user = 0; user < 8; user++) {
            send_packet_to_user(user, explore_mcs);
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
        .delta_up = 0.05, .delta_down = 0.05,
        .hold_time_up = 2, .hold_time_down = 2,
        .no_sample_decay = 0.9, .n_min = 30
    };
    mcs_init_state(&state, 8, 12, NULL);
    state.r_table = R_TABLE;
}

// 获取探索 MCS
int mcs_module_get_explore(void) {
    return mcs_get_explore_mcs(&state);
}

// 每轮调用（周期性或事件触发）
int mcs_module_update(int *tx_counts, double *ack_ratios) {
    return mcs_select(&cfg, &state, tx_counts, ack_ratios, NULL);
}

// 使用示例
void main_loop(void) {
    mcs_module_init();
    
    for (int round = 0; round < 100; round++) {
        // 1. 获取本轮的主用和探索 MCS
        int m_curr = state.m_curr;
        int m_explore = mcs_module_get_explore();
        
        // 2. 您的代码：执行本轮发送
        // ... 用 m_curr 和 m_explore 发送 ...
        
        // 3. 您的代码：统计结果
        int tx_counts[96] = {0};
        double ack_ratios[96] = {0};
        // ... 填充 tx_counts 和 ack_ratios ...
        
        // 4. 更新 MCS
        int next_mcs = mcs_module_update(tx_counts, ack_ratios);
        printf("Round %d: MCS %d, explore %d\n", round, next_mcs, m_explore);
    }
}
```

---

## 八、常见问题

### Q1: 如果我不想做探索，只用主用 MCS？
A: 可以，但会影响算法的自适应能力。在发送时忽略 `mcs_get_explore_mcs` 的返回值，全部用 `state.m_curr` 即可。注意：必须仍然调用 `mcs_get_explore_mcs` 来更新内部状态。

### Q2: 用户数或档位数不是 8/12 怎么办？
A: 修改 `mcs_controller.h` 中的 `MCS_MAX_USERS` 和 `MCS_MAX_LEVELS` 常量，然后重新编译。

### Q3: 我的系统没法统计每个用户的 ACK？
A: 如果只能统计总体成功率，可以给所有用户填相同的值：
```c
double overall_ratio = total_acks / total_attempts;
for (int i = 0; i < 8 * 12; i++) {
    success_ratio[i] = overall_ratio;
}
```
但这会降低算法对异构用户的适应性。

### Q4: 多久调用一次 mcs_select？
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
   mcs_select(&cfg, &state, attempts_delta, success_ratio, explore_mcs, &info);
   printf("MCS %d->%d, Up=%d/%d, Down=%d/%d, Score=%.2f/%.2f/%.2f\n",
          info.prev_mcs, info.m_curr,
          info.hold_counter_up, cfg.hold_time_up,
          info.hold_counter_down, cfg.hold_time_down,
          info.score_down, info.score_curr, info.score_up);
   ```

2. **验证输入数据**
   ```c
   int total = 0;
   for (int i = 0; i < 8 * 12; i++) total += attempts_delta[i];
   printf("本轮总发送次数: %d\n", total);  // 应该与实际发送数匹配
   ```

3. **验证调用顺序**
   ```c
   // 正确顺序：
   // 1. mcs_get_explore_mcs()  <- 获取探索 MCS
   // 2. 执行传输并收集统计
   // 3. mcs_select()           <- 更新状态并决策下一轮 MCS
   ```

4. **检查速率表设置**
   ```c
   if (state.r_table == NULL) {
       fprintf(stderr, "错误：未设置速率表！\n");
   }
   ```

---

如有其他问题，请参考 `mcs_sim_demo.c` 中的完整示例。
