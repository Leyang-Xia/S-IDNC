#ifndef MCS_CONTROLLER_H
#define MCS_CONTROLLER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCS_MAX_USERS 8          /* 支持的最大用户数 */
#define MCS_MAX_LEVELS 12         /* 支持的最大 MCS 档位数 */
#define MCS_MAX_CLUSTERS 3        /* 最大聚类簇数（本算法仅用 2/3） */
#define MCS_MAX_ITER_KMEANS 20    /* k-means 迭代上限 */
#define MCS_PROBE_COLS 16         /* 探索表的列数 */
#define MCS_PROBE_ROWS 4          /* 探索表的行数（对应偏移 +1,+2,-1,-2） */

/* 按 MCS 主序访问二维数据，提升缓存局部性 */
#define MCS_IDX(level, user) (((level) * MCS_MAX_USERS) + (user))

/* 控制算法的可调参数 */
typedef struct {
    double alpha;          /* EWMA 遗忘因子 */
    double zeta;           /* 活跃簇占比阈值 */
    double beta;           /* 簇权重幂次 */
    double gamma;          /* 评分幂次 */
    double deltaUp;        /* 升档收益阈值 */
    double deltaDown;      /* 降档收益阈值 */
    int holdTimeUp;        /* 升档防抖轮数 */
    int holdTimeDown;      /* 降档防抖轮数 */
    double noSampleDecay;  /* 未尝试档位的衰减系数 */
    int nMin;              /* 升档所需最小样本数 */
} MCSConfig;

/* 控制器的运行时状态 */
typedef struct {
    int userCount;                                 /* 系统用户数 */
    int mcsLevels;                                 /* MCS 档位数 */
    const double *rTable;                          /* 各档位速率表（Mbps） */
    int mCurr;                                     /* 当前使用的 MCS */
    int holdCounterUp;                             /* 升档防抖计数 */
    int holdCounterDown;                           /* 降档防抖计数 */
    double successRate[MCS_MAX_LEVELS * MCS_MAX_USERS]; /* EWMA 成功率（按 MCS 主序存储） */
    int attempts[MCS_MAX_LEVELS * MCS_MAX_USERS];        /* 累计尝试次数（按 MCS 主序存储） */
    /* 探索状态管理 */
    int probeTable[MCS_PROBE_COLS][MCS_PROBE_ROWS]; /* 预生成的探索偏移表 */
    int probeCol;                                  /* 当前使用的列索引 */
    int probeRow;                                  /* 当前使用的行索引 */
} __attribute__((aligned(64))) MCSState;

/* 聚类结果，用于后续评分 */
typedef struct {
    int clusters;                                  /* 活跃簇数量（1~3） */
    int sizes[MCS_MAX_CLUSTERS];                   /* 各簇规模 */
    int members[MCS_MAX_CLUSTERS][MCS_MAX_USERS];  /* 各簇成员索引 */
    double weights[MCS_MAX_CLUSTERS];              /* 各簇权重 */
} MCSClusterSet;

/* 单轮决策信息，便于调试和展示 */
typedef struct {
    int roundIndex;                                /* 轮次（可选赋值） */
    int prevMcs;                                   /* 上一轮 MCS */
    int mCurr;                                     /* 本轮 MCS */
    double scoreCurr;                              /* Score(mCurr) */
    double scoreUp;                                /* Score(mCurr+1) */
    double scoreDown;                              /* Score(mCurr-1) */
    int holdCounterUp;                             /* 升档防抖计数 */
    int holdCounterDown;                           /* 降档防抖计数 */
    int clusterCount;                              /* 活跃簇数量 */
    int clusterSizes[MCS_MAX_CLUSTERS];
    double clusterWeights[MCS_MAX_CLUSTERS];
    double clusterSuccess[MCS_MAX_CLUSTERS];       /* 各簇在当前档位的代表成功率 */
    int clusterMembers[MCS_MAX_CLUSTERS][MCS_MAX_USERS];
} MCSDecisionInfo;

/* ========== 核心接口 ========== */

/* 初始化控制器状态 */
void McsInitState(MCSState *state, int userCount, int mcsLevels, const double *initSuccess);

/* 选择主用 MCS（基于统计数据更新状态并决策）
 * 参数:
 *   cfg: 算法配置参数
 *   state: 控制器状态（会被更新）
 *   attemptsDelta: 本轮各用户/MCS的尝试次数增量 (user_major, size = userCount * mcsLevels)
 *   successRatio: 本轮各用户/MCS的成功率 (user_major, size = userCount * mcsLevels)
 *   info: 可选的决策信息输出（可为 NULL）
 * 返回: 选定的主用 MCS
 */
int McsSelect(const MCSConfig *cfg, MCSState *state, const int *attemptsDelta, const double *successRatio, MCSDecisionInfo *info);

/* 获取下一个探索 MCS（基于当前 mCurr，按 round-robin 探索 +1,+2,-1,-2）
 * 参数:
 *   state: 控制器状态（探索表状态会被更新）
 * 返回: 探索用的 MCS 档位
 * 注意: 应在 McsSelect 之后调用，以基于最新的 mCurr 计算探索档位
 */
int McsGetExploreMcs(MCSState *state);

/* 仅更新统计数据，不做决策（用于纯数据收集场景）*/
void McsUpdateWithRound(MCSState *state, const MCSConfig *cfg, const int *attemptsDelta, const double *successRatio);

#ifdef __cplusplus
}
#endif

#endif /* MCS_CONTROLLER_H */

