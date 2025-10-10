#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "mcs_controller.h"

static const double R_TABLE[MCS_MAX_LEVELS] = {
    72.059, 144.118, 216.176, 288.235,
    432.353, 576.471, 648.529, 720.588,
    864.706, 960.784, 1080.882, 1200.980
};

#define MAIN_TX_PER_ROUND 90
#define EXPLORE_TX_PER_ROUND 10
#define PROBE_COLS 16
#define PROBE_ROWS 4
#define TOTAL_ROUNDS 50

static double RandUniform(void) {
    return (double)rand() / (double)RAND_MAX;
}

static void InitTruePer(double (*table)[MCS_MAX_LEVELS], int users, int levels) {
    const double baseArr[MCS_MAX_USERS] = {
        0.150000, 0.088201, 0.132715, 0.133726,
        0.147581, 0.057723, 0.081746, 0.141956
    };
    const double stepArr[MCS_MAX_USERS] = {
        0.040000, 0.035716, 0.047791, 0.044604,
        0.033339, 0.033069, 0.049482, 0.038478
    };
    for (int u = 0; u < users; ++u) {
        double base = baseArr[u % 8];
        double step = stepArr[u % 8];
        for (int m = 0; m < levels; ++m) {
            double value = base + step * m;
            table[u][m] = value > 0.9 ? 0.9 : value;
        }
    }
}

static int SimulateSuccesses(double per, int attempts) {
    int success = 0;
    for (int i = 0; i < attempts; ++i) {
        if (RandUniform() > per) {
            success++;
        }
    }
    return success;
}

static void RunDemo(void) {
    MCSConfig cfg = {
        .alpha = 0.25,
        .zeta = 0.2,
        .beta = 1.0,
        .gamma = 1.2,
        .deltaUp = 0.05,
        .deltaDown = 0.05,
        .holdTimeUp = 2,
        .holdTimeDown = 2,
        .noSampleDecay = 0.90,
        .nMin = 30
    };

    MCSState state;
    McsInitState(&state, 8, 12, NULL);
    state.rTable = R_TABLE;

    double truePer[MCS_MAX_USERS][MCS_MAX_LEVELS];
    InitTruePer(truePer, state.userCount, state.mcsLevels);

    double successRatio[MCS_MAX_USERS * MCS_MAX_LEVELS];
    double successCount[MCS_MAX_USERS * MCS_MAX_LEVELS];
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
            double perMain = truePer[u][mCurr];
            int successMain = SimulateSuccesses(perMain, MAIN_TX_PER_ROUND);
            int idxMain = u * state.mcsLevels + mCurr;
            attemptsDelta[idxMain] += MAIN_TX_PER_ROUND;
            successCount[idxMain] += (double)successMain;

            if (mExplore != mCurr) {
                double perExp = truePer[u][mExplore];
                int successExp = SimulateSuccesses(perExp, EXPLORE_TX_PER_ROUND);
                int idxExp = u * state.mcsLevels + mExplore;
                attemptsDelta[idxExp] += EXPLORE_TX_PER_ROUND;
                successCount[idxExp] += (double)successExp;
            } else {
                int successExtra = SimulateSuccesses(perMain, EXPLORE_TX_PER_ROUND);
                attemptsDelta[idxMain] += EXPLORE_TX_PER_ROUND;
                successCount[idxMain] += (double)successExtra;
            }
        }

        /* C) 计算成功率 */
        for (int idx = 0; idx < state.userCount * state.mcsLevels; ++idx) {
            if (attemptsDelta[idx] > 0) {
                double ratio = successCount[idx] / (double)attemptsDelta[idx];
                if (ratio < 0.0) {
                    ratio = 0.0;
                }
                if (ratio > 1.0) {
                    ratio = 1.0;
                }
                successRatio[idx] = ratio;
            }
        }

        /* D) 基于本轮统计数据选择下一轮的主用 MCS */
        MCSDecisionInfo info = {0};
        info.roundIndex = round;
        int prevMcs = mCurr;
        int mcs = McsSelect(&cfg, &state, attemptsDelta, successRatio, &info);
        printf("[Round %02d] mCurr=%d (prev=%d) Score(m)=%.4f, Score(m+1)=%.4f, Score(m-1)=%.4f, holdUp=%d, holdDown=%d, explore=%d\n",
               round,
               mcs,
               prevMcs,
               info.scoreCurr,
               info.scoreUp,
               info.scoreDown,
               info.holdCounterUp,
               info.holdCounterDown,
               mExplore);
        for (int c = 0; c < info.clusterCount; ++c) {
            printf("    cluster %d size=%d weight=%.3f S_c=%.4f members:", c, info.clusterSizes[c], info.clusterWeights[c], info.clusterSuccess[c]);
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

