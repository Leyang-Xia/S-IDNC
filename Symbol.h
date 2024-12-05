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
    int* arr;
    int size;
}Esi;

typedef struct  {
    int* data;
    int nbytes;
    Esi esi;
    int isCoded;
} Symbol;

typedef struct {
    Symbol** symbols;
    int size;
}VectorSymbol;

Esi newEsi(int size);
VectorSymbol newVectorSymbol(int size);
void fillData(Symbol* sym, const char* src, int size);
Symbol* xxor(const Symbol* s1, const Symbol* s2);

#endif //IDNC_C_SYMBOL_H
