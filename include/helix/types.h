#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

#define PAGE_SIZE       4096u
#define PAGE_SHIFT      12u
#define LARGE_PAGE_SIZE (2u * 1024u * 1024u)

static inline u64 align_up_u64(u64 x, u64 a)
{
    return (x + a - 1) & ~(a - 1);
}

static inline u64 align_down_u64(u64 x, u64 a)
{
    return x & ~(a - 1);
}
