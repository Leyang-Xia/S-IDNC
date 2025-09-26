#include "Sender.h"

// 创建原始包
VectorSymbol createPackets(char **source, int K, int T) {
    VectorSymbol packets;
    packets.symbols = (Symbol**)malloc(sizeof(Symbol*) * K);
    packets.size = K;
    for(int i=0; i<K; i++) {
        Symbol* sym = (Symbol*)malloc(sizeof(Symbol));
        // 只设置元数据，不处理实际数据
        sym->esi.arr = (int*)malloc(sizeof(int));
        sym->esi.arr[0] = i;
        sym->esi.size = 1;
        sym->isCoded = 0;
        sym->nbytes = T;
        packets.symbols[i] = sym;
    }
    return packets;
}
/**
 * encode - 实现编码包的生成
 * Input: 分区结果和原始包
 * Output: 编码后的包
 */
VectorSymbol encode(partition_result part_res, Symbol** Packets) {
    int row = part_res.solution_count;
    VectorSymbol vs = newVectorSymbol(row);
    
    // 对每个团生成编码包
    for(int i=0; i<row; i++) {
        int col = part_res.solution_sizes[i];
        int id1 = part_res.solution[i][0];
        Symbol* encoded_sym = NULL;
        
        // if K = {vk}，直接使用原始包
        if (col == 1) {
            encoded_sym = Packets[id1];
            encoded_sym->esi = newEsi(1);
            encoded_sym->esi.arr[0] = id1;
            encoded_sym->isCoded = 0;
        } 
        // if K = {vk,vk'}，生成异或编码包
        else if (col > 1) {
            int id2 = part_res.solution[i][1];
            encoded_sym = xxor(Packets[id1], Packets[id2]);
            encoded_sym->esi = newEsi(2);
            encoded_sym->esi.arr[0] = id1;
            encoded_sym->esi.arr[1] = id2;
            encoded_sym->isCoded = 1;
        }
        vs.symbols[i] = encoded_sym;
    }
    return vs;
}

/**
 * isSFMAllzero - 检查SFM矩阵是否全为0
 * @param sfm: 状态反馈矩阵(SFM)
 * @param row: 接收者数量 N
 * @param col: 包的总数 K
 * @return: 如果SFM矩阵全为0，返回true，否则返回false
 */
bool isSFMAllzero(int** sfm, int row, int col) {
    for (int i = 0; i < row; i++) {
        for (int j = 0; j < col; j++) {
            if (sfm[i][j] != 0) {
                return false;
            }
        }
    }
    return true;
}


bool findones(int** sfm, int row, int col) {
    for (int i = 0; i < row; i++) {
        for (int j = 0; j < col; j++) {
            if (sfm[i][j] != 0) {
                return true;
            }
        }
    }
    return false;
}

/**
 * CalculateDegree - 计算顶点的度数（degree）
 * 
 * 在S-IDNC图中，顶点 vk 的权重计算公式为：
 * wk = |Tk| / δk
 * 其中：
 * - |Tk| 是需要包k的接收者集合的大小（在代码中对应 P_bewant[i]）
 * - δk 是顶点 vk 的度数，即与其相连的边的数量（该函数的返回值）
 * 
 * @param sfm: 状态反馈矩阵(SFM)
 * @param rows: 接收者数量 N
 * @param cols: 包的总数 K
 * @param lost_packets: 丢失的包的集合 V
 * @param packet_current: 当前顶点对应的包 pk
 * 
 * @return: 顶点的度数 δk
 */
int CalculateDegree(int** sfm, int rows, int cols, VectorInt lost_packets, int packet_current) {
    int degree = 0;

    for (int i = 0; i < lost_packets.size; ++i) {
        if (lost_packets.arr[i] == packet_current) continue; // 跳过当前包

        for (int j = 0; j < rows; ++j) {
            if (sfm[j][packet_current] + sfm[j][lost_packets.arr[i]] > 1) {
                degree++; //找到一条边
                break;
            }
        }
    }

    return degree;
}

/**
 * getClique - 实现算法1：在图中寻找包含最多两个顶点的团
 * Input: Graph (V, E) - 通过 sfm 矩阵表示
 * Output: Clique K - 通过 VectorInt 返回
 */
VectorInt getClique(int** sfm, int rows, int cols, int limit) {
    // Initialization: K = ∅
    VectorInt V_keep;
    V_keep.arr = (int*)malloc(cols * sizeof(int));
    V_keep.size = 0;
    
    // 创建sfm的副本用于计算
    int** sfmAlter = (int**)malloc(rows * sizeof(int*));
    for (int i = 0; i < rows; ++i) {
        sfmAlter[i] = (int*)malloc(cols * sizeof(int));
        memcpy(sfmAlter[i], sfm[i], cols * sizeof(int));
    }

    while (findones(sfmAlter, rows, cols)) {
        if (V_keep.size >= limit) break;

        // Step 1: ∀vk ∈ V, compute wk using (52)
        int* weights = (int*)malloc(cols * sizeof(int));
        int* sfmAlter_sum = (int*)calloc(cols, sizeof(int));
        // 计算每列和作为权重的一部分
        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < cols; ++j) {
                sfmAlter_sum[j] += sfmAlter[i][j];
            }
        }

        // 构建顶点集 V
        int* P_bewant = (int*)malloc(cols * sizeof(int));
        int* LostPacket = (int*)malloc(cols * sizeof(int));
        int P_bewant_size = 0;
        int LostPacket_size = 0;
        for (int i = 0; i < cols; ++i) {
            if (sfmAlter_sum[i] != 0) {
                P_bewant[P_bewant_size++] = sfmAlter_sum[i];
                LostPacket[LostPacket_size++] = i;
            }
        }

        // 计算每个顶点的权重
        VectorInt lostPacketVec;
        lostPacketVec.arr = LostPacket;
        lostPacketVec.size = LostPacket_size;
        for (int i = 0; i < LostPacket_size; ++i) {
            int degree = CalculateDegree(sfmAlter, rows, cols, lostPacketVec, LostPacket[i]);
            weights[i] = degree == 0 ? 
                         P_bewant[i] * 1000 :  // 如果度数为0，给予高权重
                         P_bewant[i] / degree;  // 否则按公式计算权重
        }

        // Step 2: Select vk = argmax{wk}
        if (LostPacket_size == 0) {
            free(weights);
            free(sfmAlter_sum);
            free(P_bewant);
            free(LostPacket);
            break;  // 没有丢失的包，退出循环
        }
        
        int max_idx = 0;
        int max_weight = weights[0];
        for (int i = 0; i < LostPacket_size; i++) {
            if (weights[i] >= max_weight) {
                max_weight = weights[i];
                max_idx = i;
            }
        }

        // Step 3: Set K = {vk}
        int V_maxWeight = LostPacket[max_idx];
        V_keep.arr[V_keep.size++] = V_maxWeight;

        // Step 4: Set V' = {vk': (vk,vk') ∈ E}
        int degree = CalculateDegree(sfmAlter, rows, cols, lostPacketVec, V_maxWeight);
        
        // Step 5-6: Remove vk from V
        for (int i = 0; i < rows; ++i) {
            sfmAlter[i][V_maxWeight] = 0;
        }

        // 移除相邻边
        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < degree; ++j) {
                sfmAlter[i][LostPacket[j]] = 0;
            }
        }

        // 清理内存
        free(weights);
        free(P_bewant);
        free(LostPacket);
        free(sfmAlter_sum);
    }

    // 清理内存
    for (int i = 0; i < rows; ++i) {
        free(sfmAlter[i]);
    }
    free(sfmAlter);

    return V_keep;  // 返回找到的团 K
}

/**
 * func_limit_partition - 实现算法2：构造S-IDNC解决方案
 * Input: Graph (V, E) - 通过 sfm 矩阵表示
 * Output: S-IDNC solution Pr
 *   @param sfm: 状态反馈矩阵
 * @param rows: 接收者数量
 * @param cols: 包的总数
 * @param limit: 团的最大顶点数限制（通常为2）
 */
partition_result func_limit_partition(int** sfm, int rows, int cols, int limit) {
    partition_result res;
    
    
    // 输入验证
    if (sfm == NULL || rows <= 0 || cols <= 0 || limit <= 0) {
        res.solution = NULL;
        res.solution_sizes = NULL;
        res.solution_count = 0;
        return res;
    }
    
    // 计算最大可能的解决方案数量：保守估计，考虑最坏情况
    int max_solutions = cols;  // 最坏情况下每个包都是单独的解  
    // 为解决方案数组分配内存
    int** Solution = (int**)malloc(max_solutions * sizeof(int*));
    int* Solution_sizes = (int*)malloc(max_solutions * sizeof(int));
    int Solution_count = 0;  // 当前解决方案数量

    // 创建状态反馈矩阵(SFM)的工作副本
    int** sfm_w = (int**)malloc(rows * sizeof(int*));
    for (int i = 0; i < rows; ++i) {
        sfm_w[i] = (int*)malloc(cols * sizeof(int));
        memcpy(sfm_w[i], sfm[i], cols * sizeof(int));  // 复制原始SFM
    }

    // 初始化顶点状态跟踪数组：0表示已处理，1表示待处理
    int* vertex_status = (int*)calloc(cols, sizeof(int));
    int remaining_vertices = 0;  // 剩余待处理的顶点数
    // 统计需要处理的顶点数量
    for (int j = 0; j < cols; ++j) {
        for (int i = 0; i < rows; ++i) {
            if (sfm[i][j] > 0) {  // 如果有接收者需要这个包
                vertex_status[j] = 1;  // 标记为待处理
                remaining_vertices++;
                break;
            }
        }
    }

    // 主循环：持续处理直到所有顶点都被处理
    while (remaining_vertices > 0) {
        // 使用getClique找到一个最多包含limit个顶点的团
        VectorInt clique = getClique(sfm_w, rows, cols, limit);
        
        if (clique.size > 0) {  // 如果找到了有效的团
            // 保存这个团到解决方案数组
            Solution[Solution_count] = (int*)malloc(clique.size * sizeof(int));
            memcpy(Solution[Solution_count], clique.arr, clique.size * sizeof(int));
            Solution_sizes[Solution_count] = clique.size;
            
            // 更新顶点状态：将团中的顶点标记为已处理
            for (int i = 0; i < clique.size; ++i) {
                int v = clique.arr[i];
                if (vertex_status[v]) {  // 如果顶点之前未处理
                    vertex_status[v] = 0;  // 标记为已处理
                    remaining_vertices--;  // 更新剩余顶点计数
                }
            }
            
            // 在SFM工作副本中移除已处理的顶点
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < clique.size; ++j) {
                    sfm_w[i][clique.arr[j]] = 0;
                }
            }
            
            Solution_count++;  // 增加解决方案计数
        } else {
            // printf("No clique found, breaking loop\n");
            free(clique.arr);  // 释放临时团数组
            break;  // 没有找到团，跳出循环
        }
        
        free(clique.arr);  // 释放临时团数组
    }

    // 设置结果
    res.solution = Solution;
    res.solution_sizes = Solution_sizes;
    res.solution_count = Solution_count;

    // 清理临时内存
    free(vertex_status);
    for (int i = 0; i < rows; ++i) {
        free(sfm_w[i]);
    }
    free(sfm_w);

    return res;
}
