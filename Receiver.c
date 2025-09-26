#include "Receiver.h"
//K是目标收包数
Receiver initReceiver(int K) {
    Receiver receiver;
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

Receiver receiveSymbol(Receiver receiver, const Symbol* sym) {
    if(sym == NULL) return receiver;
    int n = (int)sym->esi.size; //symbol中包含包的个数
    int id1=-1, id2=-1; // 收到包的id
    if(n == 1) { //原始包
        id1 = sym->esi.arr[0];
        // 检查ID是否在有效范围内
        if(id1 < 0 || id1 >= receiver.symbol_map.size) {
            return receiver;
        }
        if(receiver.symbol_map.pktid[id1] == -1) { //没有这个包则存起来
            receiver.symbol_map.pktid[id1] = id1;
            receiver.symbol_map.symbols[id1] = sym;
            receiver.rev_status.arr[id1]=0;
            receiver.pkt_recv++;
        }
    } else if(n == 2) {
        id1 = sym->esi.arr[0];
        id2 = sym->esi.arr[1];
        
        // 检查ID是否在有效范围内
        if(id1 < 0 || id1 >= receiver.symbol_map.size || id2 < 0 || id2 >= receiver.symbol_map.size) {
            return receiver;
        }
        
        if(receiver.symbol_map.pktid[id1] == -1 && receiver.symbol_map.pktid[id2] == -1) {
            return receiver;
        }
        if(receiver.symbol_map.pktid[id1] != -1 && receiver.symbol_map.pktid[id2] != -1) {
            return receiver;
        }
        Symbol* decoded_sym = NULL;
        if(receiver.symbol_map.pktid[id1] == -1) {
            // 检查symbols[id2]是否为NULL
            if(receiver.symbol_map.symbols[id2] == NULL) {
                return receiver;
            }
            decoded_sym = xxor(sym, receiver.symbol_map.symbols[id2]);
            if(decoded_sym == NULL) {
                return receiver;
            }
            decoded_sym->esi = newEsi(1);
            decoded_sym->esi.arr[0] = id1;
            decoded_sym->isCoded = 0;
            receiver.symbol_map.pktid[id1] = id1;
            receiver.symbol_map.symbols[id1] = decoded_sym;
            receiver.rev_status.arr[id1]=0;
            receiver.pkt_recv++;
        } else if(receiver.symbol_map.pktid[id2] == -1) {
            // 检查symbols[id1]是否为NULL
            if(receiver.symbol_map.symbols[id1] == NULL) {
                return receiver;
            }
            decoded_sym = xxor(sym, receiver.symbol_map.symbols[id1]);
            if(decoded_sym == NULL) {
                return receiver;
            }
            decoded_sym->esi = newEsi(1);
            decoded_sym->esi.arr[0] = id2;
            decoded_sym->isCoded = 0;
            receiver.symbol_map.pktid[id2] = id2;
            receiver.symbol_map.symbols[id2] = decoded_sym;
            receiver.rev_status.arr[id2]=0;
            receiver.pkt_recv++;
        }
    }
    return receiver;
}
