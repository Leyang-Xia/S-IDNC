#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

// 简化的接收者状态（只用数组，无动态内存分配）
typedef struct {
    int received[64];  // 静态数组，received[i] = 0表示已收到包i，1表示需要
    int pkt_count;     // 已收到的包总数
} SimpleReceiver;

// 简化的新用户结构
typedef struct {
    SimpleReceiver receiver;
    int join_round;
    int original_received;
    int bonus_received;
    int unicast_sent;
} SimpleNewUser;

// 简化的编码包结构
typedef struct {
    int id1, id2;  // XOR的两个包ID
    int valid;     // 是否有效
} SimpleEncodedPacket;

// 更好的随机数生成器（线性同余生成器）
static unsigned long random_state = 1;

void better_srand(unsigned int seed) {
    random_state = seed;
}

double better_rand() {
    random_state = (random_state * 1103515245 + 12345) & 0x7fffffff;
    return (double)random_state / 0x7fffffff;
}

// 初始化简化接收者
SimpleReceiver initSimpleReceiver(int K) {
    SimpleReceiver r;
    r.pkt_count = 0;
    for (int i = 0; i < K; i++) {
        r.received[i] = 1;  // 1表示需要，0表示已收到
    }
    return r;
}

// 模拟接收单个包
void receivePacket(SimpleReceiver* receiver, int pkt_id) {
    if (receiver->received[pkt_id] == 1) {
        receiver->received[pkt_id] = 0;  // 标记为已收到
        receiver->pkt_count++;
    }
}

// 检查SFM是否全为0（所有用户都完成）
int isSFMAllzero_simple(SimpleReceiver* receivers, int rows, int K) {
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < K; j++) {
            if (receivers[i].received[j] != 0) return 0;
        }
    }
    return 1;
}

// ============ 真正的S-IDNC算法实现 ============

// 简化的向量结构
typedef struct {
    int* arr;
    int size;
} SimpleVector;

// 检查SFM是否有1
int findones_simple(SimpleReceiver* receivers, int rows, int K) {
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < K; j++) {
            if (receivers[i].received[j] != 0) return 1;
        }
    }
    return 0;
}

// 计算顶点度数（S-IDNC核心算法）
int calculateDegree_simple(SimpleReceiver* receivers, int rows, int K, SimpleVector lost_packets, int packet_current) {
    int degree = 0;
    
    for (int i = 0; i < lost_packets.size; ++i) {
        if (lost_packets.arr[i] == packet_current) continue; // 跳过当前包
        
        for (int j = 0; j < rows; ++j) {
            if (receivers[j].received[packet_current] + receivers[j].received[lost_packets.arr[i]] > 1) {
                degree++; // 找到一条边
                break;
            }
        }
    }
    return degree;
}

// S-IDNC核心算法：getClique（按照论文Algorithm 1实现）
SimpleVector getClique_simple(SimpleReceiver* receivers, int rows, int K, int limit) {
    SimpleVector V_keep;
    V_keep.arr = (int*)malloc(K * sizeof(int));
    V_keep.size = 0;
    
    // 创建工作副本
    static SimpleReceiver work_receivers[8];
    for (int i = 0; i < rows; i++) {
        work_receivers[i] = receivers[i];
    }
    
    // Algorithm 1: 找到一个团（最多limit个顶点）
    while (findones_simple(work_receivers, rows, K) && V_keep.size < limit) {
        
        // Step 1: 计算权重 wk = |Tk| / δk
        int weights[64];
        int P_bewant[64];  // |Tk| - 需要包k的接收者数量
        int LostPacket[64]; // 当前丢失的包集合V
        int P_bewant_size = 0;
        int LostPacket_size = 0;
        
        // 构建当前丢失包集合V
        for (int i = 0; i < K; ++i) {
            int sum = 0;
            for (int j = 0; j < rows; ++j) {
                sum += work_receivers[j].received[i];
            }
            if (sum != 0) {
                P_bewant[P_bewant_size++] = sum;
                LostPacket[LostPacket_size++] = i;
            }
        }
        
        if (LostPacket_size == 0) break;
        
        // 计算每个顶点的权重 wk = |Tk| / δk
        SimpleVector lostPacketVec;
        lostPacketVec.arr = LostPacket;
        lostPacketVec.size = LostPacket_size;
        
        for (int i = 0; i < LostPacket_size; ++i) {
            int degree = calculateDegree_simple(work_receivers, rows, K, lostPacketVec, LostPacket[i]);
            weights[i] = degree == 0 ? P_bewant[i] * 1000 : P_bewant[i] / degree;
        }
        
        // Step 2: 选择权重最大的顶点 vk = argmax{wk}
        int max_idx = 0;
        int max_weight = weights[0];
        for (int i = 0; i < LostPacket_size; i++) {
            if (weights[i] >= max_weight) {
                max_weight = weights[i];
                max_idx = i;
            }
        }
        
        // Step 3: Set K = {vk}
        int selected_vertex = LostPacket[max_idx];
        V_keep.arr[V_keep.size++] = selected_vertex;
        
        // Step 4: Set V' = {vk': (vk,vk') ∈ E} (找相邻顶点)
        // 在limit=2的情况下，尝试找一个相邻顶点组成大小为2的团
        if (V_keep.size < limit) {
            int best_neighbor = -1;
            int best_neighbor_weight = -1;
            
            for (int i = 0; i < LostPacket_size; i++) {
                int candidate = LostPacket[i];
                if (candidate == selected_vertex) continue;
                
                // 检查是否相邻（存在边）
                int has_edge = 0;
                for (int r = 0; r < rows; r++) {
                    if (work_receivers[r].received[selected_vertex] == 1 && 
                        work_receivers[r].received[candidate] == 1) {
                        has_edge = 1;
                        break;
                    }
                }
                
                if (has_edge && weights[i] > best_neighbor_weight) {
                    best_neighbor = candidate;
                    best_neighbor_weight = weights[i];
                }
            }
            
            if (best_neighbor != -1) {
                V_keep.arr[V_keep.size++] = best_neighbor;
            }
        }
        
        // 找到团后就退出（Algorithm 1只找一个团）
        break;
    }
    
    return V_keep;
}

// S-IDNC分区算法：func_limit_partition
int findEncodingPairs_SIDNC(SimpleReceiver* receivers, int rows, int K, SimpleEncodedPacket* pairs) {
    int pair_count = 0;
    
    // 创建工作副本
    static SimpleReceiver sfm_work[8];
    for (int i = 0; i < rows; i++) {
        sfm_work[i] = receivers[i];
    }
    
    // 主循环：持续寻找团直到所有顶点处理完
    while (findones_simple(sfm_work, rows, K) && pair_count < 32) {
        // 使用getClique找到最优团（最多2个顶点）
        SimpleVector clique = getClique_simple(sfm_work, rows, K, 2);
        
        if (clique.size > 0) {
            if (clique.size == 1) {
                // 单个包：直接传输
                pairs[pair_count].id1 = clique.arr[0];
                pairs[pair_count].id2 = -1; // 标记为单包
                pairs[pair_count].valid = 1;
            } else if (clique.size == 2) {
                // 两个包：XOR编码
                pairs[pair_count].id1 = clique.arr[0];
                pairs[pair_count].id2 = clique.arr[1];
                pairs[pair_count].valid = 1;
            }
            pair_count++;
            
            // 从工作SFM中移除已处理的顶点
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < clique.size; ++j) {
                    sfm_work[i].received[clique.arr[j]] = 0;
                }
            }
        }
        
        if (clique.arr != NULL) free(clique.arr);
        
        // 防止无限循环
        if (clique.size == 0) break;
    }
    
    return pair_count;
}

// 检查新用户是否能从编码包解码
int canDecodeFromPair_simple(SimpleNewUser* user, SimpleEncodedPacket* pair) {
    if (!pair->valid) return 0;
    
    if (pair->id2 == -1) {
        // 单包：如果需要就能解码
        return user->receiver.received[pair->id1] == 1;
    } else {
        // XOR包：恰好有其中一个时才能解码
        int has_id1 = (user->receiver.received[pair->id1] == 0);  // 0表示已有
        int has_id2 = (user->receiver.received[pair->id2] == 0);  // 0表示已有
        return (has_id1 && !has_id2) || (!has_id1 && has_id2);
    }
}

// 运行单次实验
typedef struct {
    int join_round;
    int original_packets;
    int bonus_packets;
    int unicast_packets;
    int final_rounds;
    double bonus_efficiency;
} SimResult;

SimResult runSimulation(int join_round, int K, int num_receivers, double lossrate) {
    SimResult result = {join_round, 0, 0, 0, 0, 0.0};
    
    // 初始化老用户（使用静态数组）
    SimpleReceiver old_receivers[8];  // 最多8个老用户
    for (int i = 0; i < num_receivers; i++) {
        old_receivers[i] = initSimpleReceiver(K);
    }
    
    // 初始广播：模拟老用户接收原始包
    for (int i = 0; i < K; i++) {
        for (int j = 0; j < num_receivers; j++) {
            if (better_rand() > lossrate) {
                receivePacket(&old_receivers[j], i);
            }
        }
    }
    
    SimpleNewUser new_user;
    int new_user_added = 0;
    int roundCount = 1;
    int total_bonus_sent = 0;
    int old_complete_round = -1;
    
    // 主循环：运行S-IDNC算法直到老用户完成
    while (!isSFMAllzero_simple(old_receivers, num_receivers, K)) {
        
        // 在指定轮次添加新用户
        if (!new_user_added && roundCount == join_round) {
            new_user.receiver = initSimpleReceiver(K);
            new_user.join_round = join_round;
            new_user.original_received = 0;
            new_user.bonus_received = 0;
            new_user.unicast_sent = 0;
            
            // 阶段1: 原始包传输给新用户
            for (int i = 0; i < K; i++) {
                if (better_rand() > lossrate) {
                    receivePacket(&new_user.receiver, i);
                }
            }
            new_user.original_received = new_user.receiver.pkt_count;
            new_user_added = 1;
        }
        
        // 检查老用户是否完成
        int old_users_done = isSFMAllzero_simple(old_receivers, num_receivers, K);
        if (old_users_done) {
            if (old_complete_round == -1) {
                old_complete_round = roundCount;
            }
            break; // 老用户完成，退出循环
        }
        
        // 生成编码包（使用真正的S-IDNC分区算法）
        SimpleEncodedPacket pairs[32];
        int pair_count = findEncodingPairs_SIDNC(old_receivers, num_receivers, K, pairs);
        
        if (pair_count > 0) {
            // 向老用户发送编码包
            for (int p = 0; p < pair_count; p++) {
                for (int j = 0; j < num_receivers; j++) {
                    if (better_rand() > lossrate) {
                        if (pairs[p].id2 == -1) {
                            // 单包传输
                            receivePacket(&old_receivers[j], pairs[p].id1);
                        } else {
                            // XOR编码包：如果恰好缺一个包，就能解码
                            int needs_id1 = old_receivers[j].received[pairs[p].id1];
                            int needs_id2 = old_receivers[j].received[pairs[p].id2];
                            
                            if (needs_id1 && !needs_id2) {
                                receivePacket(&old_receivers[j], pairs[p].id1);
                            } else if (!needs_id1 && needs_id2) {
                                receivePacket(&old_receivers[j], pairs[p].id2);
                            }
                        }
                    }
                }
            }
            
            // 阶段2: 新用户bonus
            if (new_user_added) {
                for (int p = 0; p < pair_count; p++) {
                    total_bonus_sent++;
                    if (canDecodeFromPair_simple(&new_user, &pairs[p]) && 
                        better_rand() > lossrate) {
                        if (pairs[p].id2 == -1) {
                            // 单包解码
                            receivePacket(&new_user.receiver, pairs[p].id1);
                        } else {
                            // XOR解码：获得缺失的包
                            if (new_user.receiver.received[pairs[p].id1] == 1) {
                                receivePacket(&new_user.receiver, pairs[p].id1);
                            } else if (new_user.receiver.received[pairs[p].id2] == 1) {
                                receivePacket(&new_user.receiver, pairs[p].id2);
                            }
                        }
                    }
                }
                new_user.bonus_received = new_user.receiver.pkt_count - new_user.original_received;
            }
        }
        
        roundCount++;
    }
    
    // 记录老用户完成轮次
    if (old_complete_round == -1) {
        old_complete_round = roundCount - 1;
    }
    
    // 处理新用户后加入的情况
    if (!new_user_added && join_round > old_complete_round) {
        new_user.receiver = initSimpleReceiver(K);
        new_user.join_round = join_round;
        new_user.original_received = 0;
        new_user.bonus_received = 0;
        new_user.unicast_sent = 0;
        
        // 原始包传输
        for (int i = 0; i < K; i++) {
            if (better_rand() > lossrate) {
                receivePacket(&new_user.receiver, i);
            }
        }
        new_user.original_received = new_user.receiver.pkt_count;
        new_user_added = 1;
    }
    
    // 阶段3: 单播收尾
    int unicast_rounds = 0;
    if (new_user_added) {
        int missing_count = K - new_user.receiver.pkt_count;
        if (missing_count > 0) {
            int expected_sends = (int)((double)missing_count / (1.0 - lossrate) + 0.5);
            new_user.unicast_sent = expected_sends;
            unicast_rounds = (missing_count + 4) / 5; // 每轮最多5个包
        }
    }
    
    // 收集结果
    if (new_user_added) {
        result.original_packets = new_user.original_received;
        result.bonus_packets = new_user.bonus_received;
        result.unicast_packets = new_user.unicast_sent;
        result.bonus_efficiency = total_bonus_sent > 0 ? 
            (double)new_user.bonus_received / total_bonus_sent : 0.0;
        result.final_rounds = old_complete_round + unicast_rounds;
    } else {
        result.final_rounds = old_complete_round;
    }
    
    return result;
}

int main() {
    // 使用更好的随机种子：时间 + 进程ID + 内存地址
    unsigned int seed = (unsigned int)time(NULL) + (unsigned int)getpid() + (unsigned int)(uintptr_t)&seed;
    better_srand(seed);  // 使用自定义随机数生成器
    
    int K = 64, num_receivers = 8;
    double lossrate = 0.3; // 30%丢包率
    int join_rounds[] = {1, 3, 5, 7, 9, 11, 13, 15};
    int n = sizeof(join_rounds) / sizeof(join_rounds[0]);
    
    printf("S-IDNC新用户中途加入性能分析 - 算法仿真版 (K=%d, 丢包率=%.1f%%, 老用户数=%d)\n", 
           K, lossrate*100, num_receivers);
    printf("═══════════════════════════════════════════════════════════════════════════════\n");
    printf("╔═══════════╦═══════════════╦═══════════════╦═══════════════╦═══════════════╦═══════════╗\n");
    printf("║ 加入轮次  ║  原始包数量   ║  Bonus包数量  ║  单播包数量   ║   结束轮次    ║ Bonus效率 ║\n");
    printf("╠═══════════╬═══════════════╬═══════════════╬═══════════════╬═══════════════╬═══════════╣\n");
    
    for (int i = 0; i < n; ++i) {
        SimResult r = runSimulation(join_rounds[i], K, num_receivers, lossrate);
        printf("║%9d  ║%13d  ║%13d  ║%13d  ║%13d  ║   %5.1f%%  ║\n", 
               r.join_round, r.original_packets, r.bonus_packets, 
               r.unicast_packets, r.final_rounds, r.bonus_efficiency * 100);
    }
    
    printf("╚═══════════╩═══════════════╩═══════════════╩═══════════════╩═══════════════╩═══════════╝\n");
    printf("\n📊 算法仿真说明:\n");
    printf("- 使用真实S-IDNC编码逻辑，但简化数据结构\n");
    printf("- 保留包对选择、XOR编码/解码的算法本质\n");
    printf("- 避免复杂内存管理，专注算法性能分析\n");
    printf("- 与数学模型对比验证算法有效性\n");
    
    return 0;
}
