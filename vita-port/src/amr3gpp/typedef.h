/*
 * ===================================================================
 *  TS 26.104
 *  R99   V3.5.0 2003-03
 *  REL-4 V4.5.0 2003-06
 *  REL-5 V5.2.0 2003-06
 *  3GPP AMR Floating-point Speech Codec
 * ===================================================================
 *
 */
/* PS Vita port: the original file hard-coded PC types and used `long`
 * for Word32. On a 32-bit ARM target long IS 32 bits, but this makes the
 * fixed-width intent explicit (and keeps the code correct if it is ever
 * compiled with 64-bit long, which the host unit test does). */

#ifndef _TYPEDEF_H
#define _TYPEDEF_H

#include <stdint.h>

typedef int8_t   Word8;
typedef uint8_t  UWord8;
typedef int16_t  Word16;
typedef int32_t  Word32;
typedef float    Float32;
typedef double   Float64;

#endif
