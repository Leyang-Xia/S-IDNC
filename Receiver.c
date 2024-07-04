#include "Receiver.h"
//K是目标收包数
Receiver initReceiver(Receiver receiver, int K) {
    receiver.pkt_recv = 0;
    //初始化收包状态
    receiver.rev_status.arr = (int*)malloc(K * sizeof (int));
    receiver.rev_status.size = K;
    for(int i=0; i<K; i++) {
        receiver.rev_status.arr[i] = 1;
    }
    //初始化Symbol map
    receiver.symbol_map.size = K;
    receiver.symbol_map.pktid = (int*) malloc(K * sizeof (int));
    receiver.symbol_map.symbols = (Symbol**) malloc(K * sizeof (Symbol*));
    for(int i=0; i<K; i++) {
        receiver.symbol_map.pktid[i] = -1;
        receiver.symbol_map.symbols[i] = NULL;
    }
    return receiver;
}

void receiveSymbol(Receiver* receiver, Symbol* sym) {
    if(sym == NULL) return ;
    int n = (int)sym->esi.size; //symbol中包含包的个数
    int id1=-1, id2=-1; // 收到包的id
    if(n == 1) { //原始包
        id1 = sym->esi.arr[0];
        if(receiver->symbol_map.pktid[id1] == -1) { //没有这个包则存起来
            receiver->symbol_map.pktid[id1] = id1;
            receiver->symbol_map.symbols[id1] = sym;
            //接收者根据收包id给收包状态置位,0代表接收到，rev_status初始化为全1
            receiver->rev_status.arr[id1]=0;
            //统计接收者收到原始包的个数
            receiver->pkt_recv++;
        } else {
            return;
        }
    } else if(n == 2) {
        id1 = sym->esi.arr[0];
        id2 = sym->esi.arr[1];
        //两个包都没有，无法解
        if(receiver->symbol_map.pktid[id1] == -1 && receiver->symbol_map.pktid[id2] == -1) {
            return;
        }
        //两个包都有，无需解
        if(receiver->symbol_map.pktid[id1] != -1 && receiver->symbol_map.pktid[id2] != -1) {
            return;
        }
        //存解出的包
        Symbol* decoded_sym = NULL;
        //有包id2, 解id1
        if(receiver->symbol_map.pktid[id1] == -1) {
            decoded_sym = xxor(sym, receiver->symbol_map.symbols[id2]);
            decoded_sym->esi = newEsi(decoded_sym->esi, 1);
            decoded_sym->esi.arr[0] = id1;
            decoded_sym->isCoded = 0;
            receiver->symbol_map.pktid[id1] = id1;
            receiver->symbol_map.symbols[id1] = decoded_sym; //解出的symbol存入revceive map
            //接收者根据收包id给收包状态置位,0代表接收到，rev_status初始化为全1
            receiver->rev_status.arr[id1]=0;
        } else if(receiver->symbol_map.pktid[id2] == -1) { // 有包id1, 解id2
            decoded_sym = xxor(sym, receiver->symbol_map.symbols[id1]);
            decoded_sym->esi = newEsi(decoded_sym->esi, 1);;
            decoded_sym->esi.arr[0] = id2;
            decoded_sym->isCoded = 0;
            receiver->symbol_map.pktid[id2] = id2;
            receiver->symbol_map.symbols[id2] = decoded_sym; //解出的symbol存入revceive map
            //接收者根据收包id给收包状态置位,0代表接收到，rev_status初始化为全1
            receiver->rev_status.arr[id2]=0;
        }
        //统计接收者收到原始包的个数
        receiver->pkt_recv++;
    }
}
