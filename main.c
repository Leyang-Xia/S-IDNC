#include <stdio.h>
#include "Symbol.h"
#include "Sender.h"
#include "Receiver.h"

// 打印symbol内容
void printSymbol(Symbol* symbol) {
    if(!symbol) return;
    printf("esi: ");
    int i,j;
    for(i=0; i<symbol->esi.size; i++) {
        printf("%d, ",symbol->esi.arr[i]);
    }
    printf("nbytes=%d bytes, \"", symbol->nbytes);
    for(j=0; j<symbol->nbytes/4; j++) {
        printf("%d ",symbol->data[j]); // 0x33323130 转成10进制为858927408
    }
    printf("\"\n");
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
        for(int j=0; j<receivers[i].rev_status.size; j++) {
            SFM[i][j] = receivers[i].rev_status.arr[j];
        }
    }
}

int main() {
    printf("Hello, World!\n");
    int i,j,K,T;
    double lossrate;
    K = 20;
    T = 4;
    lossrate = 0.3;
    int** SFM; //全局变量

    char** source = (char**)malloc(K * sizeof(char*));
    for(i=0; i<K; i++) {
        source[i] = (char*)malloc(T * sizeof(char));
        for(j=0; j<T; j++) {
            source[i][j] = (char)(j + '0');
        }
    }
    for (i=0; i<K; i++)
    {
        for (j=0; j<T; j++) {
            printf("%c ", source[i][j]);
        }
    }
    printf("\n");

    // 初始化Sender结构体
    Sender*  sender = initSender((char**)source, K, T);
    int pkt_num = sender->packets.size;
    printf("source symbol is :\n");
    for(i=0; i<pkt_num; i++) {
        Symbol* sym = sender->packets.symbols[i];
        printSymbol(sym);
    }

    //构造receiver
    int num_rsver=10; //接收者个数
    Receiver  *rcvers = (Receiver*) malloc(sizeof(Receiver) * num_rsver);
    for(i=0; i<num_rsver; i++) {
        rcvers[i] = initReceiver(rcvers[i], K);
    }

    //发送原始包，生成SFM矩阵
    //初始时，sender发送原始包给每个接受者
    for (i=0; i< K; i++) { //逐包发给每个接收者
        for(j=0; j<num_rsver; j++) {
            if (rand()/(RAND_MAX + 1.0) > lossrate) {
                // 因为sender构造函数已经初始化了K个Symbol包对象，这里可以直接拿出来用
                // 记录收包的状态，收到原始包数统计，移到Receiver::receiveSymbol()函数实现
                receiveSymbol(&rcvers[j], sender->packets.symbols[i]);
            }
        }
    }
    //打印收包状态
    for(i=0; i<num_rsver; i++) {
        //cout<<"receiver "<<r.id<<" receive pkts num is "<<r.pkt_recv<<endl;
        printf("receiver %d receive pkts num is %d\n",i,rcvers[i].pkt_recv);
    }

    // 打印receiver当前收到的原始包
    for(j=0; j<num_rsver; j++) {
        printf("receiver%d receive %d source pkts:\n", j, rcvers[j].pkt_recv);
        for(i=0; i<rcvers[j].symbol_map.size; i++) {
            if(rcvers[j].symbol_map.pktid[i] != -1) {
                printSymbol(rcvers[j].symbol_map.symbols[i]);
            }
        }
        printf("\n");
    }

    //初始化SFM, K为包个数
    SFM = initSFM(num_rsver, K);
    formSFM(SFM,rcvers, num_rsver);
    printf("SFM matrix is:\n");
    for(i=0; i<num_rsver; i++) {
        for(j=0; j<K; j++) {
            printf("%d ", SFM[i][j]);
        }
        printf("\n");
    }

    while(!isSFMAll0(SFM, num_rsver, K)) {
        //clique算法进行包配对
        int limit = 2;
        partition_result pairs = func_limit_partition(SFM, num_rsver, K, limit);

        //生成编码包列表
        VectorSymbol symbolVec = encode(pairs, sender->packets.symbols);
        printf("print encoded pkts:\n");
        for(i=0; i<symbolVec.size; i++) {
            printSymbol(symbolVec.symbols[i]);
        }
        printf("\n");

        //发送编码包到接收方，接收方收包并解码
        for (i=0; i< symbolVec.size; i++) { //逐包发给每个接收者
            for(j=0; j<num_rsver; j++) {
                if (rand()/(RAND_MAX + 1.0) > lossrate) {
                    // 记录收包的状态，收到原始包数统计，在receiveSymbol()函数实现
                    // 解码出来的包，会记录到对应receiver的结构体中
                    receiveSymbol(&rcvers[j], symbolVec.symbols[i]);
                }
            }
        }

        // 打印receiver当前收到的原始包
        for(j=0; j<num_rsver; j++) {
            printf("receiver%d receive %d source pkts:\n", j, rcvers[j].pkt_recv);
            for(i=0; i<rcvers[j].symbol_map.size; i++) {
                if(rcvers[j].symbol_map.pktid[i] != -1) {
                    printSymbol(rcvers[j].symbol_map.symbols[i]);
                }
            }
            printf("\n");
        }

        // 重新生成SFM矩阵
        formSFM(SFM,rcvers, num_rsver);
    }

    printf("end");
    return 0;
}
