#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

// 仿真参数（可修改）
#define K 64              // 源包数量
#define NUM_RECEIVERS 8   // 接收者数量
#define LOSS_RATE 0.4     // 丢包率
#define MAX_CLIQUE_SIZE 2 // 最大团大小

// 全局数据结构
int received[NUM_RECEIVERS][K];  // 接收状态矩阵
int sfm[NUM_RECEIVERS][K];       // 状态反馈矩阵

// 团查找结果
typedef struct {
    int* packets;    // 包ID数组
    int size;        // 团大小
} Clique;

// 分区结果
typedef struct {
    int** solutions;     // 解决方案数组
    int* solution_sizes; // 每个解决方案的大小
    int count;           // 解决方案数量
} PartitionResult;

// 初始化接收状态
void init_simulation() {
    for(int i = 0; i < NUM_RECEIVERS; i++) {
        for(int j = 0; j < K; j++) {
            received[i][j] = 0;
            sfm[i][j] = 1; // 初始时所有包都需要
        }
    }
}

// 更新SFM矩阵
void update_sfm() {
    for(int i = 0; i < NUM_RECEIVERS; i++) {
        for(int j = 0; j < K; j++) {
            sfm[i][j] = (received[i][j] == 0) ? 1 : 0;
        }
    }
}

/**
 * is_sfm_all_zero - 检查SFM矩阵是否全为0
 * @return: 如果SFM矩阵全为0返回1，否则返回0
 */
int is_sfm_all_zero() {
    for(int i = 0; i < NUM_RECEIVERS; i++) {
        for(int j = 0; j < K; j++) {
            if(sfm[i][j] != 0) return 0;
        }
    }
    return 1;
}

// 辅助：检查矩阵是否存在非零元素
static int has_nonzero(int** m, int rows, int cols) {
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            if (m[i][j] != 0) return 1;
        }
    }
    return 0;
}

/**
 * CalculateDegree - 计算顶点的度数（degree）
 *
 * 在S-IDNC图中，顶点 vk 的权重计算公式为：
 * wk = |Tk| / δk
 * 其中：
 * - |Tk| 是需要包k的接收者集合的大小（在代码中对应列和 P_bewant[i]）
 * - δk 是顶点 vk 的度数，即与其相连的边的数量（该函数的返回值）
 *
 * @param sfm: 状态反馈矩阵(SFM)
 * @param rows: 接收者数量 N
 * @param cols: 包的总数 K
 * @param lost: 丢失包集合 V（列索引数组）
 * @param lost_size: 丢失包集合大小
 * @param packet_current: 当前顶点对应的包 pk
 *
 * @return: 顶点的度数 δk
 */
static int calculate_degree_sender(int** sfm, int rows, int cols, int* lost, int lost_size, int packet_current) {
    int degree = 0;
    for (int i = 0; i < lost_size; ++i) {
        int other = lost[i];
        if (other == packet_current) continue;
        for (int r = 0; r < rows; ++r) {
            if (sfm[r][packet_current] + sfm[r][other] > 1) {
                degree++;
                break;
            }
        }
    }
    return degree;
}
/**
 * get_clique - 实现算法1：在图中寻找包含最多两个顶点的团
 * Input: Graph (V, E) - 通过 sfm 矩阵表示
 * Output: Clique K - 通过 Clique 结构体返回
 */
Clique get_clique(int** sfm_work) {
    Clique result = {NULL, 0};

    // 创建可变工作副本
    int** sfmAlter = (int**)malloc(NUM_RECEIVERS * sizeof(int*));
    for (int i = 0; i < NUM_RECEIVERS; ++i) {
        sfmAlter[i] = (int*)malloc(K * sizeof(int));
        memcpy(sfmAlter[i], sfm_work[i], K * sizeof(int));
    }

    int* selected = (int*)malloc(MAX_CLIQUE_SIZE * sizeof(int));
    int selected_size = 0;

    while (has_nonzero(sfmAlter, NUM_RECEIVERS, K) && selected_size < MAX_CLIQUE_SIZE) {
        // 列和（每个包被多少接收者需要）
        int* col_sum = (int*)calloc(K, sizeof(int));
        for (int r = 0; r < NUM_RECEIVERS; ++r) {
            for (int c = 0; c < K; ++c) col_sum[c] += sfmAlter[r][c];
        }

        // 构造丢失包集合
        int* lost = (int*)malloc(K * sizeof(int));
        int* P_bewant = (int*)malloc(K * sizeof(int));
        int lost_size = 0;
        for (int c = 0; c < K; ++c) {
            if (col_sum[c] != 0) {
                lost[lost_size] = c;
                P_bewant[lost_size] = col_sum[c];
                lost_size++;
            }
        }

        if (lost_size == 0) {
            free(col_sum); free(lost); free(P_bewant);
            break;
        }

        // 计算权重（双精度）并选择最大者，加入稳定的tie-breaker
        double* weights = (double*)malloc(lost_size * sizeof(double));
        int* degrees = (int*)malloc(lost_size * sizeof(int));
        for (int i = 0; i < lost_size; ++i) {
            int deg = calculate_degree_sender(sfmAlter, NUM_RECEIVERS, K, lost, lost_size, lost[i]);
            degrees[i] = deg;
            double base = (deg == 0) ? ((double)P_bewant[i] * 16.0) : ((double)P_bewant[i] / (double)deg);
            weights[i] = base;
        }

        int max_idx = 0; 
        double max_weight = weights[0];
        const double eps = 1e-12;
        for (int i = 1; i < lost_size; ++i) {
            double diff = weights[i] - max_weight;
            if (diff > eps) {
                max_weight = weights[i];
                max_idx = i;
            } else if (diff > -eps && diff < eps) {
                // tie-breaker 1: P_bewant 更大者优先
                if (P_bewant[i] > P_bewant[max_idx]) {
                    max_idx = i; max_weight = weights[i];
                } else if (P_bewant[i] == P_bewant[max_idx]) {
                    // tie-breaker 2: 度数更小者优先
                    if (degrees[i] < degrees[max_idx]) {
                        max_idx = i; max_weight = weights[i];
                    } else if (degrees[i] == degrees[max_idx]) {
                        // tie-breaker 3: id更小者稳定选择
                        if (lost[i] < lost[max_idx]) {
                            max_idx = i; max_weight = weights[i];
                        }
                    }
                }
            }
        }

        int v = lost[max_idx];
        selected[selected_size++] = v;

        // 根据 Sender.c 思路，移除与 v 相邻（存在某接收者同时需要）的顶点列，再移除 v 列
        for (int i = 0; i < lost_size; ++i) {
            int u = lost[i];
            if (u == v) continue;
            int adjacent = 0;
            for (int r = 0; r < NUM_RECEIVERS; ++r) {
                if (sfmAlter[r][v] + sfmAlter[r][u] > 1) { adjacent = 1; break; }
            }
            if (adjacent) {
                for (int r = 0; r < NUM_RECEIVERS; ++r) sfmAlter[r][u] = 0;
            }
        }
        for (int r = 0; r < NUM_RECEIVERS; ++r) sfmAlter[r][v] = 0;

        free(weights);
        free(degrees);
        free(col_sum);
        free(lost);
        free(P_bewant);
    }

    if (selected_size > 0) {
        result.packets = (int*)malloc(selected_size * sizeof(int));
        memcpy(result.packets, selected, selected_size * sizeof(int));
        result.size = selected_size;
    }

    free(selected);
    for (int i = 0; i < NUM_RECEIVERS; ++i) free(sfmAlter[i]);
    free(sfmAlter);
    return result;
}

/**
 * limit_partition - 实现算法2：构造S-IDNC解决方案
 * Input: Graph (V, E) - 通过全局 sfm 矩阵表示
 * Output: S-IDNC solution Pr（多个团的集合，每团至多2个顶点）

 */
PartitionResult limit_partition() {
    PartitionResult result;
    result.solutions = NULL;
    result.solution_sizes = NULL;
    result.count = 0;
    
    // 创建SFM工作副本
    int** sfm_work = (int**)malloc(NUM_RECEIVERS * sizeof(int*));
    for(int i = 0; i < NUM_RECEIVERS; i++) {
        sfm_work[i] = (int*)malloc(K * sizeof(int));
        memcpy(sfm_work[i], sfm[i], K * sizeof(int));
    }
    
    // 分配解决方案数组
    int max_solutions = K;
    result.solutions = (int**)malloc(max_solutions * sizeof(int*));
    result.solution_sizes = (int*)malloc(max_solutions * sizeof(int));
    
    // 统计剩余需要处理的顶点
    int vertex_status[K];
    int remaining_vertices = 0;
    for(int j = 0; j < K; j++) {
        vertex_status[j] = 0;
        for(int i = 0; i < NUM_RECEIVERS; i++) {
            if(sfm[i][j] > 0) {
                vertex_status[j] = 1;
                remaining_vertices++;
                break;
            }
        }
    }
    
    // 主循环：持续处理直到所有顶点都被处理
    while (remaining_vertices > 0 && result.count < max_solutions) {
        Clique clique = get_clique(sfm_work);
        if (clique.size == 0) break;

        result.solutions[result.count] = (int*)malloc(clique.size * sizeof(int));
        memcpy(result.solutions[result.count], clique.packets, clique.size * sizeof(int));
        result.solution_sizes[result.count] = clique.size;

        for (int i = 0; i < clique.size; ++i) {
            int v = clique.packets[i];
            if (vertex_status[v]) { vertex_status[v] = 0; remaining_vertices--; }
        }

        for (int r = 0; r < NUM_RECEIVERS; ++r) {
            for (int j = 0; j < clique.size; ++j) sfm_work[r][clique.packets[j]] = 0;
        }

        result.count++;
        free(clique.packets);
    }
    
    // 清理工作副本
    for(int i = 0; i < NUM_RECEIVERS; i++) {
        free(sfm_work[i]);
    }
    free(sfm_work);
    
    return result;
}

// 模拟包传输
void transmit_packets(PartitionResult partition) {
    for(int s = 0; s < partition.count; s++) {
        int* solution = partition.solutions[s];
        int size = partition.solution_sizes[s];
        
        // 对每个接收者尝试传输
        for(int r = 0; r < NUM_RECEIVERS; r++) {
            if(rand() / (RAND_MAX + 1.0) > LOSS_RATE) {
                // 包成功传输
                if(size == 1) {
                    // 原始包
                    received[r][solution[0]] = 1;
                } else if(size == 2) {
                    // 编码包：严格的 XOR 逻辑
                    int id1 = solution[0], id2 = solution[1];
                    if(received[r][id1] && !received[r][id2]) {
                        received[r][id2] = 1; // 解码出id2
                    } else if(!received[r][id1] && received[r][id2]) {
                        received[r][id1] = 1; // 解码出id1
                    }
                    // 若两者皆未知或皆已知，无法带来新增信息
                }
            }
        }
    }
}

// 释放分区结果内存
void free_partition_result(PartitionResult* result) {
    if(result->solutions) {
        for(int i = 0; i < result->count; i++) {
            free(result->solutions[i]);
        }
        free(result->solutions);
    }
    if(result->solution_sizes) {
        free(result->solution_sizes);
    }
}

// 统计并打印状态
void print_status(int round) {
    int total_received = 0;
    for(int i = 0; i < NUM_RECEIVERS; i++) {
        for(int j = 0; j < K; j++) {
            if(received[i][j]) total_received++;
        }
    }
    printf("Round %d: %d/%d packets received\n", round, total_received, K * NUM_RECEIVERS);
}

int main() {
    srand(time(NULL));
    
    printf("S-IDNC面向过程仿真\n");
    printf("参数: K=%d, 接收者=%d, 丢包率=%.2f\n\n", K, NUM_RECEIVERS, LOSS_RATE);
    
    // 初始化
    init_simulation();
    
    // 阶段1：发送原始包
    printf("阶段1：初始传输\n");
    int initial_sent = 0, initial_received = 0;
    for(int i = 0; i < K; i++) {
        for(int r = 0; r < NUM_RECEIVERS; r++) {
            initial_sent++;
            if(rand() / (RAND_MAX + 1.0) > LOSS_RATE) {
                received[r][i] = 1;
                initial_received++;
            }
        }
    }
    
    double actual_loss = 1.0 - (double)initial_received / initial_sent;
    printf("初始传输: %d/%d 收到 (丢包率: %.2f)\n", initial_received, initial_sent, actual_loss);
    
    // 阶段2：S-IDNC编码传输
    printf("\n阶段2：S-IDNC编码传输\n");
    update_sfm();
    
    int round = 1;
    while(!is_sfm_all_zero() && round <= 100) {
        // 使用核心S-IDNC算法
        PartitionResult partition = limit_partition();
        
        if(partition.count == 0) {
            printf("无法找到有效编码，退出\n");
            break;
        }
        
        printf("Round %d: 生成%d个编码包\n", round, partition.count);
        
        // 传输编码包
        transmit_packets(partition);
        
        // 更新状态
        update_sfm();
        print_status(round);
        
        // 清理内存
        free_partition_result(&partition);
        
        round++;
    }
    
    printf("\n仿真完成！共用%d轮编码传输\n", round-1);
    
    return 0;
}
