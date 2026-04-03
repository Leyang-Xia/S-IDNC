#include "mcs_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 本文件实现 MCS 控制器的核心算法：
 * - McsUpdateWithRound(): 统计更新（EWMA + 未采样老化）+ 单调性修正
 * - McsSelect(): 基于当前档位锚点成功率聚类，计算评分，并通过探测-验证状态机升/降档
 *
 * 重要约定：
 * - 成功率/权重/参数均为定点数（单位 FIXED_SCALE），例如 0.93 表示为 930。
 * - attemptsDelta/successRatio 为 user-major；内部 successRate 为 level-major（见 mcs_controller.h）。
 */

/* ========== 定点数辅助函数 ========== */

/* 定点数乘法：返回 (a * b) / FIXED_SCALE */
static inline int32_t FixedMul(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * (int64_t)b) / FIXED_SCALE);
}

/* 定点数除法：返回 (a * FIXED_SCALE) / b；b==0 时返回 0（调用方需自行评估是否可接受） */
static inline int32_t FixedDiv(int32_t a, int32_t b) {
    if (b == 0) return 0;
    return (int32_t)(((int64_t)a * FIXED_SCALE) / (int64_t)b);
}

/*
 * 定点数幂运算：base^exp（exp 为定点数，但仅支持“接近整数”的幂次）
 * - 典型用途：size^beta、succ^gamma，其中 beta/gamma 常设置为 1.0/2.0 等整数幂附近。
 * - exp 会按四舍五入转为整数 expInt；若传入非整数幂，结果仅是近似（并非连续幂函数）。
 * - 支持负整数幂：返回 1 / base^{|e|}（仍为定点数）。
 */
static int32_t FixedPow(int32_t base, int32_t exp) {
    /* exp 是定点数，检查是否为整数 */
    int32_t expInt = (exp + FIXED_SCALE/2) / FIXED_SCALE;
    
    if (expInt == 1) {
        return base;
    }
    
    int32_t result = FIXED_ONE;
    int32_t b = base;
    int e = expInt;
    
    if (e < 0) {
        /* 负幂次：1 / base^|e| */
        e = -e;
        while (e > 0) {
            if (e & 1) {
                result = FixedMul(result, b);
            }
            b = FixedMul(b, b);  /* 快速幂：平方并右移指数 */
            e >>= 1;
        }
        result = FixedDiv(FIXED_ONE, result);
    } else {
        while (e > 0) {
            if (e & 1) {
                result = FixedMul(result, b);
            }
            b = FixedMul(b, b);  /* 快速幂：平方并右移指数 */
            e >>= 1;
        }
    }
    
    return result;
}

/* 小规模数组排序：插入排序在 n<=8 这类场景简单且常数开销小 */
static void InsertionSort(int32_t *values, int n) {
    for (int i = 1; i < n; ++i) {
        int32_t key = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = key;
    }
}

/*
 * 已排序数组的分位数（线性插值）
 * - 输入必须已按升序排序（本函数不做排序）。
 * - p 为定点数比例（0..FIXED_ONE），例如 p=800 表示 80 分位。
 */
static int32_t PercentilePresorted(const int32_t *sortedValues, int n, int32_t p) {
    if (n <= 0) {
        return 0;
    }
    /* p 是定点数，计算 rank = p * (n - 1) / FIXED_SCALE */
    int64_t rank64 = ((int64_t)p * (n - 1)) / FIXED_SCALE;
    int low = (int)(rank64);
    int high = low + 1;
    if (high >= n) high = n - 1;
    if (low >= n) low = n - 1;
    
    /* weight = (rank - low) * FIXED_SCALE */
    int32_t weight = (int32_t)((rank64 * FIXED_SCALE) - (low * FIXED_SCALE));
    int32_t result = sortedValues[low];
    if (high > low) {
        /* result = sortedValues[low] * (1 - weight) + sortedValues[high] * weight */
        result = FixedMul(sortedValues[low], FIXED_ONE - weight) + 
                 FixedMul(sortedValues[high], weight);
    }
    return result;
}

/*
 * 单调性修正（按“低 MCS 更稳”约束）：
 * - 目标：对每个用户，使 successRate[m] 随 m 增大呈“非增”（低 MCS 的成功率 >= 高 MCS）。
 * - 设计取舍：本轮“已采样”的档位视为权威值，不对其下调；若采样值导致曲线违反单调性，
 *   则通过“抬高”更低档位的历史值来恢复单调性（保证采样结果不被覆盖）。
 * - 对“未采样”档位：用前缀最小值进行填充/压制，避免未观测档位凭空变得更好。
 *
 * attemptsDelta 为 user-major：attemptsDelta[user * levels + m] > 0 表示本轮该 m 被真实采样过。
 */
static void ApplyMonotonicUserSuccess(MCSState *state, int user, const int *attemptsDelta, int levels) {
    int mcs = state->mcsLevels;
    int32_t values[MCS_MAX_LEVELS];
    int sampledThisRound[MCS_MAX_LEVELS];  /* 当前轮次是否采样 */
    
    /* 读取当前状态 */
    for (int m = 0; m < mcs; ++m) {
        int idx = MCS_IDX(m, user);
        values[m] = state->successRate[idx];
        int idxDelta = user * levels + m;
        sampledThisRound[m] = attemptsDelta[idxDelta];  /* 当前轮次采样状态 */
    }
    
    /* 前缀最小填充逻辑 */
    /* minSeen 维护 [0..m-1] 的最小值；初始化为 >1 的值，确保第一个元素能正确落入更新逻辑 */
    int32_t minSeen = 11 * FIXED_SCALE / 10;  /* 1.1 的定点表示（大于合法成功率上限） */
    
    for (int m = 0; m < mcs; ++m) {
        if (sampledThisRound[m] > 0) {
            /* 已采样：保持该值为权威观测（不做下调） */
            int32_t v = values[m];
            
            /*
             * 若 v > minSeen，说明当前（更高 MCS）的成功率反而高于前缀最小值，会破坏“非增”约束。
             * 因为 v 是本轮采样得到的权威值，不能把 v 压低，所以选择把更低档位抬高到至少 v。
             */
            if (v > minSeen) {
                for (int i = 0; i < m; ++i) {
                    if (values[i] < v) {
                        values[i] = v;
                    }
                }
                minSeen = v;
            } else {
                minSeen = v;
            }
        } else {
            /* 未采样：用前缀最小值压制“凭空变好”的档位，保持曲线非增 */
            if (minSeen < values[m]) {  
                values[m] = minSeen;
            } else {
                minSeen = values[m];
            }
        }
    }
    
    /* 写回结果 */
    for (int m = 0; m < mcs; ++m) {
        state->successRate[MCS_IDX(m, user)] = values[m];
    }
}

/*
 * 计算簇在指定 MCS 档位 m 的代表成功率（用于评分）
 * - 当前实现使用均值（mean）。
 * - 注：保留了分位数（q80）方案的代码路径，便于后续切换到“对尾部更鲁棒”的代表统计量。
 */
static int32_t CalcClusterSuccessAt(const MCSState *state, const int *members, int size, int m) {
    int64_t sum = 0;
    for (int i = 0; i < size; ++i) {
        int32_t val = state->successRate[MCS_IDX(m, members[i])];
        sum += val;
    }
    // InsertionSort(buffer, size);
    // int32_t q80 = PercentilePresorted(buffer, size, 800);  /* 0.80 * FIXED_SCALE */
    int32_t mean = (int32_t)(sum / size);
    /* 0.7 * q80 + 0.3 * mean = (7 * q80 + 3 * mean) / 10 */
    // return (int32_t)(((int64_t)7 * q80 + (int64_t)3 * mean) / 10);
    return mean;
}

/*
 * 基于锚点成功率做 1D 聚类，并筛选"活跃簇"
 *
 * 输入 values：
 * - 通常取当前主用档位 mCurr 上，各用户的成功率 successRate[mCurr][u]。
 *
 * 聚类方法（1D、k=2/3）：
 * - 先对 values 升序排序；
 * - 以 SSE（组内平方误差）作为代价，在 1D 上穷举最优切分点（等价于 1D k-means 的最优切分解法之一）。
 * - 若 k=3 相比 k=2 的相对改进不足 10%，则退化为 k=2（避免过拟合/小簇抖动）。
 *
 * 活跃簇筛选：
 * - 仅保留 size/n >= zeta 的簇；若全部被过滤，回退到最大簇，保证至少一个簇参与评分。
 *
 * 注意：
 * - 本函数只负责聚类和成员分配，不计算权重。
 * - 权重应在调用方完成簇成功率预计算后，再调用 AssignClusterWeights() 完成。
 */
static void SelectActiveClusters(const int32_t *values, int n, const MCSConfig *cfg,
                                 MCSClusterSet *active) {
    /* 1) 排序并保存原始索引 */
    int32_t vSorted[MCS_MAX_USERS];
    int idxSorted[MCS_MAX_USERS];
    for (int i = 0; i < n; ++i) {
        vSorted[i] = values[i];
        idxSorted[i] = i;
    }
    for (int i = 1; i < n; ++i) {
        int32_t v = vSorted[i];
        int id = idxSorted[i];
        int j = i - 1;
        while (j >= 0 && vSorted[j] > v) {
            vSorted[j + 1] = vSorted[j];
            idxSorted[j + 1] = idxSorted[j];
            --j;
        }
        vSorted[j + 1] = v;
        idxSorted[j + 1] = id;
    }

    /* 2) 前缀和供 SSE 计算（p1: Σv，p2: Σv^2） */
    int64_t p1[MCS_MAX_USERS + 1];
    int64_t p2[MCS_MAX_USERS + 1];
    p1[0] = 0;
    p2[0] = 0;
    for (int i = 0; i < n; ++i) {
        int64_t v = vSorted[i];
        p1[i+1] = p1[i] + v;
        p2[i+1] = p2[i] + v * v;  /* 优化：避免重复读取vSorted[i] */
    }
    /* SSE(l,r) = p2[r+1]-p2[l] - (p1[r+1]-p1[l])^2 / (r-l+1) */
    /* 优化：直接计算避免宏展开开销 */
    #define SSE(l,r) \
        ((p2[(r)+1] - p2[(l)]) - ((p1[(r)+1] - p1[(l)]) * (p1[(r)+1] - p1[(l)])) / ((r)-(l)+1))

    /* 3) 穷举切分：k=2、k=3（n<=8 时成本可接受，换取确定性与可解释性） */
    int64_t best2 = INT64_MAX;
    int best2S = -1;
    for (int s = 1; s <= n-1; ++s) {
        int64_t cost = SSE(0,s-1) + SSE(s,n-1);  /* 切分点 s：左[0..s-1]，右[s..n-1] */
        if (cost < best2) {
            best2 = cost;
            best2S = s;
        }
    }
    int64_t best3 = INT64_MAX;
    int best3S = -1;
    int best3T = -1;
    if (n >= 3) {
        for (int s = 1; s <= n-2; ++s) {
            for (int t = s+1; t <= n-1; ++t) {
                int64_t cost = SSE(0,s-1) + SSE(s,t-1) + SSE(t,n-1);  /* s/t：三段切分边界 */
                if (cost < best3) {
                    best3 = cost;
                    best3S = s;
                    best3T = t;
                }
            }
        }
    }

    /* 4) 选择 k：若 k=3 的相对改进不足 10% 则用 k=2（防止过拟合） */
    int useK = 2;
    if (best3 < best2) {
        /* improve = (best2 - best3) / best2 >= 0.10 */
        if ((best2 - best3) * 10 >= best2) {
            useK = 3;
        }
    }

    /* 5) 生成簇：按 ζ 过滤活跃簇；若全被过滤则回退到最大簇 */
    if (useK == 2) {
        int sizes2[2] = { best2S, n - best2S };
        const int starts2[2] = { 0, best2S };
        int idx = 0;
        for (int c = 0; c < 2; ++c) {
            /* frac = size / n，比较 frac >= zeta，即 size * FIXED_SCALE >= zeta * n */
            if ((int64_t)sizes2[c] * FIXED_SCALE >= (int64_t)cfg->zeta * n) {
                active->sizes[idx] = sizes2[c];
                for (int i = 0; i < sizes2[c]; ++i) {
                    active->members[idx][i] = idxSorted[starts2[c] + i];
                }
                idx++;
            }
        }
        if (idx == 0) {
            int cmax = sizes2[0] >= sizes2[1] ? 0 : 1;
            active->sizes[0] = sizes2[cmax];
            for (int i = 0; i < sizes2[cmax]; ++i) {
                active->members[0][i] = idxSorted[starts2[cmax] + i];
            }
            idx = 1;
        }
        active->clusters = idx;
    } else {
        int sizes3[3] = { best3S, best3T - best3S, n - best3T };
        const int starts3[3] = { 0, best3S, best3T };
        int idx = 0;
        for (int c = 0; c < 3; ++c) {
            if (sizes3[c] <= 0) {
                continue;
            }
            if ((int64_t)sizes3[c] * FIXED_SCALE >= (int64_t)cfg->zeta * n) {
                active->sizes[idx] = sizes3[c];
                for (int i = 0; i < sizes3[c]; ++i) {
                    active->members[idx][i] = idxSorted[starts3[c] + i];
                }
                idx++;
            }
        }
        if (idx == 0) {
            int cmax = 0;
            for (int c = 1; c < 3; ++c) {
                if (sizes3[c] > sizes3[cmax]) {
                    cmax = c;
                }
            }
            active->sizes[0] = sizes3[cmax];
            for (int i = 0; i < sizes3[cmax]; ++i) {
                active->members[0][i] = idxSorted[starts3[cmax] + i];
            }
            idx = 1;
        }
        active->clusters = idx;
    }

    #undef SSE
}

/*
 * 为各簇分配权重，并基于成功率门限进行过滤
 *
 * 输入:
 * - cfg: 配置参数（beta、minSuccThreshold）
 * - active: 已完成聚类的簇集合（sizes/members 已填充）
 * - clusterSucc: 各簇的代表成功率（预计算好的，长度为 active->clusters）
 *
 * 输出:
 * - active->weights[] 被填充并归一化到 sum=FIXED_ONE
 *
 * 逻辑:
 * 1. 基于 size^beta 计算初始权重
 * 2. 若簇成功率 < minSuccThreshold，权重置0（过滤低质量簇）
 * 3. 若所有簇都被过滤，保留成功率最高的簇（保证至少一个簇参与评分）
 * 4. 归一化到 sum(weights)=FIXED_ONE
 */
static void AssignClusterWeights(const MCSConfig *cfg, MCSClusterSet *active,
                                 const int32_t *clusterSucc) {
    /* 1) 基于 size^beta 计算初始权重 */
    for (int c = 0; c < active->clusters; ++c) {
        int32_t sizeFixed = active->sizes[c] * FIXED_SCALE;
        int32_t w = (cfg->beta == FIXED_ONE) ? active->sizes[c] : FixedPow(sizeFixed, cfg->beta);
        active->weights[c] = w;
    }

    /* 2) 基于成功率门限过滤：低于门限的簇权重置0 */
    for (int c = 0; c < active->clusters; ++c) {
        if (clusterSucc[c] < CLUSTER_SUCCESS_RATE_THRESHOLD) {
            active->weights[c] = 0;
        }
    }

    /* 3) 若所有簇权重都为0，保留成功率最高的簇 */
    int64_t weightSum = 0;
    for (int c = 0; c < active->clusters; ++c) {
        weightSum += active->weights[c];
    }
    if (weightSum <= 0) {
        /* 找到成功率最高的簇 */
        int bestCluster = 0;
        int32_t bestSucc = clusterSucc[0];
        for (int c = 1; c < active->clusters; ++c) {
            if (clusterSucc[c] > bestSucc) {
                bestSucc = clusterSucc[c];
                bestCluster = c;
            }
        }
        /* 恢复该簇权重（按 size^beta） */
        int32_t sizeFixed = active->sizes[bestCluster] * FIXED_SCALE;
        active->weights[bestCluster] = (cfg->beta == FIXED_ONE) ? 
            active->sizes[bestCluster] : FixedPow(sizeFixed, cfg->beta);
        weightSum = active->weights[bestCluster];
    }
    
    /* 4) 归一化到 sum(weights)=FIXED_ONE */
    for (int c = 0; c < active->clusters; ++c) {
        active->weights[c] = (int32_t)(((int64_t)active->weights[c] * FIXED_SCALE) / weightSum);
    }
}

/*
 * 动态升档防抖阈值：
 * - 越高的 MCS 越"激进"，需要更长的连续优势轮数才触发探测，降低误升档风险。
 * - 该函数只影响"触发升档探测"的门槛，不直接决定最终切换（最终由验证结果决定）。
 */
static int GetHoldTimeUp(int targetMcs) {
    if (targetMcs <= 6) {
        return 2;
    } else if (targetMcs <= 8) {
        return 3;
    } else if (targetMcs <= 10) {
        return 4;
    } else {
        return 5;
    }
}

/*
 * 计算指定档位 m 的聚合评分
 * - score = Σ_c rate[m] * weight[c] * f(succ_c[m])
 * - f(x) = x^gamma；当 gamma=1 时退化为线性（避免不必要的幂运算）
 *
 * 说明：
 * - clusterSuccCache 可传入预计算的簇成功率（减少重复统计开销）。
 * - 返回值使用 int64 以避免 rate/weight/succ 乘法溢出；量纲上可理解为“速率加权的成功率”。
 */
static int64_t ComputeScore(const MCSState *state, const MCSConfig *cfg, const MCSClusterSet *clusters, int m, 
                            const int32_t *clusterSuccCache) {
    if (m < 0 || m >= state->mcsLevels) {
        return INT64_MIN;
    }
    int64_t score = 0;
    int32_t rate = state->rTable[m];
    
    if (cfg->gamma == FIXED_ONE) {
        /* gamma = 1.0，不需要幂运算 */
        for (int c = 0; c < clusters->clusters; ++c) {
            int32_t succ = clusterSuccCache ? clusterSuccCache[c] : 
                          CalcClusterSuccessAt(state, clusters->members[c], clusters->sizes[c], m);
            /* score += rate * weight * succ */
            int64_t term = (int64_t)rate * clusters->weights[c];
            term = (term * succ) / FIXED_SCALE;  /* weight/succ 为定点数：乘完需要除回一次 */
            score += term;
        }
    } else {
        for (int c = 0; c < clusters->clusters; ++c) {
            int32_t succ = clusterSuccCache ? clusterSuccCache[c] : 
                          CalcClusterSuccessAt(state, clusters->members[c], clusters->sizes[c], m);
            int32_t succPow = FixedPow(succ, cfg->gamma);
            /* score += rate * weight * succ^gamma */
            int64_t term = (int64_t)rate * clusters->weights[c];
            term = (term * succPow) / FIXED_SCALE;  /* succPow 仍是定点数 */
            score += term;
        }
    }
    return score;
}

/* ---------- 对外接口 ---------- */

void McsInitState(MCSState *state, int userCount, int mcsLevels, const int32_t *initSuccess) {
    state->userCount = userCount;
    state->mcsLevels = mcsLevels;
    /* 初始主用档位：可根据系统经验值调整；本实现默认从 MCS 6 起步 */
    state->mCurr = 6;
    state->holdCounterUp = 0;
    state->holdCounterDown = 0;
    state->probeState = MCS_PROBE_NORMAL;
    state->probeTarget = -1;
    state->probeStateCount = 0;
    for (int u = 0; u < MCS_MAX_USERS; ++u) {
        state->probeFeedbackReceived[u] = 0;
    }
    
    MCS_LOG_INIT("Initialize: users=%d, levels=%d, initMCS=%d\n", 
                 userCount, mcsLevels, state->mCurr);
    int32_t fallback[MCS_MAX_LEVELS] = {
        900, 845, 790, 735, 680, 625, 570, 515, 460, 405, 350, 300
    };
    
    for (int u = 0; u < userCount; ++u) {
        for (int m = 0; m < mcsLevels; ++m) {
            int idx = MCS_IDX(m, u);
            const int32_t *src = initSuccess ? initSuccess : fallback;
            /* initSuccess/fallback 均以定点数表示成功率（0..FIXED_ONE），按 mcsLevels 取用 */
            state->successRate[idx] = src[m];
        }
    }
}

int McsSelect(const MCSConfig *cfg, MCSState *state) {
    int users = state->userCount;
    int levels = state->mcsLevels;
    int prevMcs = state->mCurr;
    int mReturn = state->mCurr;  /* 默认返回当前 MCS */

    /*
     * 1) 以当前主用档位 mCurr 的用户成功率作为“锚点”，对用户做 1D 聚类
     *    直觉：同一档位下成功率相近的用户更可能具有相似链路/干扰条件，聚合评分更稳健。
     */
    int32_t anchor[MCS_MAX_USERS];
    for (int u = 0; u < users; ++u) {
        anchor[u] = state->successRate[MCS_IDX(state->mCurr, u)];
    }

    /* 2) 聚类后，对候选档位（当前/上/下）计算评分 */
    MCSClusterSet clusters;
    SelectActiveClusters(anchor, users, cfg, &clusters);
    
    /* 预计算：当前档位的簇代表成功率（后续日志/验证会重复用到） */
    int32_t clusterSuccCache[MCS_MAX_CLUSTERS];
    for (int c = 0; c < clusters.clusters; ++c) {
        clusterSuccCache[c] = CalcClusterSuccessAt(state, clusters.members[c], clusters.sizes[c], state->mCurr);
    }

    /* 基于预计算的簇成功率分配权重（含成功率门限过滤） */
    AssignClusterWeights(cfg, &clusters, clusterSuccCache);

    int64_t scoreCurr = ComputeScore(state, cfg, &clusters, state->mCurr, clusterSuccCache);
    
    /*
     * 相邻档位预计算：
     * - 仅对 mCurr±1 计算，避免全量遍历所有档位带来的开销。
     * - 上探限制到 MCS 9：属于业务策略（例如高于 9 的档位收益/风险不成比例）。
     */
    int32_t clusterSuccUp[MCS_MAX_CLUSTERS];
    int32_t clusterSuccDown[MCS_MAX_CLUSTERS];
    for (int c = 0; c < clusters.clusters; ++c) {
        if (state->mCurr + 1 < levels && state->mCurr + 1 <= 9) {
            clusterSuccUp[c] = CalcClusterSuccessAt(state, clusters.members[c], clusters.sizes[c], state->mCurr + 1);
        }
        if (state->mCurr - 1 >= 0) {
            clusterSuccDown[c] = CalcClusterSuccessAt(state, clusters.members[c], clusters.sizes[c], state->mCurr - 1);
        }
    }
    
    int64_t scoreUp = INT64_MIN;
    if (state->mCurr + 1 < levels && state->mCurr + 1 <= 9) {
        scoreUp = ComputeScore(state, cfg, &clusters, state->mCurr + 1, clusterSuccUp);
    }
    int64_t scoreDown = ComputeScore(state, cfg, &clusters, state->mCurr - 1, 
                                     (state->mCurr - 1 >= 0) ? clusterSuccDown : NULL);

    /* 聚类信息 */
    MCS_LOG_CLUSTER("mCurr=%d, clusters=%d, scores: curr=%lld, up=%lld, down=%lld\n",
                    state->mCurr, clusters.clusters,
                    (long long)scoreCurr, (long long)scoreUp, (long long)scoreDown);
    for (int c = 0; c < clusters.clusters; ++c) {
#if MCS_DEBUG
        /* 使用已缓存的成功率，避免重复计算 */
        int32_t cSucc = clusterSuccCache[c];
        MCS_LOG_CLUSTER("  Cluster[%d]: size=%d, weight=%d, succ@%d=%d, members=[",
                        c, clusters.sizes[c], clusters.weights[c], 
                        state->mCurr, cSucc);
        for (int i = 0; i < clusters.sizes[c]; ++i) {
            MCS_LOG("%d%s", clusters.members[c][i], i < clusters.sizes[c]-1 ? "," : "");
        }
        MCS_LOG("]\n");
#else
        (void)c;  /* Suppress unused variable warning */
#endif
    }

    /*
     * 3) 探测-验证状态机
     *
     * 关键点：
     * - “触发探测”只表示下一轮去发探测档位的帧（mReturn=probeTarget），并不立即更新 mCurr；
     * - “验证切换”发生在收齐所有用户在 probeTarget 的反馈之后：比较 scoreTarget vs scoreCurr。
     *
     * 注意：probeStateCount/MAX_PROBE_STATE_COUNT 目前未在此处强制使用；
     * 若存在反馈长期缺失的可能，可在此加入超时回退逻辑，避免卡在探测状态。
     */
    if (state->probeState == MCS_PROBE_UP) {
        /* 检查是否收齐了所有用户的feedback */
        int allFeedbackReceived = 1;
        for (int u = 0; u < users; ++u) {
            if (!state->probeFeedbackReceived[u]) {
                allFeedbackReceived = 0;
                break;
            }
        }
        
        if (!allFeedbackReceived) {
            /* 没有收齐所有用户的feedback，返回原来的MCS */
            MCS_LOG_PROBE("Up-shift probe: feedback not complete, use original MCS %d\n", state->mCurr);
            mReturn = state->mCurr;
        } else {
            /* 收齐了所有用户的feedback，进行验证 */
            /* 验证升档探测结果：使用探测前的聚类（基于 mCurr），但用更新后的数据 */
            /* 需要重新计算探测目标档位的簇成功率 */
            int32_t clusterSuccTarget[MCS_MAX_CLUSTERS];
            for (int c = 0; c < clusters.clusters; ++c) {
                clusterSuccTarget[c] = CalcClusterSuccessAt(state, clusters.members[c], clusters.sizes[c], state->probeTarget);
            }
            int64_t scoreTarget = ComputeScore(state, cfg, &clusters, state->probeTarget, clusterSuccTarget);
            MCS_LOG_PROBE("Verify up-shift: %d->%d, scoreTarget=%lld, scoreCurr=%lld, %s\n",
                   prevMcs,
                   state->probeTarget, (long long)scoreTarget, (long long)scoreCurr,
                   scoreTarget >= scoreCurr ? "PASS" : "FAIL");
            if (scoreTarget >= scoreCurr) {
                /* 升档成功 */
                state->mCurr = state->probeTarget;
                MCS_LOG_PROBE("Up-shift SUCCESS: %d -> %d\n", prevMcs, state->mCurr);        
            } else {
                MCS_LOG_PROBE("Up-shift FAILED, rollback to %d\n", state->mCurr);
            }
            /* 无论成功与否，重置状态 */
            state->holdCounterUp = 0;
            state->holdCounterDown = 0;
            state->probeState = MCS_PROBE_NORMAL;
            state->probeTarget = -1;
            for (int u = 0; u < MCS_MAX_USERS; ++u) {
                state->probeFeedbackReceived[u] = 0;
            }
            mReturn = state->mCurr;
        }
        
    } else if (state->probeState == MCS_PROBE_DOWN) {
        /* 检查是否收齐了所有用户的feedback */
        int allFeedbackReceived = 1;
        for (int u = 0; u < users; ++u) {
            if (!state->probeFeedbackReceived[u]) {
                allFeedbackReceived = 0;
                break;
            }
        }
        
        if (!allFeedbackReceived) {
            /* 没有收齐所有用户的feedback，返回原来的MCS */
            MCS_LOG_PROBE("Down-shift probe: feedback not complete, use original MCS %d\n", state->mCurr);
            mReturn = state->mCurr;
        } else {
            /* 收齐了所有用户的feedback，进行验证 */
            /* 验证降档探测结果：使用探测前的聚类（基于 mCurr），但用更新后的数据 */
            /* 需要重新计算探测目标档位的簇成功率 */
            int32_t clusterSuccTarget[MCS_MAX_CLUSTERS];
            for (int c = 0; c < clusters.clusters; ++c) {
                clusterSuccTarget[c] = CalcClusterSuccessAt(state, clusters.members[c], clusters.sizes[c], state->probeTarget);
            }
            int64_t scoreTarget = ComputeScore(state, cfg, &clusters, state->probeTarget, clusterSuccTarget);
            MCS_LOG_PROBE("Verify down-shift: %d->%d, scoreTarget=%lld, scoreCurr=%lld, %s\n",
                   prevMcs,
                   state->probeTarget, (long long)scoreTarget, (long long)scoreCurr,
                   scoreTarget >= scoreCurr ? "PASS" : "FAIL");
            if (scoreTarget >= scoreCurr) {
                /* 降档成功 */
                state->mCurr = state->probeTarget;
                MCS_LOG_PROBE("Down-shift SUCCESS: %d -> %d\n", prevMcs, state->mCurr);
            } else {
                MCS_LOG_PROBE("Down-shift FAILED, keep %d\n", state->mCurr);
            }
            /* 无论成功与否，重置状态 */
            state->holdCounterDown = 0;
            state->holdCounterUp = 0;
            state->probeState = MCS_PROBE_NORMAL;
            state->probeTarget = -1;
            for (int u = 0; u < MCS_MAX_USERS; ++u) {
                state->probeFeedbackReceived[u] = 0;
            }
            mReturn = state->mCurr;
        }
        
    } else {
        /* NORMAL 状态：评估是否进入探测（需要连续多轮优势，避免单轮波动触发） */
        
        /* 升档判定 - 限制最高到MCS 9 */
        int mNext = state->mCurr + 1;
        if (mNext < levels && mNext <= 9) {  /* 限制最高MCS为9 */
            MCS_LOG_SELECT("Up-shift check: mNext=%d, scoreUp=%lld, scoreCurr=%lld, condition: %s\n",
                          mNext, (long long)scoreUp, (long long)scoreCurr,
                          scoreUp >= scoreCurr ? "met" : "not met");
            
            if (scoreUp >= scoreCurr) {
                state->holdCounterUp += 1;
                
                int holdTimeUp = GetHoldTimeUp(mNext);
                MCS_LOG_SELECT("Up-shift counter: %d/%d\n",
                              state->holdCounterUp, holdTimeUp);
                
                if (state->holdCounterUp >= holdTimeUp) {
                    /* 进入升档探测状态 */
                    MCS_LOG_PROBE("Trigger up-shift probe: %d -> %d\n", state->mCurr, mNext);
                    state->probeState = MCS_PROBE_UP;
                    state->probeTarget = mNext;
                    /* 初始化反馈收集位图：所有用户都未收到反馈 */
                    for (int u = 0; u < users; ++u) {
                        state->probeFeedbackReceived[u] = 0;
                    }
                    mReturn = mNext;  /* 下一轮发送探测帧；mCurr 仍保持不变，等待验证 */
                }
            } else {
                state->holdCounterUp = 0;
            }
        }
        
        /* 降档判定 */
        int mPrev = state->mCurr - 1;
        if (mPrev >= 0) {
            MCS_LOG_SELECT("Down-shift check: mPrev=%d, scoreDown=%lld, scoreCurr=%lld, condition: %s\n",
                          mPrev, (long long)scoreDown, (long long)scoreCurr,
                          scoreDown >= scoreCurr ? "met" : "not met");
            
            if (scoreDown >= scoreCurr) {
                state->holdCounterDown += 1;
                
                MCS_LOG_SELECT("Down-shift counter: %d/%d\n",
                              state->holdCounterDown, cfg->holdTimeDown);
                
                if (state->holdCounterDown >= cfg->holdTimeDown) {
                    /* 进入降档探测状态 */
                    MCS_LOG_PROBE("Trigger down-shift probe: %d -> %d\n", state->mCurr, mPrev);
                    state->probeState = MCS_PROBE_DOWN;
                    state->probeTarget = mPrev;
                    /* 初始化反馈收集位图：所有用户都未收到反馈 */
                    for (int u = 0; u < users; ++u) {
                        state->probeFeedbackReceived[u] = 0;
                    }
                    mReturn = mPrev;  /* 下一轮发送探测帧；mCurr 仍保持不变，等待验证 */
                }
            } else {
                state->holdCounterDown = 0;
            }
        }
    }

    /* 决策信息 */
    MCS_LOG_SELECT("Decision: prevMcs=%d, mCurr=%d, mReturn=%d, holdUp=%d, holdDown=%d\n",
                   prevMcs, state->mCurr, mReturn,
                   state->holdCounterUp, state->holdCounterDown);
    MCS_LOG_SELECT("  Scores: curr=%lld, up=%lld, down=%lld\n",
                   (long long)scoreCurr, (long long)scoreUp, (long long)scoreDown);

    return mReturn;
}


void McsUpdateWithRound(MCSState *state, const MCSConfig *cfg, const int *attemptsDelta, const int32_t *successRatio) {
    int users = state->userCount;
    int levels = state->mcsLevels;
    
    MCS_LOG_UPDATE("Update statistics (EWMA + monotonic correction)\n");
    
    for (int u = 0; u < users; ++u) {
        for (int m = 0; m < levels; ++m) {
            int idxDelta = u * levels + m;
            int inc = attemptsDelta[idxDelta];
            int idx = MCS_IDX(m, u);
            if (inc > 0) {
                /*
                 * 本轮有真实采样：
                 * - 若该采样发生在 probeTarget，则视为该用户对探测档位的反馈已到达。
                 * - 使用 EWMA 融合历史成功率与本轮观测，降低瞬时波动影响。
                 */
                if ((state->probeState == MCS_PROBE_UP || state->probeState == MCS_PROBE_DOWN) 
                    && m == state->probeTarget) {
                    state->probeFeedbackReceived[u] = 1;
                    MCS_LOG_UPDATE("  User%d MCS%d: feedback received for probe target\n", u, m);
                }
                int32_t recent = successRatio[idxDelta];
#if MCS_DEBUG
                int32_t oldRate = state->successRate[idx];
#endif
                /* EWMA: old * alpha + new * (1 - alpha) */
                state->successRate[idx] = FixedMul(cfg->alpha, state->successRate[idx]) + 
                                          FixedMul(FIXED_ONE - cfg->alpha, recent);
                MCS_LOG_UPDATE("  User%d MCS%d: sampled, old=%d, recent=%d, new=%d\n",
                              u, m, oldRate, recent, state->successRate[idx]);
            } else {
                /*
                 * 本轮未采样：
                 * - 对 PER 做衰减（即 successRate 逐步“变好”一些），模拟“随着时间推移误码趋缓”的保守假设；
                 * - 但会设置上限，避免未观测档位无限乐观；
                 * - 若正在探测，为避免“等反馈期间用老化把候选档位抬高”，会跳过对应方向的老化。
                 */
                int shouldDecay = 1;
                if (state->probeState == MCS_PROBE_UP && m > state->mCurr) {
                    /* 升档探测：暂停所有高于当前MCS的档位的老化 */
                    shouldDecay = 0;
                    MCS_LOG_UPDATE("  User%d MCS%d: up-shift probe in progress (waiting feedback), skip decay\n", u, m);
                } else if (state->probeState == MCS_PROBE_DOWN && m < state->mCurr) {
                    /* 降档探测：暂停所有低于当前MCS的档位的老化 */
                    shouldDecay = 0;
                    MCS_LOG_UPDATE("  User%d MCS%d: down-shift probe in progress (waiting feedback), skip decay\n", u, m);
                }
                
                if (shouldDecay) {
                    int32_t per = FIXED_ONE - state->successRate[idx];
                    per = FixedMul(per, cfg->noSampleDecay);          /* PER 衰减 => 成功率上升 */
                    state->successRate[idx] = FIXED_ONE - per;        /* succ = 1 - PER */
                    
                    /* 未采样上限：用于把高档位维持在“需要真实样本才能变好”的区间 */
                    if (m == 8) {
                        /* MCS 8：成功率上限较低（示例策略：<=0.75） */
                        if (state->successRate[idx] > 750) {
                            state->successRate[idx] = 750;
                        }
                    } else if (m == 9) {
                        /* MCS 9：更激进档位，上限更低（示例策略：<=0.30） */
                        if (state->successRate[idx] > 300) {
                            state->successRate[idx] = 300;
                        }
                    } else {
                        /* 其他档位：允许更高上限，但仍禁止接近 1.0 的“无依据乐观” */
                        if (state->successRate[idx] > 990) {
                            state->successRate[idx] = 990;
                        }
                    }
                }
            }
        }
        /* 每个用户更新完成后统一做单调性修正，避免跨档位的不合理曲线影响下一轮聚类与评分 */
        ApplyMonotonicUserSuccess(state, u, attemptsDelta, levels);
        
#if MCS_DEBUG
        /* 单调性修正后的各阶成功率 */
        MCS_LOG_UPDATE("  User%d after monotonic correction: ", u);
        for (int m = 0; m < levels; ++m) {
            int idx = MCS_IDX(m, u);
            printf("MCS%d=%d%s", m, state->successRate[idx], 
                   m < levels-1 ? ", " : "");
        }
        printf("\n");
#endif
    }
}
