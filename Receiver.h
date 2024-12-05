#ifndef IDNC_C_RECEIVER_H
#define IDNC_C_RECEIVER_H

#include "Symbol.h"
typedef struct {
    int size;
    int* pktid;
    Symbol** symbols;
}MapSymbol; //key: pktId，存放解出的包

typedef struct {
    VectorInt rev_status; //收到的包对应位置置0
    MapSymbol symbol_map;
    int pkt_recv; //收到包的个数
}Receiver;

Receiver initReceiver(int K);
Receiver receiveSymbol(Receiver receiver, const Symbol* sym);

#endif //IDNC_C_RECEIVER_H
