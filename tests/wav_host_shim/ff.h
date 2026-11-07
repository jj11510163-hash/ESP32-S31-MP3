#pragma once

#include <stdint.h>
#include <stdio.h>

typedef uint32_t FSIZE_t;
typedef unsigned int UINT;
typedef struct {
    FILE *fp;
    FSIZE_t size;
} FIL;

#define FR_OK 0
#define FA_READ 1

int f_open(FIL *file, const char *path, unsigned char mode);
int f_close(FIL *file);
int f_read(FIL *file, void *dst, UINT bytes, UINT *read_bytes);
int f_lseek(FIL *file, FSIZE_t offset);
FSIZE_t f_tell(FIL *file);
FSIZE_t f_size(FIL *file);
