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

#define TX_PER_ROUND 50
#define TOTAL_ROUNDS 50

static int RandInt(int max) {
    return rand() % max;
}

/* 初始化真实 PER */
static void InitTruePer(int32_t (*table)[MCS_MAX_LEVELS], int users, int levels) {
    /* 基础值和步长 */
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
        .alpha = 250,
        .zeta = 200,
        .beta = 1000,
        .gamma = 1000,
        .holdTimeDown = 2,
        .noSampleDecay = 900,
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

        /* A) 基于更新后的状态选择本轮的 MCS（可能是主用或探测） */
        printf("\n========== Round %02d ==========\n", round);
        int prevMcs = state.mCurr;
        int mTx = McsSelect(&cfg, &state);
        
        /* B) 执行发送并模拟接收（全部使用 mTx） */
        for (int u = 0; u < state.userCount; ++u) {
            int32_t per = truePer[u][mTx];
            int success = SimulateSuccesses(per, TX_PER_ROUND);
            int idx = u * state.mcsLevels + mTx;
            attemptsDelta[idx] += TX_PER_ROUND;
            successCount[idx] += success;
        }

        /* C) 计算成功率 */
        for (int idx = 0; idx < state.userCount * state.mcsLevels; ++idx) {
            if (attemptsDelta[idx] > 0) {
                /* ratio = success / attempts */
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
        
        /* E) 输出轮次摘要 */
        const char *stateStr = "NORMAL";
        if (state.probeState == MCS_PROBE_UP) {
            stateStr = "PROBE_UP";
        } else if (state.probeState == MCS_PROBE_DOWN) {
            stateStr = "PROBE_DOWN";
        }
        
        printf("[Round %02d Summary] mTx=%d, mCurr=%d (prev=%d), state=%s\n",
               round, mTx, state.mCurr, prevMcs, stateStr);
    }
}

int main(void) {
    srand(2025);
    printf("MCS controller demo\n");
    RunDemo();
    return 0;
}
