#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "mcs_controller.h"

/* 速率表(Mbps)*/
static const int32_t R_TABLE[MCS_MAX_LEVELS] = {
    72, 144, 216, 288,
    432, 576, 649, 721,
    865, 961, 1081, 1201
};

#define MAIN_TX_PER_ROUND 90
#define EXPLORE_TX_PER_ROUND 10
#define PROBE_COLS 16
#define PROBE_ROWS 4
#define TOTAL_ROUNDS 50

static int RandInt(int max) {
    return rand() % max;
}

/* 初始化真实 PER（定点数） */
static void InitTruePer(int32_t (*table)[MCS_MAX_LEVELS], int users, int levels) {
    /* 基础值和步长（定点数，FIXED_SCALE = 1000） */
    const int32_t baseArr[MCS_MAX_USERS] = {
        150, 88, 132, 133,
        147, 57, 81, 142
    };
    const int32_t stepArr[MCS_MAX_USERS] = {
        40, 35, 47, 44,
        33, 33, 49, 38
    };
    for (int u = 0; u < users; ++u) {
        int32_t base = baseArr[u % 8];
        int32_t step = stepArr[u % 8];
        for (int m = 0; m < levels; ++m) {
            int32_t value = base + step * m;
            table[u][m] = value > 900 ? 900 : value;  /* 最大 0.9 */
        }
    }
}

/* 模拟传输成功数（基于 PER） */
static int SimulateSuccesses(int32_t per, int attempts) {
    int success = 0;
    for (int i = 0; i < attempts; ++i) {
        /* per 是定点数，生成 [0, FIXED_SCALE) 的随机数 */
        int randVal = RandInt(FIXED_SCALE);
        if (randVal > per) {  /* 随机数 > PER 表示成功 */
            success++;
        }
    }
    return success;
}

static void RunDemo(void) {
    /* 配置参数（全部转换为定点数） */
    MCSConfig cfg = {
        .alpha = 250,            /* 0.25 * FIXED_SCALE */
        .zeta = 200,             /* 0.2 * FIXED_SCALE */
        .beta = 1000,            /* 1.0 * FIXED_SCALE */
        .gamma = 1200,           /* 1.2 * FIXED_SCALE */
        .deltaUp = 50,           /* 0.05 * FIXED_SCALE */
        .deltaDown = 50,         /* 0.05 * FIXED_SCALE */
        .holdTimeUp = 2,
        .holdTimeDown = 2,
        .noSampleDecay = 900,    /* 0.90 * FIXED_SCALE */
        .nMin = 30
    };

    MCSState state;
    McsInitState(&state, 8, 12, NULL);
    state.rTable = R_TABLE;

    int32_t truePer[MCS_MAX_USERS][MCS_MAX_LEVELS];
    InitTruePer(truePer, state.userCount, state.mcsLevels);

    int32_t successRatio[MCS_MAX_USERS * MCS_MAX_LEVELS];
    int32_t successCount[MCS_MAX_USERS * MCS_MAX_LEVELS];
    int attemptsDelta[MCS_MAX_USERS * MCS_MAX_LEVELS];

    for (int round = 1; round <= TOTAL_ROUNDS; ++round) {
        memset(successRatio, 0, sizeof(successRatio));
        memset(successCount, 0, sizeof(successCount));
        memset(attemptsDelta, 0, sizeof(attemptsDelta));

        /* A) 基于当前 mCurr 获取本轮的探索 MCS */
        int mCurr = state.mCurr;
        int mExplore = McsGetExploreMcs(&state);

        /* B) 执行发送并模拟接收（主用 + 探索） */
        for (int u = 0; u < state.userCount; ++u) {
            int32_t perMain = truePer[u][mCurr];
            int successMain = SimulateSuccesses(perMain, MAIN_TX_PER_ROUND);
            int idxMain = u * state.mcsLevels + mCurr;
            attemptsDelta[idxMain] += MAIN_TX_PER_ROUND;
            successCount[idxMain] += successMain;

            if (mExplore != mCurr) {
                int32_t perExp = truePer[u][mExplore];
                int successExp = SimulateSuccesses(perExp, EXPLORE_TX_PER_ROUND);
                int idxExp = u * state.mcsLevels + mExplore;
                attemptsDelta[idxExp] += EXPLORE_TX_PER_ROUND;
                successCount[idxExp] += successExp;
            } else {
                int successExtra = SimulateSuccesses(perMain, EXPLORE_TX_PER_ROUND);
                attemptsDelta[idxMain] += EXPLORE_TX_PER_ROUND;
                successCount[idxMain] += successExtra;
            }
        }

        /* C) 计算成功率（定点数） */
        for (int idx = 0; idx < state.userCount * state.mcsLevels; ++idx) {
            if (attemptsDelta[idx] > 0) {
                /* ratio = success / attempts，转换为定点数 */
                int32_t ratio = (successCount[idx] * FIXED_SCALE) / attemptsDelta[idx];
                if (ratio < 0) {
                    ratio = 0;
                }
                if (ratio > FIXED_SCALE) {
                    ratio = FIXED_SCALE;
                }
                successRatio[idx] = ratio;
            }
        }

        /* D) 更新统计数据 */
        McsUpdateWithRound(&state, &cfg, attemptsDelta, successRatio);
        
        /* E) 基于更新后的状态选择下一轮的主用 MCS */
        MCSDecisionInfo info = {0};
        info.roundIndex = round;
        int prevMcs = mCurr;
        int mcs = McsSelect(&cfg, &state, &info);
        
        /* 输出结果（转换定点数为浮点数显示） */
        printf("[Round %02d] mCurr=%d (prev=%d) Score(m)=%lld, Score(m+1)=%lld, Score(m-1)=%lld, holdUp=%d, holdDown=%d, explore=%d\n",
               round,
               mcs,
               prevMcs,
               (long long)info.scoreCurr,
               (long long)info.scoreUp,
               (long long)info.scoreDown,
               info.holdCounterUp,
               info.holdCounterDown,
               mExplore);
        
        for (int c = 0; c < info.clusterCount; ++c) {
            printf("    cluster %d size=%d weight=%.3f S_c=%.3f members:",
                   c,
                   info.clusterSizes[c],
                   info.clusterWeights[c] / 1000.0,
                   info.clusterSuccess[c] / 1000.0);
            for (int i = 0; i < info.clusterSizes[c]; ++i) {
                printf(" %d", info.clusterMembers[c][i]);
            }
            puts("");
        }
    }
}

int main(void) {
    srand(2025);
    printf("MCS controller demo\n");
    RunDemo();
    return 0;
}
