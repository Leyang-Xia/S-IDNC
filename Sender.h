#ifndef IDNC_C_SENDER_H
#define IDNC_C_SENDER_H

#include "Symbol.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>


typedef struct {
    int** solution;
    int* solution_sizes;
    int solution_count;
} partition_result;

// 函数声明
VectorSymbol createPackets(char **source, int K, int T);
partition_result func_limit_partition(int** sfm, int rows, int cols, int limit);
VectorSymbol encode(partition_result part_res, Symbol** Packets);
bool isSFMAllzero(int** sfm, int row, int col);
int CalculateDegree(int** sfm, int rows, int cols, VectorInt lost_packets, int packet_current);
VectorInt getClique(int** sfm, int rows, int cols, int limit);

#endif //IDNC_C_SENDER_H
