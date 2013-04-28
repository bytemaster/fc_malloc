#include <stdint.h>
#include <memory.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <assert.h>
#include <unistd.h>
#ifdef _MSC_VER
#pragma intrinsic(__rdtsc)
uint64_t get_cc_time () {
    return __rdtsc();
}
#else
/* define this somewhere */
#ifdef __i386
__inline__ uint64_t rdtsc() {
     uint64_t x;
       __asm__ volatile ("rdtsc" : "=A" (x));
         return x;
}
#elif __amd64
__inline__ uint64_t rdtsc() {
     uint64_t a, d;
       __asm__ volatile ("rdtsc" : "=a" (a), "=d" (d));
         return a; //(d<<32) | a;
}
#endif


uint64_t get_cc_time () {
   return rdtsc();
}
#endif


// Some primes between 2^63 and 2^64 for various uses.
// source: CityHash
static const uint64_t k0 = 0xc3a5c85c97cb3127ULL;
static const uint64_t k1 = 0xb492b66fbe98f273ULL;
static const uint64_t k2 = 0x9ae16a3b2f90404fULL;

inline uint64_t ShiftMix(uint64_t val) { return val ^ (val >> 47); }

uint64_t fast_rand()
{
  int64_t now = rdtsc(); //get_cc_time();
  char*   s = (char*)&now; // note first 4 bits are 'LSB' on intel... 
                           // on bigendian machine we want to add 4
                           // LSB is most rand, the higher-order bits
                           // will not change much if at all between
                           // calls...

  const uint8_t a = s[0];
  const uint8_t b = s[4 >> 1];
  const uint8_t c = s[4 - 1];
  const uint32_t y = static_cast<uint32_t>(a) + (static_cast<uint32_t>(b) << 8);
  const uint32_t z = 4 + (static_cast<uint32_t>(c) << 2);
  return ShiftMix(y * k2 ^ z * k0) * k2;
}
