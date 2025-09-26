#include "Symbol.h"

Esi newEsi(int size) {
    Esi esi;
    esi.arr = (int*)malloc(size * sizeof(int));
    esi.size = size;
    return esi;
}

VectorSymbol newVectorSymbol(int size) {
    VectorSymbol vs;
    vs.symbols = (Symbol **)malloc(sizeof(Symbol*) * size);
    vs.size = size;
    return vs;
}

void fillData(Symbol* sym, const char *src, int size)
{
    if(sym == NULL) return;
    sym->nbytes = size;
    // 只记录元数据，不存储实际数据
}

Symbol* xxor(const Symbol* s1, const Symbol* s2)
{
    if (s1 == NULL || s2 == NULL) return NULL;
    
    // 检查ESI数组是否有效
    if (s1->esi.arr == NULL || s2->esi.arr == NULL) return NULL;
    if (s1->esi.size <= 0 || s2->esi.size <= 0) return NULL;

    int i;
    if (s1->nbytes != s2->nbytes)
        printf("Error! try to xor symbols with unmatched size\n");

    Symbol* tmp = (Symbol*)malloc(sizeof(Symbol));
    tmp->nbytes = s1->nbytes;
    
    // 合并ESI（元数据异或）
    tmp->esi = newEsi(s1->esi.size + s2->esi.size);
    for(i=0; i<s1->esi.size; i++) {
        tmp->esi.arr[i] = s1->esi.arr[i];
    }
    for(i=0; i<s2->esi.size; i++) {
        tmp->esi.arr[s1->esi.size + i] = s2->esi.arr[i];
    }
    tmp->isCoded = 1;  // 标记为编码包
    
    return tmp;
}

