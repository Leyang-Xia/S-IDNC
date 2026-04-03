#ifndef MCS_CONTROLLER_H
#define MCS_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MCS 控制器（面向多用户的档位选择）
 *
 * 核心思路（高层概览）：
 * - 每轮传输后先调用 McsUpdateWithRound()：用 EWMA 融合“本轮观测成功率”，并对未采样档位做保守老化；
 *   然后对每个用户做单调性修正，保证低 MCS 的成功率不低于高 MCS（避免出现不合理的“高档位更稳”曲线）。
 * - 再调用 McsSelect()：以当前 mCurr 档位的用户成功率作为锚点做 1D 聚类，按簇权重聚合计算评分，
 *   并通过“探测 -> 收集反馈 -> 验证是否切换”的状态机来升/降档，避免抖动。
 *
 * 定点数约定：
 * - 所有“成功率/比例/权重/参数”均采用定点数表示，缩放因子 FIXED_SCALE（默认 1000）。
 *   例如 0.95 表示为 950，1.0 表示为 1000。
 */

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
#define CLUSTER_SUCCESS_RATE_THRESHOLD 500 /* 簇成功率阈值 */
/* 定点数缩放因子：用于表示 [0.0, 1.0] 范围的成功率、权重、参数等 */
#define FIXED_SCALE 1000         /* 1000 表示 1.0，即保留 3 位小数精度 */
#define FIXED_ONE FIXED_SCALE    /* 1.0 的定点表示 */

/*
 * 内部数组布局：level-major（MCS 主序）
 * - successRate 使用 [level][user] 的线性展开；这样在按 MCS 扫描时缓存局部性更好。
 * - 注意：对外输入 attemptsDelta/successRatio 采用 user-major（见 McsUpdateWithRound 注释）。
 */
#define MCS_IDX(level, user) (((level) * MCS_MAX_USERS) + (user))

/* 探测状态超时保护：最大探测轮数，防止反馈丢失导致状态卡住 */
#define MAX_PROBE_STATE_COUNT 10

/* 探测状态 */
typedef enum {
    MCS_PROBE_NORMAL = 0,   /* 正常状态 */
    MCS_PROBE_UP = 1,       /* 升档探测中 */
    MCS_PROBE_DOWN = 2      /* 降档探测中 */
} MCSProbeState;

/*
 * 控制算法可调参数（除 holdTimeDown/nMin 外，均为定点数，单位 FIXED_SCALE）
 * - alpha: EWMA 遗忘因子。越大越“稳”（更依赖历史），越小越“灵”（更依赖本轮观测）。
 * - zeta : 活跃簇占比阈值。簇规模/用户数 >= zeta 才会被纳入评分（避免极小簇噪声主导）。
 * - beta : 簇权重指数。权重 ∝ size^beta，beta=1.0 时权重近似按簇规模线性分配。
 * - gamma: 评分中成功率的幂次。gamma=1.0 时评分对成功率线性；>1 会更偏好“更稳”的簇。
 * - holdTimeDown: 降档防抖轮数（整数）。通常小于等于升档防抖，避免持续高 PER。
 * - noSampleDecay: 未采样档位的“PER 老化系数”。用于把陈旧的高成功率逐步拉回保守估计。
 * - nMin: 预留的最小样本数门槛（当前实现未强制使用；可用于约束升档需要足够观测）。
 */
typedef struct {
    int32_t alpha;           /* EWMA 遗忘因子 */
    int32_t zeta;            /* 活跃簇占比阈值 */
    int32_t beta;            /* 簇权重幂次 */
    int32_t gamma;           /* 评分幂次 */
    int holdTimeDown;        /* 降档防抖轮数（固定为2） */
    int32_t noSampleDecay;   /* 未尝试档位的衰减系数 */
    int nMin;                /* 升档所需最小样本数 */
    int32_t minSuccThreshold;/* 簇权重过滤门限（成功率低于此值则权重为0） */
} MCSConfig;

/* 控制器的运行时状态 */
typedef struct {
    int userCount;                                 /* 系统用户数 */
    int mcsLevels;                                 /* MCS 档位数 */
    const int32_t *rTable;                         /* 各档位速率表（由调用方提供，长度>=mcsLevels） */
    int mCurr;                                     /* 当前使用的 MCS */
    int holdCounterUp;                             /* 升档防抖计数 */
    int holdCounterDown;                           /* 降档防抖计数 */
    int32_t successRate[MCS_MAX_LEVELS * MCS_MAX_USERS]; /* 成功率 EWMA（0..FIXED_ONE） */
    /* 探测-验证状态机（McsSelect / McsUpdateWithRound 联动维护） */
    MCSProbeState probeState;                      /* 探测状态 */
    int probeTarget;                               /* 探测目标 MCS */
    int probeStateCount;                           /* 探测状态持续轮数（预留：用于超时保护） */
    char probeFeedbackReceived[MCS_MAX_USERS];     /* 探测反馈收集位图：1 表示收到该用户在目标档位的反馈 */
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
 *   attemptsDelta: 本轮各用户/各 MCS 的尝试次数增量（user-major）
 *     - 索引：idx = user * mcsLevels + mcs
 *     - 取值：>0 表示本轮该用户在该档位有真实采样；==0 表示未采样
 *   successRatio: 本轮各用户/各 MCS 的观测成功率（user-major，定点数 0..FIXED_ONE）
 *     - 仅当 attemptsDelta[idx] > 0 时该值才被使用
 *
 * 调用时机:
 * - 建议每轮传输结束后调用一次，且在 McsSelect 之前调用（先更新统计，再做决策）。
 */
void McsUpdateWithRound(MCSState *state, const MCSConfig *cfg, const int *attemptsDelta, const int32_t *successRatio);

/* 选择本轮使用的 MCS（包含探测-验证逻辑）
 * 参数:
 *   cfg: 算法配置参数
 *   state: 控制器状态（会更新：mCurr、holdCounterUp/Down、probeState 等）
 *
 * 返回:
 * - “本轮应使用的 MCS”：
 *   - 正常情况下返回 state->mCurr（主用档位）
 *   - 进入探测时会返回探测目标档位（用于下一轮发探测帧）
 *
 * 说明:
 * - 应在 McsUpdateWithRound 之后调用。
 * - state->rTable 需由调用方提前赋值，否则评分无意义。
 */
int McsSelect(const MCSConfig *cfg, MCSState *state);

#ifdef __cplusplus
}
#endif

#endif /* MCS_CONTROLLER_H */

