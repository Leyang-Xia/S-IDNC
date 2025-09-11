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

// 检查SFM是否全为0
int is_sfm_all_zero() {
    for(int i = 0; i < NUM_RECEIVERS; i++) {
        for(int j = 0; j < K; j++) {
            if(sfm[i][j] != 0) return 0;
        }
    }
    return 1;
}

// 计算包的度数（有多少接收者需要它）
int calculate_degree(int** sfm_work, int packet_id) {
    int degree = 0;
    for(int i = 0; i < NUM_RECEIVERS; i++) {
        if(sfm_work[i][packet_id] > 0) degree++;
    }
    return degree;
}

// 核心算法：getClique - 查找最大权重的团
Clique get_clique(int** sfm_work) {
    Clique result = {NULL, 0};
    
    // 找到所有丢失的包
    int lost_packets[K];
    int lost_count = 0;
    
    for(int j = 0; j < K; j++) {
        for(int i = 0; i < NUM_RECEIVERS; i++) {
            if(sfm_work[i][j] > 0) {
                lost_packets[lost_count++] = j;
                break;
            }
        }
    }
    
    if(lost_count == 0) return result;
    
    // 计算每个包的权重
    int* weights = (int*)malloc(lost_count * sizeof(int));
    for(int i = 0; i < lost_count; i++) {
        int degree = calculate_degree(sfm_work, lost_packets[i]);
        weights[i] = (degree == 0) ? 0 : lost_count / degree;
    }
    
    // 找到最大权重的包
    int max_idx = 0;
    for(int i = 1; i < lost_count; i++) {
        if(weights[i] > weights[max_idx]) {
            max_idx = i;
        }
    }
    
    int selected_packet = lost_packets[max_idx];
    
    // 创建团：从选中的包开始
    result.packets = (int*)malloc(MAX_CLIQUE_SIZE * sizeof(int));
    result.packets[0] = selected_packet;
    result.size = 1;
    
    // 尝试添加第二个包形成大小为2的团
    for(int i = 0; i < lost_count && result.size < MAX_CLIQUE_SIZE; i++) {
        if(lost_packets[i] != selected_packet) {
            // 检查这两个包是否能形成有效的编码对
            int can_pair = 0;
            for(int r = 0; r < NUM_RECEIVERS; r++) {
                if(sfm_work[r][selected_packet] > 0 && sfm_work[r][lost_packets[i]] > 0) {
                    can_pair = 1;
                    break;
                }
            }
            if(can_pair) {
                result.packets[result.size++] = lost_packets[i];
                break;
            }
        }
    }
    
    free(weights);
    return result;
}

// 核心算法：func_limit_partition - S-IDNC分区算法
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
    int main_iteration = 0;
    int max_main_iterations = K;
    
    while(remaining_vertices > 0 && main_iteration < max_main_iterations && result.count < max_solutions) {
        // 使用getClique找到团
        Clique clique = get_clique(sfm_work);
        
        if(clique.size == 0) break; // 没有找到有效团，退出
        
        // 保存团到解决方案
        result.solutions[result.count] = (int*)malloc(clique.size * sizeof(int));
        memcpy(result.solutions[result.count], clique.packets, clique.size * sizeof(int));
        result.solution_sizes[result.count] = clique.size;
        
        // 更新顶点状态
        for(int i = 0; i < clique.size; i++) {
            int v = clique.packets[i];
            if(vertex_status[v]) {
                vertex_status[v] = 0;
                remaining_vertices--;
            }
        }
        
        // 在工作副本中移除已处理的顶点
        for(int i = 0; i < NUM_RECEIVERS; i++) {
            for(int j = 0; j < clique.size; j++) {
                sfm_work[i][clique.packets[j]] = 0;
            }
        }
        
        result.count++;
        main_iteration++;
        
        // 清理团内存
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
                    // 编码包：S-IDNC解码逻辑
                    int id1 = solution[0], id2 = solution[1];
                    if(received[r][id1] && !received[r][id2]) {
                        received[r][id2] = 1; // 解码出id2
                    } else if(!received[r][id1] && received[r][id2]) {
                        received[r][id1] = 1; // 解码出id1
                    } else if(!received[r][id1] && !received[r][id2]) {
                        // 两个包都没有，存储编码包（简化处理）
                        received[r][id1] = 1;
                        received[r][id2] = 1;
                    }
                    // 如果两个包都已有，编码包无用
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
