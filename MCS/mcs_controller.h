#ifndef MCS_CONTROLLER_H
#define MCS_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 调试输出开关 */
#ifndef MCS_DEBUG
#define MCS_DEBUG 1
#endif

#if MCS_DEBUG
#include <stdio.h>
#define MCS_LOG(fmt, ...) printf("[MCS] " fmt, ##__VA_ARGS__)
#define MCS_LOG_INIT(fmt, ...) printf("[MCS-INIT] " fmt, ##__VA_ARGS__)
#define MCS_LOG_UPDATE(fmt, ...) printf("[MCS-UPDATE] " fmt, ##__VA_ARGS__)
#define MCS_LOG_SELECT(fmt, ...) printf("[MCS-SELECT] " fmt, ##__VA_ARGS__)
#define MCS_LOG_CLUSTER(fmt, ...) printf("[MCS-CLUSTER] " fmt, ##__VA_ARGS__)
#define MCS_LOG_PROBE(fmt, ...) printf("[MCS-PROBE] " fmt, ##__VA_ARGS__)
#else
#define MCS_LOG(fmt, ...)
#define MCS_LOG_INIT(fmt, ...)
#define MCS_LOG_UPDATE(fmt, ...)
#define MCS_LOG_SELECT(fmt, ...)
#define MCS_LOG_CLUSTER(fmt, ...)
#define MCS_LOG_PROBE(fmt, ...)
#endif

#define MCS_MAX_USERS 8          /* 支持的最大用户数 */
#define MCS_MAX_LEVELS 12         /* 支持的最大 MCS 档位数 */
#define MCS_MAX_CLUSTERS 3        /* 最大聚类簇数（本算法仅用 2/3） */
#define MCS_MAX_ITER_KMEANS 20    /* k-means 迭代上限 */

    /* 定点数缩放因子：用于表示 [0.0, 1.0] 范围的成功率、速率等参数 */
#define FIXED_SCALE 1000         /* 1000 表示 1.0，即保留 3 位小数精度 */
#define FIXED_ONE FIXED_SCALE    /* 1.0 的定点表示 */

/* 按 MCS 主序访问二维数据，提升缓存局部性 */
#define MCS_IDX(level, user) (((level) * MCS_MAX_USERS) + (user))

/* 探测状态 */
typedef enum {
    MCS_PROBE_NORMAL = 0,   /* 正常状态 */
    MCS_PROBE_UP = 1,       /* 升档探测中 */
    MCS_PROBE_DOWN = 2      /* 降档探测中 */
} MCSProbeState;

/* 控制算法的可调参数（定点数表示，单位为 FIXED_SCALE） */
typedef struct {
    int32_t alpha;          /* EWMA 遗忘因子 */
    int32_t zeta;           /* 活跃簇占比阈值 */
    int32_t beta;           /* 簇权重幂次 */
    int32_t gamma;          /* 评分幂次 */
    int holdTimeDown;       /* 降档防抖轮数（固定为2） */
    int32_t noSampleDecay;  /* 未尝试档位的衰减系数 */
    int nMin;               /* 升档所需最小样本数 */
} MCSConfig;

/* 控制器的运行时状态 */
typedef struct {
    int userCount;                                 /* 系统用户数 */
    int mcsLevels;                                 /* MCS 档位数 */
    const int32_t *rTable;                         /* 各档位速率表 */
    int mCurr;                                     /* 当前使用的 MCS */
    int holdCounterUp;                             /* 升档防抖计数 */
    int holdCounterDown;                           /* 降档防抖计数 */
    int32_t successRate[MCS_MAX_LEVELS * MCS_MAX_USERS]; /* EWMA 成功率 */
    int attempts[MCS_MAX_LEVELS * MCS_MAX_USERS];        /* 累计尝试次数（按 MCS 主序存储） */
    /* 探测-验证状态管理 */
    MCSProbeState probeState;                      /* 探测状态 */
    int probeTarget;                               /* 探测目标 MCS */
} __attribute__((aligned(64))) MCSState;

/* 聚类结果，用于后续评分 */
typedef struct {
    int clusters;                                  /* 活跃簇数量（1~3） */
    int sizes[MCS_MAX_CLUSTERS];                   /* 各簇规模 */
    int members[MCS_MAX_CLUSTERS][MCS_MAX_USERS];  /* 各簇成员索引 */
    int32_t weights[MCS_MAX_CLUSTERS];             /* 各簇权重 */
} MCSClusterSet;

/* ========== 核心接口 ========== */

/* 初始化控制器状态 */
void McsInitState(MCSState *state, int userCount, int mcsLevels, const int32_t *initSuccess);

/* 更新统计数据（EWMA + 单调性修正）
 * 参数:
 *   state: 控制器状态（会被更新）
 *   cfg: 算法配置参数
 *   attemptsDelta: 本轮各用户/MCS的尝试次数增量 (user_major, size = userCount * mcsLevels)
 *   successRatio: 本轮各用户/MCS的成功率 (user_major, size = userCount * mcsLevels)
 * 说明: 应在每轮传输后、McsSelect 之前调用
 */
void McsUpdateWithRound(MCSState *state, const MCSConfig *cfg, const int *attemptsDelta, const int32_t *successRatio);

/* 选择本轮使用的 MCS（包含探测-验证逻辑）
 * 参数:
 *   cfg: 算法配置参数
 *   state: 控制器状态（更新决策状态：mCurr, holdCounter, probeState）
 * 返回: 本轮应使用的 MCS（可能是主用MCS或探测MCS）
 * 说明: 应在 McsUpdateWithRound 之后调用
 */
int McsSelect(const MCSConfig *cfg, MCSState *state);

#ifdef __cplusplus
}
#endif

#endif /* MCS_CONTROLLER_H */

