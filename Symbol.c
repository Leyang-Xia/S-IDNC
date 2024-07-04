#include "Symbol.h"

Esi newEsi(Esi esi, int size) {
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

void fillData(Symbol* sym, char *src, int size)
{
    if(sym == NULL) return;
    if (sym->nbytes != size) {
        sym->data = (int*)malloc(size); //int[size/sizeof(int)];
    }
    memcpy(sym->data, src, size);
}

Symbol* xxor(Symbol *s1, Symbol *s2)
{
    if (s1 == NULL || s2 == NULL) return NULL;

    int i;
    if (s1->nbytes != s2->nbytes)
        printf("Error! try to xor symbols with unmatched size\n");

    Symbol* tmp = (Symbol*)malloc(sizeof(Symbol));
    tmp->data = (int*) malloc(s2->nbytes );

    for (i=0;i<s1->nbytes/4; i++)
        tmp->data[i] = s1->data[i] ^ s2->data[i];
    tmp->nbytes = s1->nbytes;
    return tmp;

}

