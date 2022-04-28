#ifndef _URCU_H
#define _URCU_H

#include "utils.h"

#define RCU_WAIT_ATTEMPTS 1000
#define RCU_QS_ACTIVE_ATTEMPTS 100
#define RCU_GP_CTR_PHASE (1UL << (sizeof(unsigned long) << 2)) /* 2^32 */
#define RCU_GP_CTR_NEST_MASK (RCU_GP_CTR_PHASE - 1)

enum rcu_wait_state {
  RCU_WAIT_WAITING = 0,
  RCU_WAIT_WAKEUP = (1 << 0),
  RCU_WAIT_RUNNING = (1 << 1),
  RCU_WAIT_TEARDOWN = (1 << 2),
};

enum rcu_reader_state {
  RCU_READER_ACTIVE_CURRENT,
  RCU_READER_ACTIVE_OLD,
  RCU_READER_INACTIVE,
};

/* --------------- structs --------------- */

typedef struct wait_node {
  struct wait_node *next;
  int32_t state;
} wait_node;

typedef struct wait_queue {
  wait_node *head;
} wait_queue;

typedef struct rcu_gp {
  /* 
   * global grace period counter,
   * written to only by writer with mutex taken,
   * read by writer and readers.
   */
  unsigned long ctr;
  int32_t futex;
} rcu_gp;

struct reader_list;

typedef struct rcu_reader {
  /* data used by reader and synchronize_rcu() */
  unsigned long ctr;
  char need_mb;
  pthread_t tid;
  /* reader registered flag, for internal checks */
  unsigned int registered:1;
  struct reader_list *list;
} rcu_reader;

typedef struct reader_list {
  struct reader_list *next, *prev;
  rcu_reader *node;
} reader_list;

/* --------------- functions --------------- */
void rcu_read_lock(void);

void rcu_read_unlock(void);

int rcu_read_ongoing(void);

void synchronize_rcu(void);

void rcu_register_thread(void);

void rcu_unregister_thread(void);

/* use p + 0 to get rid of ther const-ness */
#define rcu_dereference(p) __extension__ ({             \
      __typeof__(p + 0) ____p1;                         \
      __atomic_load(&(p), &____p1, __ATOMIC_CONSUME);   \
      (____p1);                                         \
    })                                                  \
 
#define rcu_assgin_pointer(p, v)                        \
  do {                                                  \
    __typeof__(p) ____pv = (v);                         \
    if (!__builtin_constant_p(v) || ((v) != NULL))      \
      smp_mb();                                         \
    store_shared(p, ____pv);                            \
  } while (0)                                           \

/* --------------- tls --------------- */

typedef struct rcu_tls {
  pthread_key_t key;
  pthread_mutex_t init_mutex;
  int init_done;
} rcu_tls;

rcu_reader *tls_access_reader(void);

#endif  /* _URCU_H */

