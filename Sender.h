#ifndef IDNC_C_SENDER_H
#define IDNC_C_SENDER_H

#include "Symbol.h"
#include <stdio.h>
#include <stdlib.h>

#define bool int
#define true 1
#define false 0


typedef struct {
//    static vector<vector<int>> SFM;
//    static vector<Symbol*> ackSymbol;

//    static void formSFM();
//    static bool isSFMAll0();
//    pair<int, vv_int> func_limit_partition(vv_int sfm, int limit);

    VectorSymbol packets;

}Sender;

// clique算法相关结构体
// 定义结构体来代替 pair
typedef struct {
    int* first;
    int first_size;
    int* second;
    int second_size;
} int_pair;

typedef struct {
    int cd;
    int** solution;
    int* solution_sizes;
    int solution_count;
} partition_result;


Sender*  initSender(char **source, int K, int T);
partition_result func_limit_partition(int** sfm, int rows, int cols, int limit);
VectorSymbol encode(partition_result part_res, Symbol** Packets);
bool isSFMAll0(int** sfm, int row, int col);


#endif //IDNC_C_SENDER_H
