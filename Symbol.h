#ifndef IDNC_C_SYMBOL_H
#define IDNC_C_SYMBOL_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int* arr;
    int size;
}VectorInt;

typedef struct {
    int *arr;
    int size;
}Esi;

typedef struct  {
    int *data;
    int nbytes;
    Esi esi; /* encoding symbol id */
    int isCoded; //区分是0源码包，还是1编码包
} Symbol;

typedef struct {
    Symbol** symbols;
    int size;
}VectorSymbol;

Esi newEsi(Esi esi, int size);
VectorSymbol newVectorSymbol(int size);
void fillData(Symbol* sym, char *src, int size);
Symbol* xxor(Symbol *s1, Symbol *s2);

#endif //IDNC_C_SYMBOL_H
