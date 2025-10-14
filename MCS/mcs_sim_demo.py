"""基于 mcs_multicast.md 的方案，演示 8 个用户、12 个 MCS 档位的多轮仿真：
- 每轮：主用 9 次发送 + 探索 1 次（round-robin遍历 {+1,+2,-1,-2}）
- 用户级 PER 单调修正：缺失 → 前缀最低 → Isotonic(PAV)
- 聚类：1D k-means++（K=2 或 3，按轮廓系数筛选）
- 升/降档：收益对比 + 防抖（步长 = 1）
"""

import numpy as np
from collections import deque, Counter
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from sklearn.isotonic import IsotonicRegression
from sklearn.cluster import KMeans
from sklearn.metrics import silhouette_score

# ----------------------
# 参数设定
# ----------------------
MCS_TABLE = np.arange(12)
N_USERS = 8
ALPHA = 0.25
ZETA = 0.2
BETA = 1.0
GAMMA = 1.2
DELTA_UP = 0.05
DELTA_DOWN = 0.05
HOLD_TIME_UP = 2
HOLD_TIME_DOWN = 2
EXPLORE_RATIO = 0.10
NO_SAMPLE_DECAY = 0.9  # 本轮未尝试的 MCS 成功率轻度衰减
N_MIN = 30          # 升档最小样本
MAIN_TX_PER_ROUND = 90
EXPLORE_TX_PER_ROUND = 10
TOTAL_ROUNDS = 500
RNG = np.random.default_rng(2025)

# 真实 PER：基础值 (5~15%) + 线性递增 (3~5%/级)，并截顶到 90%
true_per = np.zeros((N_USERS, len(MCS_TABLE)))
for user in range(N_USERS):
    base = RNG.uniform(0.05, 0.15)
    step = RNG.uniform(0.03, 0.05)
    true_per[user] = np.clip(base + step * MCS_TABLE, 0, 0.9)

# 速率表：802.11ax HE80 (80MHz, NSS=2), GI=800ns, 12 档（Mbps）
R_TABLE = np.array([
    72.059, 144.118, 216.176, 288.235,
    432.353, 576.471, 648.529, 720.588,
    864.706, 960.784, 1080.882, 1200.980
])

# ----------------------
# 工具函数
# ----------------------
def prefix_min_fill(values, attempts):
    """按 MCS 升序，用已有观测的最低成功率填充缺失项。"""
    filled = values.copy()
    min_seen = None
    for idx, _ in enumerate(MCS_TABLE):
        if attempts[idx] > 0:
            current = filled[idx]
            min_seen = current if min_seen is None else min(min_seen, current)
        elif min_seen is not None:
            filled[idx] = min_seen
    return filled


def monotonic_user_success(s_user, attempts_user):
    """用户级 PER 单调修正：缺失→前缀最低→ PAV。"""
    s_pref = prefix_min_fill(s_user, attempts_user)
    weights = np.clip(attempts_user, 1, None)
    iso = IsotonicRegression(increasing=False, out_of_bounds="clip")
    return iso.fit_transform(MCS_TABLE, s_pref, sample_weight=weights)

def cluster_users(values):
    """根据 silhouette 自动选择 K=2/3 的 1D 聚类，返回簇索引列表。"""
    arr = np.asarray(values).reshape(-1, 1)
    n = arr.shape[0]
    if n <= 1:
        return [list(range(n))]

    best_clusters = None
    best_score = -np.inf
    best_k = None

    for k in (2, 3):
        if n < k:
            continue
        km = KMeans(n_clusters=k, init="k-means++", n_init=5, random_state=2025)
        labels = km.fit_predict(arr)
        unique = np.unique(labels)

        if unique.size < 2:
            score = -np.inf
        else:
            try:
                score = silhouette_score(arr, labels)
            except ValueError:
                score = -np.inf

        if score > best_score + 1e-6:
            best_score = score
            best_clusters = [np.where(labels == lab)[0].tolist() for lab in unique]
            best_k = k

    if best_clusters is None:
        return [list(range(n))]
    if best_k == 3 and best_score < 0.1:  # 小幅提升不值得增加簇
        return cluster_users(values)  # 回退到 k=2 再调用一次
    return best_clusters

def calc_cluster_success(cluster, s_users):
    """簇代表成功率：0.7 * Q80 + 0.3 * mean。"""
    subset = s_users[cluster]  # shape: (#cluster, #mcs)
    q80 = np.percentile(subset, 80, axis=0)
    mean = subset.mean(axis=0)
    return 0.7 * q80 + 0.3 * mean

# ----------------------
# 初始状态
# ----------------------
s_base = np.linspace(0.95, 0.40, len(MCS_TABLE))
s = np.tile(s_base, (N_USERS, 1))
attempts = np.zeros((N_USERS, len(MCS_TABLE)), dtype=int)
success = np.zeros((N_USERS, len(MCS_TABLE)), dtype=int)
m_curr = 6
hold_counter = 0
hold_counter_down = 0
explore_offsets = deque([1, 2, -1, -2])
round_history = []

# ----------------------
# 模拟循环
# ----------------------
for round_idx in range(1, TOTAL_ROUNDS + 1):
    transmissions = []
    transmissions.extend([m_curr] * MAIN_TX_PER_ROUND)
    offset = explore_offsets[0]
    explore_offsets.rotate(-1)
    m_explore = np.clip(m_curr + offset, MCS_TABLE[0], MCS_TABLE[-1])
    transmissions.extend([m_explore] * EXPLORE_TX_PER_ROUND)

    round_attempts = np.zeros_like(attempts)
    round_success = np.zeros_like(success)

    for m in transmissions:
        per_this = true_per[:, m]
        rand = RNG.random(N_USERS)
        ack = (rand > per_this).astype(int)
        round_attempts[:, m] += 1
        round_success[:, m] += ack

    mask = round_attempts > 0
    attempts += round_attempts
    success += round_success
    recent_rate = np.zeros_like(s)
    np.divide(
        round_success, round_attempts,
        out=recent_rate, where=mask
    )
    s_candidate = ALPHA * s + (1 - ALPHA) * recent_rate
    per_est = 1.0 - s
    per_est = np.where(
        mask,
        1.0 - s_candidate,
        np.clip(per_est * NO_SAMPLE_DECAY, 0.0, 1.0)
    )
    s = 1.0 - per_est

    for u in range(N_USERS):
        s[u] = monotonic_user_success(s[u], attempts[u])

    values = s[:, m_curr]
    clusters = cluster_users(values)
    active_clusters = [
        cluster for cluster in clusters
        if len(cluster) / N_USERS >= ZETA
    ]
    if not active_clusters:
        largest = max(clusters, key=len)
        active_clusters.append(largest)

    weights = np.array([len(cluster) ** BETA for cluster in active_clusters], dtype=float)
    norm = weights.sum()
    S_c = [calc_cluster_success(cluster, s) for cluster in active_clusters]

    score_curr = 0.0
    for w, sc in zip(weights, S_c):
        score_curr += R_TABLE[m_curr] * w * (sc[m_curr] ** GAMMA)
    score_curr /= norm

    def score_at(m):
        if m < MCS_TABLE[0] or m > MCS_TABLE[-1]:
            return float('-inf')
        score = 0.0
        for w, sc in zip(weights, S_c):
            score += R_TABLE[m] * w * (sc[m] ** GAMMA)
        return score / norm

    score_next = score_at(m_curr + 1)
    score_prev = score_at(m_curr - 1)
    m_next = m_curr + 1
    attempts_next = attempts[:, m_next].sum() if m_next in MCS_TABLE else 0

    if (m_next in MCS_TABLE and
        score_next >= (1 + DELTA_UP) * score_curr and
        attempts_next >= N_MIN):
        hold_counter += 1
        if hold_counter >= HOLD_TIME_UP:
            m_curr = m_next
            hold_counter = 0
    else:
        hold_counter = 0

    m_prev = m_curr - 1
    if m_prev in MCS_TABLE and score_prev >= (1 + DELTA_DOWN) * score_curr:
        hold_counter_down += 1
        if hold_counter_down >= HOLD_TIME_DOWN:
            m_curr = max(m_prev, MCS_TABLE[0])
            hold_counter_down = 0
    else:
        hold_counter_down = 0

    round_history.append({
        "round": round_idx,
        "m_curr": int(m_curr),
        "score_curr": float(score_curr),
        "score_next": float(score_next),
        "score_prev": float(score_prev),
        "hold_counter": int(hold_counter),
        "hold_counter_down": int(hold_counter_down),
        "clusters": [cluster for cluster in active_clusters],
        "S_c_m_curr": [float(sc[m_curr]) for sc in S_c],
        "transmissions": list(transmissions),
        "per_curves": [1.0 - s_user.copy() for s_user in s],
    })

print("=== 模拟结果（前 50 轮） ===")
for info in round_history[:50]:
    print(
        f"[Round {info['round']:02d}] m_curr={info['m_curr']}, "
        f"Score(m)={info['score_curr']:.4f}, "
        f"Score(m+1)={info['score_next']:.4f}, "
        f"Score(m-1)={info['score_prev']:.4f}, "
        f"hold={info['hold_counter']}, "
        f"hold_down={info['hold_counter_down']}, "
        f"clusters={info['clusters']}, "
        f"S_c={['{:.4f}'.format(x) for x in info['S_c_m_curr']]}"
    )

# ----------------------
# 统计摘要与可视化
# ----------------------
rounds = np.array([info["round"] for info in round_history], dtype=int)
m_curr_hist = np.array([info["m_curr"] for info in round_history], dtype=int)
score_curr_hist = np.array([info["score_curr"] for info in round_history], dtype=float)
score_next_hist = np.array([info["score_next"] for info in round_history], dtype=float)
score_prev_hist = np.array([info["score_prev"] for info in round_history], dtype=float)

s_c_lists = [info["S_c_m_curr"] for info in round_history]
s_c_mean = np.array([float(np.mean(vals)) if vals else np.nan for vals in s_c_lists])
s_c_min = np.array([float(np.min(vals)) if vals else np.nan for vals in s_c_lists])
s_c_max = np.array([float(np.max(vals)) if vals else np.nan for vals in s_c_lists])

transmission_counter = Counter(m for info in round_history for m in info["transmissions"])
total_tx = sum(transmission_counter.values())

per_selected = np.array([
    [per_curve[user_idx][info["m_curr"]] for user_idx in range(N_USERS)]
    for info, per_curve in zip(round_history, [info["per_curves"] for info in round_history])
])

print("\n=== 统计摘要 ===")
if rounds.size:
    mcs_changes = np.diff(m_curr_hist)
    ups = int(np.sum(mcs_changes > 0))
    downs = int(np.sum(mcs_changes < 0))
    stable = int(np.sum(mcs_changes == 0)) + 1
    print(f"总轮数: {rounds.size}, 最终 MCS: {m_curr_hist[-1]}")
    print(f"档位变化次数: 升档 {ups} 次, 降档 {downs} 次, 保持 {stable} 轮")

if total_tx:
    print("发送占比:")
    for m in sorted(transmission_counter):
        count = transmission_counter[m]
        ratio = count / total_tx
        print(f"  MCS {m:02d}: {count:4d} 次 ({ratio:.1%})")

fig, axes = plt.subplots(4, 1, figsize=(10, 16), sharex=True)

axes[0].step(rounds, m_curr_hist, where="post", label="m_curr")
axes[0].set_ylabel("MCS Level")
axes[0].set_title("Selected MCS per Round")
axes[0].grid(True, linestyle="--", alpha=0.3)

axes[1].plot(rounds, score_curr_hist, label="Score(current MCS)", color="#1f77b4")
axes[1].plot(rounds, score_next_hist, label="Score(current+1)", color="#ff7f0e")
axes[1].plot(rounds, score_prev_hist, label="Score(current-1)", color="#2ca02c")
axes[1].set_ylabel("Score")
axes[1].set_title("Score Comparison vs Adjacent Levels (per Round)")
axes[1].legend(loc="best")
axes[1].grid(True, linestyle="--", alpha=0.3)

axes[2].plot(rounds, s_c_mean, label="Active Cluster Mean Success", color="#9467bd")
axes[2].fill_between(rounds, s_c_min, s_c_max, color="#c5b0d5", alpha=0.4,
                    label="Active Cluster Success Range")
axes[2].set_ylabel("Success Rate")
axes[2].set_xlabel("Round")
axes[2].set_title("Active Cluster Success Statistics")
axes[2].set_ylim(0, 1)
axes[2].legend(loc="best")
axes[2].grid(True, linestyle="--", alpha=0.3)

for user_idx in range(N_USERS):
    axes[3].plot(rounds, per_selected[:, user_idx], label=f"User {user_idx}")

axes[3].set_ylabel("PER")
axes[3].set_xlabel("Round")
axes[3].set_title("Per-User PER at Selected MCS")
axes[3].set_ylim(0, 1)
axes[3].grid(True, linestyle="--", alpha=0.3)
axes[3].legend(loc="upper right", ncol=2)

fig.tight_layout()

output_path = Path(__file__).with_name("mcs_sim_demo_results.png")
fig.savefig(output_path, dpi=150)
plt.close(fig)

print(f"可视化图表已保存至: {output_path}")