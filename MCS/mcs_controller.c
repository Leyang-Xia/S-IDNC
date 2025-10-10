#include "mcs_controller.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static void InsertionSort(double *values, int n) {
    for (int i = 1; i < n; ++i) {
        double key = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = key;
    }
}

/* 已排序数组的分位数，线性插值 */
static double PercentilePresorted(const double *sortedValues, int n, double p) {
    if (n <= 0) {
        return 0.0;
    }
    double rank = p * (n - 1);
    int low = (int)floor(rank);
    int high = (int)ceil(rank);
    double weight = rank - low;
    double result = sortedValues[low];
    if (high > low) {
        result = sortedValues[low] * (1.0 - weight) + sortedValues[high] * weight;
    }
    return result;
}

/* 前缀最小填充：高档未采样时继承已观测的最低成功率 */
static void PrefixMinFill(const double *values, const int *attempts, int n, double *out) {
    double minSeen = 1.1;
    int hasSeen = 0;
    for (int m = 0; m < n; ++m) {
        if (attempts[m] > 0) {
            double v = values[m];
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
static void PavNonIncreasing(const double *input, const double *weights, int n, double *output) {
    double *y = (double *)malloc(sizeof(double) * n);
    double *w = (double *)malloc(sizeof(double) * n);
    int *startPos = (int *)malloc(sizeof(int) * n);  // 跟踪每个合并块的起始位置
    int *endPos = (int *)malloc(sizeof(int) * n);    // 跟踪每个合并块的结束位置
    
    int m = -1;
    for (int i = 0; i < n; ++i) {
        double val = input[i];
        double wt = weights ? weights[i] : 1.0;
        ++m;
        y[m] = val;
        w[m] = wt;
        startPos[m] = i;  // 记录起始位置
        endPos[m] = i;    // 记录结束位置
        
        while (m > 0 && y[m - 1] < y[m]) {
            double numerator = y[m - 1] * w[m - 1] + y[m] * w[m];
            double denom = w[m - 1] + w[m];
            y[m - 1] = numerator / denom;
            w[m - 1] = denom;
            endPos[m - 1] = endPos[m];  // 更新结束位置
            --m;
        }
    }
    
    // 正确的逆向填充：直接根据位置范围填充
    for (int block = 0; block <= m; ++block) {
        double value = y[block];
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
    double sPref[MCS_MAX_LEVELS];
    double weights[MCS_MAX_LEVELS];
    double src[MCS_MAX_LEVELS];
    int att[MCS_MAX_LEVELS];
    for (int m = 0; m < mcs; ++m) {
        int idx = MCS_IDX(m, user);
        src[m] = state->successRate[idx];
        att[m] = state->attempts[idx];
    }
    PrefixMinFill(src, att, mcs, sPref);
    for (int m = 0; m < mcs; ++m) {
        weights[m] = att[m] > 0 ? att[m] : 1.0;
    }
    double corrected[MCS_MAX_LEVELS];
    PavNonIncreasing(sPref, weights, mcs, corrected);
    for (int m = 0; m < mcs; ++m) {
        state->successRate[MCS_IDX(m, user)] = corrected[m];
    }
}

/* 计算簇在MCS档位m的代表成功率 */
static double CalcClusterSuccessAt(const MCSState *state, const int *members, int size, int m) {
    double buffer[MCS_MAX_USERS];
    double sum = 0.0;
    for (int i = 0; i < size; ++i) {
        double val = state->successRate[MCS_IDX(m, members[i])];
        buffer[i] = val;
        sum += val;
    }
    InsertionSort(buffer, size);
    double q80 = PercentilePresorted(buffer, size, 0.80);
    double mean = sum / (double)size;
    return 0.7 * q80 + 0.3 * mean;
}

/* 根据锚定成功率对用户聚类，并筛选出活跃簇 */
static void SelectActiveClusters(const double *values, int n, const MCSConfig *cfg, MCSClusterSet *active) {
    /* 1) 排序并保存原始索引 */
    double vSorted[MCS_MAX_USERS];
    int idxSorted[MCS_MAX_USERS];
    for (int i = 0; i < n; ++i) {
        vSorted[i] = values[i];
        idxSorted[i] = i;
    }
    for (int i = 1; i < n; ++i) {
        double v = vSorted[i];
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
    double p1[MCS_MAX_USERS + 1];
    double p2[MCS_MAX_USERS + 1];
    p1[0] = 0.0;
    p2[0] = 0.0;
    for (int i = 0; i < n; ++i) {
        p1[i+1] = p1[i] + vSorted[i];
        p2[i+1] = p2[i] + vSorted[i]*vSorted[i];
    }
    #define SSE(l,r) ( (p2[(r)+1]-p2[(l)]) - (((p1[(r)+1]-p1[(l)])*(p1[(r)+1]-p1[(l)]))/((double)((r)-(l)+1))) )

    /* 3) k=2、k=3 穷举切分（n 很小） */
    double best2 = INFINITY;
    int best2S = -1;
    for (int s = 1; s <= n-1; ++s) {
        double cost = SSE(0,s-1) + SSE(s,n-1);
        if (cost < best2) {
            best2 = cost;
            best2S = s;
        }
    }
    double best3 = INFINITY;
    int best3S = -1;
    int best3T = -1;
    if (n >= 3) {
        for (int s = 1; s <= n-2; ++s) {
            for (int t = s+1; t <= n-1; ++t) {
                double cost = SSE(0,s-1) + SSE(s,t-1) + SSE(t,n-1);
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
        double improve = (best2 - best3) / best2;
        if (improve >= 0.10) {
            useK = 3;
        }
    }

    /* 5) 生成簇（按 ζ 过滤），不足则取最大簇 */
    int counts[MCS_MAX_CLUSTERS] = {0};
    if (useK == 2) {
        int sizes2[2] = { best2S, n - best2S };
        const int starts2[2] = { 0, best2S };
        int idx = 0;
        for (int c = 0; c < 2; ++c) {
            double frac = (double)sizes2[c] / (double)n;
            if (frac >= cfg->zeta) {
                active->sizes[idx] = sizes2[c];
                for (int i = 0; i < sizes2[c]; ++i) {
                    active->members[idx][i] = idxSorted[starts2[c] + i];
                }
                counts[idx] = sizes2[c];
                idx++;
            }
        }
        if (idx == 0) { /* 全被过滤，取最大簇 */
            int cmax = sizes2[0] >= sizes2[1] ? 0 : 1;
            active->sizes[0] = sizes2[cmax];
            for (int i = 0; i < sizes2[cmax]; ++i) {
                active->members[0][i] = idxSorted[starts2[cmax] + i];
            }
            counts[0] = sizes2[cmax];
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
            double frac = (double)sizes3[c] / (double)n;
            if (frac >= cfg->zeta) {
                active->sizes[idx] = sizes3[c];
                for (int i = 0; i < sizes3[c]; ++i) {
                    active->members[idx][i] = idxSorted[starts3[c] + i];
                }
                counts[idx] = sizes3[c];
                idx++;
            }
        }
        if (idx == 0) { /* 全被过滤，取最大簇 */
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
            counts[0] = sizes3[cmax];
            idx = 1;
        }
        active->clusters = idx;
    }

    /* 6) 权重：w_c ∝ size(c)^β，并一次归一化 */
    double weightSum = 0.0;
    for (int c = 0; c < active->clusters; ++c) {
        double w = pow((double)active->sizes[c], cfg->beta);
        active->weights[c] = w;
        weightSum += w;
    }
    if (weightSum <= 0.0) {
        weightSum = 1.0;
    }
    for (int c = 0; c < active->clusters; ++c) {
        active->weights[c] /= weightSum;
    }
    #undef SSE
}

/* 生成随机探索偏移表（每列是 {1,2,-1,-2} 的一个排列） */
static void GenerateProbeTable(int table[MCS_PROBE_COLS][MCS_PROBE_ROWS]) {
    static int initialized = 0;
    if (!initialized) {
        srand((unsigned int)time(NULL));
        initialized = 1;
    }
    
    for (int col = 0; col < MCS_PROBE_COLS; ++col) {
        int offsets[MCS_PROBE_ROWS] = {1, 2, -1, -2};
        /* Fisher-Yates 洗牌 */
        for (int i = MCS_PROBE_ROWS - 1; i > 0; --i) {
            int j = rand() % (i + 1);
            int tmp = offsets[i];
            offsets[i] = offsets[j];
            offsets[j] = tmp;
        }
        for (int row = 0; row < MCS_PROBE_ROWS; ++row) {
            table[col][row] = offsets[row];
        }
    }
}


/* 计算指定档位的聚合评分 */
static double ComputeScore(const MCSState *state, const MCSConfig *cfg, const MCSClusterSet *clusters, int m) {
    if (m < 0 || m >= state->mcsLevels) {
        return -INFINITY;
    }
    double score = 0.0;
    for (int c = 0; c < clusters->clusters; ++c) {
        double succ = CalcClusterSuccessAt(state, clusters->members[c], clusters->sizes[c], m);
        score += state->rTable[m] * clusters->weights[c] * pow(succ, cfg->gamma);
    }
    return score;
}


/* ---------- 对外接口 ---------- */

void McsInitState(MCSState *state, int userCount, int mcsLevels, const double *initSuccess) {
    state->userCount = userCount;
    state->mcsLevels = mcsLevels;
    state->mCurr = 6;
    state->holdCounterUp = 0;
    state->holdCounterDown = 0;
    
    /* 初始化探索表 */
    GenerateProbeTable(state->probeTable);
    state->probeCol = 0;
    state->probeRow = 0;
    double fallback[MCS_MAX_LEVELS];
    if (!initSuccess) {
        const double start = 0.95;
        const double end = 0.40;
        const double step = (mcsLevels > 1) ? (start - end) / (double)(mcsLevels - 1) : 0.0;
        for (int m = 0; m < mcsLevels; ++m) {
            double value = start - step * m;
            if (value < 0.05) {
                value = 0.05;
            }
            if (value > 0.99) {
                value = 0.99;
            }
            fallback[m] = value;
        }
    }
    for (int u = 0; u < userCount; ++u) {
        for (int m = 0; m < mcsLevels; ++m) {
            int idx = MCS_IDX(m, u);
            const double *src = initSuccess ? initSuccess : fallback;
            state->successRate[idx] = src[m];
            state->attempts[idx] = 0;
        }
    }
}

int McsSelect(const MCSConfig *cfg, MCSState *state, const int *attemptsDelta, const double *successRatio, MCSDecisionInfo *info) {
    int users = state->userCount;
    int levels = state->mcsLevels;
    int prevMcs = state->mCurr;
    for (int u = 0; u < users; ++u) {
        for (int m = 0; m < levels; ++m) {
            int idxDelta = u * levels + m;
            int inc = attemptsDelta[idxDelta];
            int idx = MCS_IDX(m, u);
            if (inc > 0) {
                state->attempts[idx] += inc;
                double recent = successRatio[idxDelta];
                state->successRate[idx] = cfg->alpha * state->successRate[idx] + (1.0 - cfg->alpha) * recent;
            } else {
                double per = 1.0 - state->successRate[idx];
                per *= cfg->noSampleDecay;
                if (per < 0.0) {
                    per = 0.0;
                }
                if (per > 1.0) {
                    per = 1.0;
                }
                state->successRate[idx] = 1.0 - per;
            }
        }
        ApplyMonotonicUserSuccess(state, u);
    }

    double anchor[MCS_MAX_USERS];
    for (int u = 0; u < users; ++u) {
        anchor[u] = state->successRate[MCS_IDX(state->mCurr, u)];
    }

    MCSClusterSet clusters;
    SelectActiveClusters(anchor, users, cfg, &clusters);
    double scoreCurr = ComputeScore(state, cfg, &clusters, state->mCurr);
    double scoreUp = ComputeScore(state, cfg, &clusters, state->mCurr + 1);
    double scoreDown = ComputeScore(state, cfg, &clusters, state->mCurr - 1);

    int attemptsNext = 0;
    if (state->mCurr + 1 < levels) {
        for (int u = 0; u < users; ++u) {
            attemptsNext += state->attempts[MCS_IDX(state->mCurr + 1, u)];
        }
    }
    if (state->mCurr + 1 < levels &&
        scoreUp >= (1.0 + cfg->deltaUp) * scoreCurr &&
        attemptsNext >= cfg->nMin) {
        state->holdCounterUp += 1;
        if (state->holdCounterUp >= cfg->holdTimeUp) {
            state->mCurr += 1;
            state->holdCounterUp = 0;
            state->holdCounterDown = 0;
        }
    } else {
        state->holdCounterUp = 0;
    }

    if (state->mCurr - 1 >= 0 &&
        scoreDown >= (1.0 + cfg->deltaDown) * scoreCurr) {
        state->holdCounterDown += 1;
        if (state->holdCounterDown >= cfg->holdTimeDown) {
            state->mCurr -= 1;
            state->holdCounterDown = 0;
            state->holdCounterUp = 0;
        }
    } else {
        state->holdCounterDown = 0;
    }

    if (info) {
        info->prevMcs = prevMcs;
        info->mCurr = state->mCurr;
        info->scoreCurr = scoreCurr;
        info->scoreUp = scoreUp;
        info->scoreDown = scoreDown;
        info->holdCounterUp = state->holdCounterUp;
        info->holdCounterDown = state->holdCounterDown;
        info->clusterCount = clusters.clusters;
        for (int c = 0; c < clusters.clusters; ++c) {
            info->clusterSizes[c] = clusters.sizes[c];
            info->clusterWeights[c] = clusters.weights[c];
            info->clusterSuccess[c] = CalcClusterSuccessAt(state, clusters.members[c], clusters.sizes[c], state->mCurr);
            for (int i = 0; i < clusters.sizes[c]; ++i) {
                info->clusterMembers[c][i] = clusters.members[c][i];
            }
        }
    }

    return state->mCurr;
}

int McsGetExploreMcs(MCSState *state) {
    /* 从探索表中获取当前偏移 */
    int offset = state->probeTable[state->probeCol][state->probeRow];
    
    /* 更新探索表索引 */
    state->probeRow++;
    if (state->probeRow >= MCS_PROBE_ROWS) {
        state->probeRow = 0;
        state->probeCol++;
        if (state->probeCol >= MCS_PROBE_COLS) {
            /* 所有列用完后重新生成随机表 */
            GenerateProbeTable(state->probeTable);
            state->probeCol = 0;
        }
    }
    
    /* 计算探索 MCS，并裁剪到合法范围 */
    int mExplore = state->mCurr + offset;
    if (mExplore < 0) {
        mExplore = 0;
    }
    if (mExplore >= state->mcsLevels) {
        mExplore = state->mcsLevels - 1;
    }
    
    return mExplore;
}

void McsUpdateWithRound(MCSState *state, const MCSConfig *cfg, const int *attemptsDelta, const double *successRatio) {
    int users = state->userCount;
    int levels = state->mcsLevels;
    for (int u = 0; u < users; ++u) {
        for (int m = 0; m < levels; ++m) {
            int idxDelta = u * levels + m;
            int inc = attemptsDelta[idxDelta];
            int idx = MCS_IDX(m, u);
            if (inc > 0) {
                state->attempts[idx] += inc;
                double recent = successRatio[idxDelta];
                state->successRate[idx] = cfg->alpha * state->successRate[idx] + (1.0 - cfg->alpha) * recent;
            } else {
                double per = 1.0 - state->successRate[idx];
                per *= cfg->noSampleDecay;
                if (per < 0.0) {
                    per = 0.0;
                }
                if (per > 1.0) {
                    per = 1.0;
                }
                state->successRate[idx] = 1.0 - per;
            }
        }
        ApplyMonotonicUserSuccess(state, u);
    }
}

