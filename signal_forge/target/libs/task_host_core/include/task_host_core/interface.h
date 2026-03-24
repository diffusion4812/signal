#ifndef INTERFACE_H
#define INTERFACE_H

#include <stddef.h>
#include <stdint.h>

#define STATIC
#define INPUT
#define OUTPUT

typedef float REAL;

#pragma pack(push, 1)
typedef struct {
    void** slots;
} TaskContext;
#pragma pack(pop)

#endif