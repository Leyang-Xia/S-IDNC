#include "mcs_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========== 定点数辅助函数 ========== */

/* 定点数乘法：(a * b) / FIXED_SCALE */
static inline int32_t FixedMul(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * (int64_t)b) / FIXED_SCALE);
}

/* 定点数除法：(a * FIXED_SCALE) / b */
static inline int32_t FixedDiv(int32_t a, int32_t b) {
    if (b == 0) return 0;
    return (int32_t)(((int64_t)a * FIXED_SCALE) / (int64_t)b);
}

/* 定点数幂运算（仅支持小整数幂次）*/
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
            b = FixedMul(b, b);
            e >>= 1;
        }
        result = FixedDiv(FIXED_ONE, result);
    } else {
        while (e > 0) {
            if (e & 1) {
                result = FixedMul(result, b);
            }
            b = FixedMul(b, b);
            e >>= 1;
        }
    }
    
    return result;
}

/* 对簇进行插入排序 */
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

/* 已排序数组的分位数，线性插值 */
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
    
    /* weight = (rank - low) * FIXED_SCALE，即小数部分 */
    int32_t weight = (int32_t)((rank64 * FIXED_SCALE) - (low * FIXED_SCALE));
    int32_t result = sortedValues[low];
    if (high > low) {
        /* result = sortedValues[low] * (1 - weight) + sortedValues[high] * weight */
        result = FixedMul(sortedValues[low], FIXED_ONE - weight) + 
                 FixedMul(sortedValues[high], weight);
    }
    return result;
}

/* 前缀最小填充：高档未采样时继承已观测的最低成功率 */
static void PrefixMinFill(const int32_t *values, const int *attempts, int n, int32_t *out) {
    int32_t minSeen = 11 * FIXED_SCALE / 10;  /* 1.1 的定点表示 */
    int hasSeen = 0;
    for (int m = 0; m < n; ++m) {
        if (attempts[m] > 0) {
            int32_t v = values[m];
            if (!hasSeen || v < minSeen) {
                minSeen = v;
                hasSeen = 1;
            }
            out[m] = v;
        } else if (hasSeen) {
            out[m] = minSeen;
        } else {
            out[m] = values[m];
        }
    }
}

/* PAV算法实现 */
static void PavNonIncreasing(const int32_t *input, const int32_t *weights, int n, int32_t *output) {
    int32_t *y = (int32_t *)malloc(sizeof(int32_t) * n);
    int32_t *w = (int32_t *)malloc(sizeof(int32_t) * n);
    int *startPos = (int *)malloc(sizeof(int) * n);
    int *endPos = (int *)malloc(sizeof(int) * n);
    
    int m = -1;
    for (int i = 0; i < n; ++i) {
        int32_t val = input[i];
        int32_t wt = weights ? weights[i] : FIXED_ONE;
        ++m;
        y[m] = val;
        w[m] = wt;
        startPos[m] = i;
        endPos[m] = i;
        
        while (m > 0 && y[m - 1] < y[m]) {
            /* 加权平均：(y[m-1]*w[m-1] + y[m]*w[m]) / (w[m-1] + w[m]) */
            int64_t numerator = (int64_t)y[m - 1] * w[m - 1] + (int64_t)y[m] * w[m];
            int32_t denom = w[m - 1] + w[m];
            y[m - 1] = (int32_t)(numerator / denom);
            w[m - 1] = denom;
            endPos[m - 1] = endPos[m];
            --m;
        }
    }
    
    /* 逆向填充 */
    for (int block = 0; block <= m; ++block) {
        int32_t value = y[block];
        for (int pos = startPos[block]; pos <= endPos[block]; ++pos) {
            output[pos] = value;
        }
    }
    
    free(y);
    free(w);
    free(startPos);
    free(endPos);
}

/* 对单个用户执行"前缀最高 + PAV" 修正 */
static void ApplyMonotonicUserSuccess(MCSState *state, int user) {
    int mcs = state->mcsLevels;
    int32_t sPref[MCS_MAX_LEVELS];
    int32_t weights[MCS_MAX_LEVELS];
    int32_t src[MCS_MAX_LEVELS];
    int att[MCS_MAX_LEVELS];
    for (int m = 0; m < mcs; ++m) {
        int idx = MCS_IDX(m, user);
        src[m] = state->successRate[idx];
        att[m] = state->attempts[idx];
    }
    PrefixMinFill(src, att, mcs, sPref);
    for (int m = 0; m < mcs; ++m) {
        weights[m] = att[m] > 0 ? att[m] : FIXED_ONE;
    }
    int32_t corrected[MCS_MAX_LEVELS];
    PavNonIncreasing(sPref, weights, mcs, corrected);
    for (int m = 0; m < mcs; ++m) {
        state->successRate[MCS_IDX(m, user)] = corrected[m];
    }
}

/* 计算簇在MCS档位m的代表成功率 */
static int32_t CalcClusterSuccessAt(const MCSState *state, const int *members, int size, int m) {
    int32_t buffer[MCS_MAX_USERS];
    int64_t sum = 0;
    for (int i = 0; i < size; ++i) {
        int32_t val = state->successRate[MCS_IDX(m, members[i])];
        buffer[i] = val;
        sum += val;
    }
    InsertionSort(buffer, size);
    int32_t q80 = PercentilePresorted(buffer, size, 800);  /* 0.80 * FIXED_SCALE */
    int32_t mean = (int32_t)(sum / size);
    /* 0.7 * q80 + 0.3 * mean */
    return FixedMul(700, q80) + FixedMul(300, mean);
}

/* 根据锚定成功率对用户聚类，并筛选出活跃簇 */
static void SelectActiveClusters(const int32_t *values, int n, const MCSConfig *cfg, MCSClusterSet *active) {
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

    /* 2) 前缀和供 SSE 计算 */
    int64_t p1[MCS_MAX_USERS + 1];
    int64_t p2[MCS_MAX_USERS + 1];
    p1[0] = 0;
    p2[0] = 0;
    for (int i = 0; i < n; ++i) {
        p1[i+1] = p1[i] + vSorted[i];
        p2[i+1] = p2[i] + (int64_t)vSorted[i] * vSorted[i];
    }
    /* SSE(l,r) = p2[r+1]-p2[l] - (p1[r+1]-p1[l])^2 / (r-l+1) */
    #define SSE(l,r) ( (p2[(r)+1]-p2[(l)]) - ((p1[(r)+1]-p1[(l)])*(p1[(r)+1]-p1[(l)]))/((r)-(l)+1) )

    /* 3) k=2、k=3 穷举切分 */
    int64_t best2 = INT64_MAX;
    int best2S = -1;
    for (int s = 1; s <= n-1; ++s) {
        int64_t cost = SSE(0,s-1) + SSE(s,n-1);
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
                int64_t cost = SSE(0,s-1) + SSE(s,t-1) + SSE(t,n-1);
                if (cost < best3) {
                    best3 = cost;
                    best3S = s;
                    best3T = t;
                }
            }
        }
    }

    /* 4) 选择 k，若 k=3 的相对改进不足 10% 则用 k=2 */
    int useK = 2;
    if (best3 < best2) {
        /* improve = (best2 - best3) / best2 >= 0.10 */
        if ((best2 - best3) * 10 >= best2) {
            useK = 3;
        }
    }

    /* 5) 生成簇（按 ζ 过滤），不足则取最大簇 */
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

    /* 6) 权重：w_c ∝ size(c)^β，并一次归一化 */
    int64_t weightSum = 0;
    for (int c = 0; c < active->clusters; ++c) {
        /* w = size^beta，beta 为定点数 */
        int32_t sizeFixed = active->sizes[c] * FIXED_SCALE;
        int32_t w = cfg->beta == 1000 ? active->sizes[c] : FixedPow(sizeFixed, cfg->beta);  
        active->weights[c] = w;
        weightSum += w;
    }
    if (weightSum <= 0) {
        weightSum = FIXED_SCALE;
    }
    for (int c = 0; c < active->clusters; ++c) {
        active->weights[c] = (int32_t)(((int64_t)active->weights[c] * FIXED_SCALE) / weightSum);
    }
    #undef SSE
}

/* 动态计算升档防抖阈值 */
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

/* 计算指定档位的聚合评分 */
static int64_t ComputeScore(const MCSState *state, const MCSConfig *cfg, const MCSClusterSet *clusters, int m) {
    if (m < 0 || m >= state->mcsLevels) {
        return INT64_MIN;
    }
    int64_t score = 0;
    int32_t rate = state->rTable[m];
    
    if (cfg->gamma == FIXED_ONE) {
        /* gamma = 1.0，不需要幂运算 */
        for (int c = 0; c < clusters->clusters; ++c) {
            int32_t succ = CalcClusterSuccessAt(state, clusters->members[c], clusters->sizes[c], m);
            /* score += rate * weight * succ */
            int64_t term = (int64_t)rate * clusters->weights[c];
            term = (term * succ) / FIXED_SCALE;
            score += term;
        }
    } else {
        for (int c = 0; c < clusters->clusters; ++c) {
            int32_t succ = CalcClusterSuccessAt(state, clusters->members[c], clusters->sizes[c], m);
            int32_t succPow = FixedPow(succ, cfg->gamma);
            /* score += rate * weight * succ^gamma */
            int64_t term = (int64_t)rate * clusters->weights[c];
            term = (term * succPow) / FIXED_SCALE;
            score += term;
        }
    }
    return score;
}

/* ---------- 对外接口 ---------- */

void McsInitState(MCSState *state, int userCount, int mcsLevels, const int32_t *initSuccess) {
    state->userCount = userCount;
    state->mcsLevels = mcsLevels;
    state->mCurr = 6;
    state->holdCounterUp = 0;
    state->holdCounterDown = 0;
    state->probeState = MCS_PROBE_NORMAL;
    state->probeTarget = -1;
    
    MCS_LOG_INIT("Initialize: users=%d, levels=%d, initMCS=%d\n", 
                 userCount, mcsLevels, state->mCurr);
    
    int32_t fallback[MCS_MAX_LEVELS];
    if (!initSuccess) {
        /* 从 0.90 到 0.30 的线性下降 */
        const int32_t start = 900;   /* 0.90 * FIXED_SCALE */
        const int32_t end = 300;     /* 0.40 * FIXED_SCALE */
        const int32_t step = (mcsLevels > 1) ? (start - end) / (mcsLevels - 1) : 0;

        for (int m = 0; m < mcsLevels; ++m) {
            int32_t value = start - step * m;
            fallback[m] = value;
        }
    }
    
    for (int u = 0; u < userCount; ++u) {
        for (int m = 0; m < mcsLevels; ++m) {
            int idx = MCS_IDX(m, u);
            const int32_t *src = initSuccess ? initSuccess : fallback;
            state->successRate[idx] = src[m];
            state->attempts[idx] = 0;
        }
    }
}

int McsSelect(const MCSConfig *cfg, MCSState *state) {
    int users = state->userCount;
    int levels = state->mcsLevels;
    int prevMcs = state->mCurr;
    int mReturn = state->mCurr;  /* 默认返回当前 MCS */

    /*  基于当前 mCurr 的成功率进行聚类 */
    int32_t anchor[MCS_MAX_USERS];
    for (int u = 0; u < users; ++u) {
        anchor[u] = state->successRate[MCS_IDX(state->mCurr, u)];
    }

    /* 聚类并计算评分 */
    MCSClusterSet clusters;
    SelectActiveClusters(anchor, users, cfg, &clusters);
    int64_t scoreCurr = ComputeScore(state, cfg, &clusters, state->mCurr);
    int64_t scoreUp = ComputeScore(state, cfg, &clusters, state->mCurr + 1);
    int64_t scoreDown = ComputeScore(state, cfg, &clusters, state->mCurr - 1);

    /* 聚类信息 */
    MCS_LOG_CLUSTER("mCurr=%d, clusters=%d, scores: curr=%lld, up=%lld, down=%lld\n",
                    state->mCurr, clusters.clusters,
                    (long long)scoreCurr, (long long)scoreUp, (long long)scoreDown);
    for (int c = 0; c < clusters.clusters; ++c) {
#if MCS_DEBUG
        int32_t cSucc = CalcClusterSuccessAt(state, clusters.members[c], clusters.sizes[c], state->mCurr);
        MCS_LOG_CLUSTER("  Cluster[%d]: size=%d, weight=%.3f, succ@%d=%.3f, members=[",
                        c, clusters.sizes[c], clusters.weights[c]/1000.0, 
                        state->mCurr, cSucc/1000.0);
        for (int i = 0; i < clusters.sizes[c]; ++i) {
            MCS_LOG("%d%s", clusters.members[c][i], i < clusters.sizes[c]-1 ? "," : "");
        }
        MCS_LOG("]\n");
#else
        (void)c;  /* Suppress unused variable warning */
#endif
    }

    /* 探测-验证状态机 */
    if (state->probeState == MCS_PROBE_UP) {
        /* 验证升档探测结果：使用探测前的聚类（基于 mCurr），但用更新后的数据 */
        int64_t scoreTarget = ComputeScore(state, cfg, &clusters, state->probeTarget);
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
        mReturn = state->mCurr;
        
    } else if (state->probeState == MCS_PROBE_DOWN) {
        /* 验证降档探测结果：使用探测前的聚类（基于 mCurr），但用更新后的数据 */
        int64_t scoreTarget = ComputeScore(state, cfg, &clusters, state->probeTarget);
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
        mReturn = state->mCurr;
        
    } else {
        /* NORMAL 状态：评估是否需要进入探测 */
        
        /* 升档判定 */
        int mNext = state->mCurr + 1;
        if (mNext < levels) {
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
                    mReturn = mNext;  /* 下一轮发送探测帧 */
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
                    mReturn = mPrev;  /* 下一轮发送探测帧 */
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
                state->attempts[idx] += inc;
                int32_t recent = successRatio[idxDelta];
#if MCS_DEBUG
                int32_t oldRate = state->successRate[idx];
#endif
                /* EWMA: old * alpha + new * (1 - alpha) */
                state->successRate[idx] = FixedMul(cfg->alpha, state->successRate[idx]) + 
                                          FixedMul(FIXED_ONE - cfg->alpha, recent);
                MCS_LOG_UPDATE("  User%d MCS%d: sampled, old=%.3f, recent=%.3f, new=%.3f\n",
                              u, m, oldRate/1000.0, recent/1000.0, state->successRate[idx]/1000.0);
            } else {
                /* 未采样档位：PER 衰减，成功率上升，但限制上限 */
#if MCS_DEBUG
                int32_t oldSucc = state->successRate[idx];
#endif
                int32_t per = FIXED_ONE - state->successRate[idx];
                per = FixedMul(per, cfg->noSampleDecay);
                state->successRate[idx] = FIXED_ONE - per;
                
                /* 限制未采样档位的成功率上限为 0.95 */
                if (state->successRate[idx] > 950) {
                    state->successRate[idx] = 950;
                }
                
                MCS_LOG_UPDATE("  User%d MCS%d: un-sampled, decay PER, succ %.3f -> %.3f\n",
                              u, m, oldSucc/1000.0, state->successRate[idx]/1000.0);
            }
        }
        ApplyMonotonicUserSuccess(state, u);
    }
}
