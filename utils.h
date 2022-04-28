#ifndef _URCU_UTILS_H
#define _URCU_UTILS_H

#include <errno.h>
#include <linux/futex.h>
#include <linux/membarrier.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

/* only compiler barrier */
#define	barrier() __asm__ __volatile__ ("":::"memory")

/* x86-TSO store-load barrier */
#define smp_mb() __asm__ __volatile__ ("mfence":::"memory")

/* x86-TSO store-load barrier for that lacks mfence instruction */
#define smp_mb2() __asm__ __volatile__ ("lock; addl $0,0(%%rsp)":::"memory")

/* improves the performance of spin-wait loops */
#define PAUSE() __asm__ __volatile__ ("rep; nop":::"memory")

#define membarrier_master() membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0)

#define membarrier_slave() barrier()

#define membarrier_register() membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0)

/* counts clock cycles */
static inline __attribute__((always_inline))
uint64_t get_cycles()
{
  unsigned int edx, eax;
  __asm__ __volatile__ ("rdtsc" : "=a" (eax), "=d" (edx));
  return (uint64_t)eax | ((uint64_t)edx) << 32;
}

static inline __attribute__((always_inline))
int futex(int32_t *uaddr, int futex_op, int32_t val, const struct timespec *timeout,
          int32_t *uaddr2, int32_t val3)
{
  return syscall(__NR_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

static inline __attribute__((always_inline))
int membarrier(int cmd, unsigned int flags, int cpu_id)
{
  return syscall(__NR_membarrier, cmd, flags, cpu_id);
}

#define terminate_(cause)                                       \
  do {                                                          \
    fprintf(stderr, "(%s:%s@%u error %s cause termination\n)",  \
            __FILE__, __func__, __LINE__, strerror(cause));     \
    abort();                                                    \
  } while (0)                                                   \

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* get the address of an object containing a field */
#define container_of(ptr, type, field)                          \
  ({                                                            \
    const __typeof__(((type *) NULL)->field) *_ptr = (ptr);     \
    (type *)((char *)_ptr - offsetof(type, field));             \
  })                                                            \

#define access_once(x) (*(__volatile__ __typeof__(x) *)&(x))

#define load_shared(x) ({ barrier(); access_once(x); })

#define store_shared(x, v) ({ access_once(x) = (v); barrier(); })

#define add_and_fetch(addr, v)                                  \
  ((__typeof__(*(addr))) sync_add_and_fetch((addr),             \
                                            (unsigned long)(v), \
                                            sizeof(*(addr))))   \

static inline __attribute__((always_inline))
unsigned long sync_add_and_fetch(void *addr, unsigned long val, int len)
{
  switch (len) {
  case 1:
    return __sync_add_and_fetch_1((uint8_t *)addr, val);
  case 2:
    return __sync_add_and_fetch_2((uint16_t *)addr, val);
  case 4:
    return __sync_add_and_fetch_4((uint32_t *)addr, val);
  case 8:
    return __sync_add_and_fetch_8((uint64_t *)addr, val);
  }
  return 0;
}

#define or_and_fetch(addr, v)                                   \
  ((__typeof__(*(addr))) sync_or_and_fetch((addr),              \
                                           (unsigned long)(v),  \
                                           sizeof(*(addr))))    \

static inline __attribute__((always_inline))
unsigned long sync_or_and_fetch(void *addr, unsigned long val, int len)
{
  switch (len) {
  case 1:
    return __sync_or_and_fetch_1((uint8_t *)addr, val);
  case 2:
    return __sync_or_and_fetch_2((uint16_t *)addr, val);
  case 4:
    return __sync_or_and_fetch_4((uint32_t *)addr, val);
  case 8:
    return __sync_or_and_fetch_8((uint64_t *)addr, val);
  }
  return 0;
}

#define and_and_fetch(addr, v)                                  \
  ((__typeof__(*(addr))) sync_and_and_fetch((addr),             \
                                            (unsigned long)(v), \
                                            sizeof(*(addr))))   \

static inline __attribute__((always_inline))
unsigned long sync_and_and_fetch(void *addr, unsigned long val, int len)
{
  switch (len) {
  case 1:
    return __sync_and_and_fetch_1((uint8_t *)addr, val);
  case 2:
    return __sync_and_and_fetch_2((uint16_t *)addr, val);
  case 4:
    return __sync_and_and_fetch_4((uint32_t *)addr, val);
  case 8:
    return __sync_and_and_fetch_8((uint64_t *)addr, val);
  }
  return 0;
}

#define exchange(addr, v)                               \
  ((__typeof__(*(addr))) _exchange((addr),              \
                                   (unsigned long)(v),  \
                                   sizeof(*(addr))))    \
  
static inline __attribute__((always_inline))
unsigned long _exchange(void *addr, unsigned long val, int len)
{
  switch (len) {
  case 1:
    {
      uint8_t old;
      do {
        old = load_shared(*(uint8_t *)addr);
      } while (!__sync_bool_compare_and_swap_1((uint8_t *)addr, old, val));
      return old;
    }
  case 2:
    {
      uint16_t old;
      do {
        old = load_shared(*(uint16_t *)addr);
      } while (!__sync_bool_compare_and_swap_2((uint16_t *)addr, old, val));
      return old;
    }
  case 4:
    {
      uint32_t old;
      do {
        old = load_shared(*(uint32_t *)addr);
      } while (!__sync_bool_compare_and_swap_4((uint32_t *)addr, old, val));
      return old;
    }
  case 8:
    {
      uint64_t old;
      do {
        old = load_shared(*(uint64_t *)addr);
      } while (!__sync_bool_compare_and_swap_8((uint64_t *)addr, old, val));
      return old;
    }
  }
  return 0;
}

#define compare_exchange(addr, old, v)                                  \
  ((__typeof__(*(addr))) _compare_exchange((addr),                      \
                                           (unsigned long)(old),        \
                                           (unsigned long)(v),          \
                                           sizeof(*(addr))))            \

static inline __attribute__((always_inline))
unsigned long _compare_exchange(void *addr, unsigned long old, unsigned long val, int len)
{
  switch (len) {
  case 1:
    return __sync_val_compare_and_swap_1((uint8_t *)addr, old, val);
  case 2:
    return __sync_val_compare_and_swap_2((uint16_t *)addr, old, val);
  case 4:
    return __sync_val_compare_and_swap_4((uint32_t *)addr, old, val);
  case 8:
    return __sync_val_compare_and_swap_8((uint64_t *)addr, old, val);
  }
  return 0;
}


#endif  /* _URCU_UTILS_H */
