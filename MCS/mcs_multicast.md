## MCS 组播调控算法（Minstrel-like + 聚类）

### 1. 目标
- **目标**：在 2–8 用户的组播场景下，依据每用户 ACK 推断 PER，按簇聚合得到“代表性成功率”，忽略/降权离群者/簇，选择收益最大且稳定的组播 MCS。

### 2. 场景与假设
- 每轮传输结束，收集每个用户是否成功接收（ACK/NAK）。
- 链路条件可随时间变化，可能出现离群用户或突然离开。
- 初始化：`default_mcs = 6`

### 3. 主要思路
- **聚类与稳健化**：基于每个用户在锚定档位 $m_{\text{curr}}$ 的成功率 EWMA $s[i][m_{\text{curr}}]$，采用一维聚类方法将用户划分为簇，仅利用“活跃簇”的代表性成功率参与决策，从而避免少量表现差的用户影响整体选择。
- **Minstrel 思路**：持续 EWMA 更新、吞吐评分、带约束的选择、防抖与周期性探索。
- **无样本衰减**：对当轮未尝试的 MCS，先将上一轮的 PER 估计 $p[i][m]$ 做温和衰减，随后再换算为成功率（即 $s[i][m]=1-p[i][m]$）。

### 4. 变量与符号
- 索引与集合：
  - `i`：用户索引，$i \in \{1..N\}$；`N`：用户总数
  - `m`：MCS 档位索引，$m \in \text{MCS\_TABLE}$；`MCS_TABLE`：可用 MCS 集合（802.11ax WiFi6 {0..11}）
  - `users`：用户集合；`clusters`：聚类后的簇集合；`c`：某一簇；`C_active`：活跃簇集合

- 基本统计量（逐用户/逐 MCS）：
  - `attempt[i][m]`：对用户 `i` 在档位 `m` 的尝试次数
  - `success[i][m]`：对应成功次数
  - `round_succ_main`：主簇在当前轮的滚动成功率（用于突发降档）

- 成功率估计：
  - $s[i][m] \in [0,1]$：用户 $i$ 在 $m$ 的 EWMA 成功率
  - `Q80(·)`：80% 分位数；`mean(·)`：平均值
  - $S_c[m]$：簇 `c` 在 `m` 的代表成功率（如 $0.7 \cdot Q80 + 0.3 \cdot \text{mean}$）
  - `sustained(m, k)`：候选条件是否已连续满足 `k` 轮

- 评分与探索：
  - $R[m]$：档位 $m$ 的净吞吐
  $\text{Score}[m] = R[m] \cdot \sum_{c\in C_{\text{active}}} w_c \cdot (S_c[m])^{\gamma}$：档位评分
  <!-- - $\text{UCB}[m] = \text{Score}[m] + c \cdot \sqrt{\frac{\ln(T)}{n_m}}$：探索上界评分；`c`：UCB 系数；`T`：累计探索步；`n_m`：档位 `m` 已探索次数 -->
  - $w_c$：簇权重，$w_c \propto |c|^{\beta} \cdot \text{priority}_c$，归一化后 $\sum w_c = 1$；`priority_c`：簇优先级（默认 1）


- 控制状态与决策：
  - `m_curr`：当前使用的 MCS
  - `hold_counter`：保持计数（用于防抖）
  - `m_safe_low`：保底档位（稳妥低 MCS）
  - `default_mcs`：初始档位（默认 6）
  - $c^*$：活跃簇中规模最大的主簇（若有两簇规模相同，选簇代表成功率较低的簇）
  - `K_clusters`：k-means++ 的簇数（2 或 3）

- 时间与衰减：
  - `Δt`：统计更新的时间间隔
  - `α`（`ALPHA`）：EWMA 对 `s` 的遗忘因子

- 阈值与超参：
  - $\zeta$（`ZETA`）：活跃簇规模占比下限；$\beta$（`BETA`）：簇权重幂；$\gamma$（`GAMMA`）：评分幂
  - `Δ_up`：升档收益增益阈值；`Δ_down`：降档收益增益阈值
  - `HOLD_TIME_UP`：升档连续满足轮数；`HOLD_TIME_DOWN`：降档连续满足轮数
  - `EXPLORE_RATIO`：探索占比
  
<!-- `θ_drop`：突发降档的滚动成功率阈值； -->
- 调度与计划：
  - `K_total`：本轮总发送数；`K_explore`：探索发送数；`K_main`：主用发送数
  - `plan`：当轮发送计划；`explore_set`：探索档位集合

- 固定设定与外部常量：
  - `BW`：带宽（默认 `80MHz`）；`NSS`：空间流数（默认 `2`）
  - `prior(m)`：样本不足时对 `S_c[m]` 的先验估计（如递减到 0.6）

### 5. 统计与估计
- EWMA 更新：
  - 若近期窗口成功率 $x=\frac{\text{success}}{\text{attempt}}$：
    - $ s[i,m] \leftarrow \alpha \cdot s[i,m] + (1-\alpha)\cdot x $
  - 时间基准：固定更新间隔 $ \Delta t = 100\text{ms} $。

- 无样本衰减：若当轮 $\text{attempt}[i][m] = 0$，则将上一轮 PER 估计 $p[i,m]=1-s[i,m]$ 做衰减：
  - $ p[i,m] \leftarrow \rho \cdot p[i,m] $
  - 更新成功率：$ s[i,m] \leftarrow 1 - p[i,m] $



#### 5.1 用户PER单调修正
- 目的：纠正样本稀疏/噪声导致的跨 MCS 非单调估计，使高阶成功率不高于低阶（PER 单调不下降）。

- 方法：缺失数据处理 + PAV（同调回归）：
  - 缺失定义：$\text{attempt}[i][m] = 0$
  - 前缀最低填充：对低\rightarrow高 MCS，若 $s[i,m]$ 缺失，则用“前缀最低值”填入：$s^{\text{pref}}[m] = \min\limits_{k\le m,\ \text{observed}} s[i,k]$（无前缀则保持缺失）。
  - PAV：对序列 `s[i,m]` 加约束 `s[m] ≥ s[m+1]`，相邻违约块合并为均值，直至整体非增。

### 6. MCS 评分

#### 6.1. 聚类与活跃簇选择
- 输入：一维样本向量 `s[i][m_curr]`
- 聚类方法：1D k-means++（K∈{2,3}，优先 2；当 N≥6 且存在明显中间簇时允许 3）。
  - K 选择：计算 K=2 与 K=3 的 轮廓系数，若 K=3 的相对改进 < 10% 则用 K=2。
- 活跃簇选择：仅保留规模占比 ≥ ζ 的簇进入 `C_active`；其余视为离群簇（评分不考虑）。
 - 簇权重：$ w_c \propto |c|^{\beta}\cdot \text{priority}_c $，归一化到 Σw=1。
- 簇代表成功率：
  - $ S_c[m] = 0.7 \cdot Q80(\{s[i,m]\}) + 0.3 \cdot \text{mean}(\cdot) $

#### 6.2 评分公式
- 评分（活跃簇的期望吞吐）：
  - $ \text{Score}[m] = R[m] \cdot \sum_{c\in C_{\text{active}}} w_c \cdot (S_c[m])^{\gamma} $

### 7 升降阶与探测机制

#### 7.1 升降阶逻辑
- 升阶（更高 MCS）
- 触发条件：
  - 收益比较：若存在 $m-1$ 且 $ \text{Score}[m{+}1] \ge (1{+}\Delta_{up})\cdot \text{Score}[m] $
  - 防抖：连续 `HOLD_TIME_UP` 轮满足条件，发送 `m+1`阶探针

- 降阶（更低 MCS）
- 触发条件：
  - 收益比较：若存在 $m-1$ 且 $ \text{Score}[m-1] \ge (1{+}\Delta_{down})\cdot \text{Score}[m] $。
  - 防抖：连续 `HOLD_TIME_DOWN` 轮满足条件后，发送 `m-1` 阶探针。


#### 7.2. 条件触发式探测-验证机制
- **探测触发**：仅在升/降阶条件连续满足达到防抖阈值时触发，不采用固定比例探测。
- **探测对象**：仅探测目标档位（`m_curr+1` 或 `m_curr-1`），不同时探测多个档位。
- **探测-验证流程**：
  1. **条件判定**：当升/降阶条件连续满足达到防抖阈值时，进入探测状态
  2. **发送探测帧**：下一轮整轮使用目标 MCS（`m_curr±1`）发送
  3. **更新统计**：根据探测结果更新目标 MCS 的统计数据（EWMA + 单调修正）
  4. **再次验证**：重新计算评分并判断升/降阶条件：
     - 仍满足 → 正式切换到目标 MCS，防抖计数清零
     - 不满足 → 回退到原 MCS，防抖计数清零

### 8. 参数默认值
- **EWMA**：`ALPHA=0.25`
- **聚类**：`ZETA=0.2`（簇规模下限），`BETA=1.0`（线性按人数加权）
- **评分**：`GAMMA=1.0`（线性评分），`R[m]` 净吞吐
- **收益阈值**：`Δ_up=0.05`，`Δ_down=0.05`
- **防抖（动态）**：
  - **降阶**：`HOLD_TIME_DOWN=2`（固定）
  - **升阶**：动态调整
    - MCS ≤ 6：`HOLD_TIME_UP=2`
    - MCS 7-8：`HOLD_TIME_UP=3`
    - MCS 9-10：`HOLD_TIME_UP=4`
    - MCS ≥ 11：`HOLD_TIME_UP=5`
- **无样本衰减**：`NO_SAMPLE_DECAY=0.90`
- **固定设定**：`BW=80MHz`，`NSS=2`，`default_mcs=6`，`m_safe_low=2`，`MCS_TABLE=0..11`。


