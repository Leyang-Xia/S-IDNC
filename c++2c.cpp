#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// 定义结构体来代替 pair
typedef struct {
    int* first;
    int first_size;
    int* second;
    int second_size;
} int_pair;

// 辅助函数：查找数组中的元素
bool findElement(int** arr, int rows, int cols, int value) {
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; j++) {
            if (arr[i][j] == value) {
                return true;
            }
        }
    }
    return false;
}

// 辅助函数：构建集合 C 和 U
int_pair GetSet(int** sfm, int rows, int cols, int* lost_packets, int lost_size, int packet_current) {
    int* c_set = (int*)malloc(lost_size * sizeof(int));
    int* u_set = (int*)malloc(lost_size * sizeof(int));
    int c_set_size = 0;
    int u_set_size = 0;
    int* diff_set = (int*)malloc(lost_size * sizeof(int));
    int diff_set_size = 0;

    for (int i = 0; i < lost_size; ++i) {
        if (lost_packets[i] != packet_current) {
            diff_set[diff_set_size++] = lost_packets[i];
        }
    }
    
    for (int i = 0; i < diff_set_size; ++i) {
        int* temp = (int*)malloc(rows * sizeof(int));
        for (int j = 0; j < rows; ++j) {
            temp[j] = sfm[j][packet_current] + sfm[j][diff_set[i]];
        }

        bool result = false;
        for (int k = 0; k < rows; ++k) {
            if (temp[k] > 1) {
                result = true;
                break;
            }
        }
        if (result) {
            c_set[c_set_size++] = diff_set[i];
        } else {
            u_set[u_set_size++] = diff_set[i];
        }
        free(temp);
    }

    free(diff_set);

    int_pair res;
    res.first = c_set;
    res.first_size = c_set_size;
    res.second = u_set;
    res.second_size = u_set_size;

    return res;
}

// 获得 Clique 集合
int* getClique(int** sfm, int rows, int cols, int limit, int* result_size) {
    int* V_keep = (int*)malloc(cols * sizeof(int));
    int V_keep_size = 0;
    int** sfmAlter = (int**)malloc(rows * sizeof(int*));
    for (int i = 0; i < rows; ++i) {
        sfmAlter[i] = (int*)malloc(cols * sizeof(int));
        for (int j = 0; j < cols; ++j) {
            sfmAlter[i][j] = sfm[i][j];
        }
    }

    while (findElement(sfmAlter, rows, cols, 1)) {
        if (V_keep_size >= limit)
            break;

        double* weights = (double*)malloc(cols * sizeof(double));
        int* sfmAlter_sum = (int*)calloc(cols, sizeof(int));
        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < cols; ++j) {
                sfmAlter_sum[j] += sfmAlter[i][j];
            }
        }

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

        for (int i = 0; i < LostPacket_size; ++i) {
            int_pair res = GetSet(sfmAlter, rows, cols, LostPacket, LostPacket_size, LostPacket[i]);
            double degree = 0.001;
            if (res.second_size > degree)
                degree = res.second_size;
            weights[i] = (double)P_bewant[i] / degree;
            free(res.first);
            free(res.second);
        }

        int max_idx = 0;
        double max_weight = weights[0];
        for (int i = 0; i < LostPacket_size; i++) {
            if (weights[i] >= max_weight) {
                max_weight = weights[i];
                max_idx = i;
            }
        }
        free(weights);

        int V_maxWeight = LostPacket[max_idx];
        V_keep[V_keep_size++] = V_maxWeight;

        int_pair sets = GetSet(sfmAlter, rows, cols, LostPacket, LostPacket_size, V_maxWeight);
        int* c_set = sets.first;
        int c_set_size = sets.first_size;

        printf("V_maxWeight = %d\n", V_maxWeight);
        printf("c_set = ");
        for (int i = 0; i < c_set_size; ++i) {
            printf("%d,", c_set[i]);
        }
        printf("\n");

        for (int i = 0; i < rows; ++i) {
            sfmAlter[i][V_maxWeight] = 0;
        }
        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < c_set_size; ++j) {
                sfmAlter[i][c_set[j]] = 0;
            }
        }
        free(c_set);
        free(P_bewant);
        free(LostPacket);
        free(sfmAlter_sum);

    }

    printf("V_keep = ");
    for (int i = 0; i < V_keep_size; ++i) {
        printf("%d,", V_keep[i]);
    }
    printf("\n");

    for (int i = 0; i < rows; ++i) {
        free(sfmAlter[i]);
    }
    free(sfmAlter);

    *result_size = V_keep_size;
    return V_keep;
}

// 限制分区函数，它返回一个分区方案和cd的数目
typedef struct {
    int cd;
    int** solution;
    int* solution_sizes;
    int solution_count;
} partition_result;

partition_result func_limit_partition(int** sfm, int rows, int cols, int limit) {
    partition_result res;
    
    int* V = (int*)malloc(cols * sizeof(int));
    int V_size = 0;

    for (int j = 0; j < cols; ++j) {
        for (int i = 0; i < rows; ++i) {
            if (sfm[i][j] > 0) {
                V[V_size++] = j;
                break;
            }
        }
    }
    
    printf("V arr is: \n");
    for (int i = 0; i < V_size; ++i) {
        printf("%d,", V[i]);
    }
    printf("\n");
    
    int** sfm_w = (int**)malloc(rows * sizeof(int*));
    for (int i = 0; i < rows; ++i) {
        sfm_w[i] = (int*)malloc(cols * sizeof(int));
        for (int j = 0; j < cols; ++j) {
            sfm_w[i][j] = sfm[i][j];
        }
    }

    int cd = 0;
    int** Solution = (int**)malloc(cols * sizeof(int*));
    int* Solution_sizes = (int*)malloc(cols * sizeof(int));
    int Solution_count = 0;

    while (V_size > 0) {
        int V_keep_size;
        int* V_keep = getClique(sfm_w, rows, cols, limit, &V_keep_size);

        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < V_keep_size; ++j) {
                sfm_w[i][V_keep[j]] = 0;
            }
        }
        
        int* temp = (int*)malloc(cols * sizeof(int));
        int temp_size = 0;

        for (int j = 0; j < V_size; ++j) {
            bool found = false;
            for (int k = 0; k < V_keep_size; ++k) {
                if (V[j] == V_keep[k]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                temp[temp_size++] = V[j];
            }
        }

        free(V);
        V = temp;
        V_size = temp_size;

        ++cd;
        if (V_keep_size > 0) {
            // sort V_keep (just simple sort for example)
            for (int i = 0; i < V_keep_size - 1; ++i) {
                for (int j = 0; j < V_keep_size - i - 1; ++j) {
                    if (V_keep[j] > V_keep[j + 1]) {
                        int t = V_keep[j];
                        V_keep[j] = V_keep[j + 1];
                        V_keep[j + 1] = t;
                    }
                }
            }
            Solution[Solution_count] = V_keep;
            Solution_sizes[Solution_count] = V_keep_size;
            ++Solution_count;
        }
    }

    res.cd = cd;
    res.solution = Solution;
    res.solution_sizes = Solution_sizes;
    res.solution_count = Solution_count;

    for (int i = 0; i < rows; ++i) {
        free(sfm_w[i]);
    }
    free(sfm_w);
    free(V);

    return res;
}

int main() {
    // 假设有一个3x3的矩阵
    int sfm_data[3][3] = {
        {1, 0, 1},
        {0, 1, 1},
        {1, 1, 0}
    };

    int rows = 3;
    int cols = 3;
    int limit = 2;

    // 分配内存并初始化矩阵
    int** sfm = (int**)malloc(rows * sizeof(int*));
    for (int i = 0; i < rows; ++i) {
        sfm[i] = (int*)malloc(cols * sizeof(int));
        for (int j = 0; j < cols; ++j) {
            sfm[i][j] = sfm_data[i][j];
        }
    }

    // 调用函数
    partition_result res = func_limit_partition(sfm, rows, cols, limit);

    // 打印结果
    printf("cd: %d\n", res.cd);
    for (int i = 0; i < res.solution_count; ++i) {
        printf("Solution %d: ", i+1);
        for (int j = 0; j < res.solution_sizes[i]; ++j) {
            printf("%d ", res.solution[i][j]);
        }
        printf("\n");
    }

    // 释放内存
    for (int i = 0; i < res.solution_count; ++i) {
        free(res.solution[i]);
    }
    free(res.solution);
    free(res.solution_sizes);

    for (int i = 0; i < rows; ++i) {
        free(sfm[i]);
    }
    free(sfm);

    return 0;
}
