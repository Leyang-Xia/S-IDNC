#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "Symbol.h"
#include "Sender.h"
#include "Receiver.h"

// 简化的symbol信息打印（调试用，可选）
void printSymbol(Symbol* symbol) {
    if(!symbol) return;
    printf("ESI[%d]: ", symbol->esi.size);
    for(int i=0; i<symbol->esi.size; i++) {
        printf("%d%s", symbol->esi.arr[i], (i<symbol->esi.size-1)?",":"");
    }
    printf(" (coded=%d)\n", symbol->isCoded);
}

// 分配内存并初始化SFM矩阵
int** initSFM(int revers_num, int K) {
    int** sfm = (int**)malloc(revers_num * sizeof(int*));
    for (int i = 0; i < revers_num; ++i) {
        sfm[i] = (int*)malloc(K * sizeof(int));
    }
    return sfm;
}

//生成SFM矩阵
void formSFM(int** SFM, Receiver* receivers, int n) {
    for(int i=0; i<n; i++) {
        if (rand()/(RAND_MAX + 1.0) > 0.1) {
            for(int j=0; j<receivers[i].rev_status.size; j++) {
                    SFM[i][j] = receivers[i].rev_status.arr[j];
            }
        }
    }
}

// S-IDNC仿真参数（可直接修改）
#define DEFAULT_K 32        // 源包数量
#define DEFAULT_RECEIVERS 4 // 接收者数量  
#define DEFAULT_LOSSRATE 0.8 // 丢包率

int main() {
    // 初始化随机数种子
    srand(time(NULL));
    
    int roundCount = 1;
    int i,j;
    
    // 仿真参数
    int K = DEFAULT_K;
    int T = 0;  // 元数据模式
    double lossrate = DEFAULT_LOSSRATE;
    int num_rsver = DEFAULT_RECEIVERS;
    
    printf("Simulation parameters: K=%d, receivers=%d, loss_rate=%.2f\n", K, num_rsver, lossrate);
    
    int** SFM; //全局变量

    // 创建源包（元数据模式）
    VectorSymbol packets = createPackets(NULL, K, T);

    // 初始化接收者
    Receiver  *rcvers = (Receiver*) malloc(sizeof(Receiver) * num_rsver);
    for(i=0; i<num_rsver; i++) {
        rcvers[i] = initReceiver(K);
    }

    // 初始传输阶段
    int total_sent = K * num_rsver, total_received_initial = 0;
    for (i=0; i< K; i++) {
        for(j=0; j<num_rsver; j++) {
            if (rand()/(RAND_MAX + 1.0) > lossrate) {
                rcvers[j] = receiveSymbol(rcvers[j], packets.symbols[i]);
                total_received_initial++;
            }
        }
    }
    double actual_loss_rate = 1.0 - (double)total_received_initial / total_sent;
    printf("Initial: %d/%d received (loss: %.2f)\n", total_received_initial, total_sent, actual_loss_rate);

    // 初始化状态反馈矩阵(SFM)
    SFM = initSFM(num_rsver, K);
    formSFM(SFM,rcvers, num_rsver);
    // printf("Round %d started\n", roundCount);

    while(!isSFMAllzero(SFM, num_rsver, K)) {
        // S-IDNC编码：clique算法包配对
        int limit = 2;
        partition_result pairs = func_limit_partition(SFM, num_rsver, K, limit);

        // 生成编码包
        VectorSymbol symbolVec = encode(pairs, packets.symbols);
        // printf("Generated %d coded packets\n", symbolVec.size);

        // 传输编码包
        for (i=0; i< symbolVec.size; i++) {
            for(j=0; j<num_rsver; j++) {
                if (rand()/(RAND_MAX + 1.0) > lossrate) {
                    rcvers[j] = receiveSymbol(rcvers[j], symbolVec.symbols[i]);
                }
            }
        }

        // 重新生成SFM矩阵
        formSFM(SFM,rcvers, num_rsver);
        roundCount++;
    }

    printf("Simulation completed after %d rounds\n", roundCount-1);
    return 0;
}
