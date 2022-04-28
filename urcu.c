#include "urcu.h"

/* --------------- global variables --------------- */

/* ensures mutual exclusion between threads calling synchronize_rcu() */
static pthread_mutex_t rcu_gp_lock = PTHREAD_MUTEX_INITIALIZER;

/* ensures mutual exclusion between threads (un)registering themselves to/from the registry */
static pthread_mutex_t rcu_registry_lock = PTHREAD_MUTEX_INITIALIZER;

struct rcu_gp gp = { .ctr = 1 };

reader_list registry_reader_list = { .next = &registry_reader_list,
  .prev = &registry_reader_list };

wait_queue gp_waiters = { NULL };

pthread_mutex_t gp_waiters_lock = PTHREAD_MUTEX_INITIALIZER;

/* --------------- grace period --------------- */

/* synchronize_rcu() waiting single thread */
static void wait_gp(void)
{
  membarrier_master();
  pthread_mutex_unlock(&rcu_registry_lock);
  if (load_shared(gp.futex) != -1) {
    pthread_mutex_lock(&rcu_registry_lock);
    return;
  }
  while (futex(&gp.futex, FUTEX_WAIT, -1, NULL, NULL, 0)) {
    /* EWOULDBLOCK: the value pointed to by uaddr was not equal to the 
     * expected value val at the time of the call
     */
    if (errno == EWOULDBLOCK) {
      pthread_mutex_lock(&rcu_registry_lock);
      return;
    }
  }
}

static inline void wake_up_gp(rcu_gp *gp)
{
  if (unlikely(load_shared(gp->futex) == -1)) {
    store_shared(gp->futex, 0);
    futex(&gp->futex, FUTEX_WAKE, 1, NULL, NULL, 0);
  }
}

/* --------------- wait queue --------------- */

static inline int wait_queue_push(wait_queue *queue, wait_node *node)
{
  wait_node *old_head = exchange(&queue->head, node);
  store_shared(node->next, old_head);
  return !!old_head;
}

static inline void wait_queue_move(wait_queue *src, wait_queue *dst)
{
  dst->head = exchange(&src->head, NULL);
}

static inline void adaptative_busy_wait(wait_node *node)
{
  unsigned int i;
  smp_mb();
  for (i = 0; i < RCU_WAIT_ATTEMPTS; ++i) {
    if (load_shared(node->state) != RCU_WAIT_WAITING)
      goto skip_futex;
    PAUSE();
  }
  while (futex(&node->state, FUTEX_WAIT, RCU_WAIT_WAITING, NULL, NULL, 0)) {
    if (errno == EWOULDBLOCK) goto skip_futex;
  }
 skip_futex:
  or_and_fetch(&node->state, RCU_WAIT_RUNNING);
  for (i = 0; i < RCU_WAIT_ATTEMPTS; ++i) {
    if (load_shared(node->state) & RCU_WAIT_TEARDOWN)
      break;
    PAUSE();
  }
  while (!(load_shared(node->state) & RCU_WAIT_TEARDOWN))
    poll(NULL, 0, 10);
}

static inline void adaptative_wake_up(wait_node *node)
{
  smp_mb();
  store_shared(node->state, RCU_WAIT_WAKEUP);
  if (!(load_shared(node->state) & RCU_WAIT_RUNNING)) {
    futex(&node->state, FUTEX_WAKE, 1, NULL, NULL, 0);
  }
  or_and_fetch(&node->state, RCU_WAIT_TEARDOWN);
}

/* --------------- reader --------------- */

static inline enum rcu_reader_state reader_state(rcu_gp *gp, rcu_reader *reader)
{
  unsigned long v = load_shared(reader->ctr);
  /* low 32-bits all 0 */
  if (!(v & RCU_GP_CTR_NEST_MASK))
    return RCU_READER_INACTIVE;
  /* 33th bit equals to gp's */
  if (!((v ^ gp->ctr) & RCU_GP_CTR_PHASE))
    return RCU_READER_ACTIVE_CURRENT;
  return RCU_READER_ACTIVE_OLD;
}


static inline void reader_add(rcu_reader *reader, reader_list *list)
{
  reader_list *tmp = (reader_list *)calloc(1, sizeof(reader_list));
  tmp->node = reader;
  list->next->prev = tmp;
  tmp->next = list->next;
  tmp->prev = list;
  list->next = tmp;
  reader->list = tmp;
}

static inline void reader_del(rcu_reader *reader)
{
  if (reader->list == NULL) return;
  reader->list->next->prev = reader->list->prev;
  reader->list->prev->next = reader->list->next;
  free(reader->list);
  reader->list = NULL;
}

static inline void reader_move(reader_list *src, reader_list *dst)
{
  src->next->prev = src->prev;
  src->prev->next = src->next;
  dst->prev->next = src;
  src->next = dst->next;
  src->prev = dst;
  dst->next = src;
}

static inline void reader_splice(reader_list *src, reader_list *dst)
{
  src->next->prev = dst;
  src->prev->next = dst->next;
  dst->next->prev = src->prev;
  dst->next = src->next;
}

/* --------------- interfaces --------------- */

void rcu_read_lock(void)
{
  unsigned long tmp;
  barrier();
  tmp = tls_access_reader()->ctr;
  /* if reader.ctr low 32-bits is 0 */
  if (likely(!(tmp & RCU_GP_CTR_NEST_MASK))) {
    store_shared(tls_access_reader()->ctr, load_shared(gp.ctr));
    membarrier_slave();
  } else {
    store_shared(tls_access_reader()->ctr, tmp + 1);
  }
}

void rcu_read_unlock(void)
{
  unsigned long tmp;
  tmp = tls_access_reader()->ctr;
  /* if reader.ctr low 32-bits equals to 1 */
  if (likely((tmp & RCU_GP_CTR_NEST_MASK) == 1)) {
    membarrier_slave();
    store_shared(tls_access_reader()->ctr, tmp - 1);
    membarrier_slave();
    wake_up_gp(&gp);
  } else {
    store_shared(tls_access_reader()->ctr, tmp - 1);
  }
  barrier();
}

/* 
 * return whether within a read-side critical section 
 * (reader.ctr low 32-bits equals to 1)
 */
int rcu_read_ongoing(void)
{
  return tls_access_reader()->ctr & RCU_GP_CTR_NEST_MASK;
}

static void wait_for_readers(reader_list *input_readers, reader_list *cur_snap_readers,
                             reader_list *qs_readers)
{
  unsigned int wait_loops = 0;
  while (1) {
    wait_loops++;
    if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
      add_and_fetch(&gp.futex, -1);
      membarrier_master();
    }
    reader_list *it = input_readers;
    while (it != NULL) {
      if (it->node != NULL) {
        enum rcu_reader_state state = reader_state(&gp, it->node);
        switch (state) {
        case RCU_READER_ACTIVE_CURRENT:
          if (cur_snap_readers) {
            reader_move(it, cur_snap_readers);
            break;
          }
        case RCU_READER_INACTIVE:
          reader_move(it, qs_readers);
          break;
        case RCU_READER_ACTIVE_OLD:
          break;
        }
      }
      it = it->next;
      if (it == input_readers) break;
    }
    /* if empty */
    if (input_readers->next = input_readers) {
      if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
        membarrier_master();
        store_shared(gp.futex, 0);
      }
      break;
    } else {                    /* exists old active readers */
      if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
        wait_gp();
      } else {
        pthread_mutex_unlock(&rcu_registry_lock);
        PAUSE();
        pthread_mutex_lock(&rcu_registry_lock);
      }
    }
  }
}

void synchronize_rcu(void)
{
  /* current snapshot readers */
  reader_list cur_snap_readers = { .next = &cur_snap_readers, .prev = &cur_snap_readers };
  /* quiescent readers */
  reader_list qs_readers = { .next = &qs_readers, .prev = &qs_readers };
  wait_node wait = { .state = RCU_WAIT_WAITING };
  wait_queue waiters = { NULL };
  if (wait_queue_push(&gp_waiters, &wait) != 0) {
    /* not first in queue: will be awakened by another thread */
    adaptative_busy_wait(&wait);
    smp_mb();
    return;
  }
  wait.state = RCU_WAIT_RUNNING;
  pthread_mutex_lock(&rcu_gp_lock);
  wait_queue_move(&gp_waiters, &waiters);
  pthread_mutex_lock(&rcu_registry_lock);
  if (registry_reader_list.next == &registry_reader_list)
    goto out;
  membarrier_master();
  wait_for_readers(&registry_reader_list, &cur_snap_readers, &qs_readers);
  /* enforce compiler-order of load reader.ctr before store to gp.ctr */
  barrier();
  store_shared(gp.ctr, gp.ctr ^ RCU_GP_CTR_PHASE);
  /* enforce compiler-order of store to gp.ctr before load reader.ctr */
  barrier();
  wait_for_readers(&cur_snap_readers, NULL, &qs_readers);
  /* put quiescent readers back into reader */
  if (qs_readers.next != &qs_readers) {
    reader_splice(&qs_readers, &registry_reader_list);
  }
  membarrier_master();
 out:
  pthread_mutex_unlock(&rcu_registry_lock);
  pthread_mutex_unlock(&rcu_gp_lock);
  wait_node *it = waiters.head;
  while (it != NULL) {
    if (!(it->state & RCU_WAIT_RUNNING))
      adaptative_wake_up(it);
    it = it->next;
  }
}

void rcu_register_thread(void)
{
  tls_access_reader()->tid = pthread_self();
  pthread_mutex_lock(&rcu_registry_lock);
  tls_access_reader()->registered = 1;
  membarrier_register();
  reader_add(tls_access_reader(), &registry_reader_list);
  pthread_mutex_unlock(&rcu_registry_lock);
}

void rcu_unregister_thread(void)
{
  pthread_mutex_lock(&rcu_registry_lock);
  tls_access_reader()->registered = 0;
  reader_del(tls_access_reader());
  pthread_mutex_unlock(&rcu_registry_lock);
}

/* --------------- tls --------------- */

rcu_reader *tls_access_reader(void)
{
  static struct rcu_tls tls_reader = {
    .key = 0,
    .init_mutex = PTHREAD_MUTEX_INITIALIZER,
    .init_done = 0,
  };
  rcu_reader *ret;
  if (!tls_reader.init_done) {
    pthread_mutex_lock(&tls_reader.init_mutex);
    if (!tls_reader.init_done) {
      pthread_key_create(&tls_reader.key, free);
      smp_mb();
      tls_reader.init_done = 1;
    }
    pthread_mutex_unlock(&tls_reader.init_mutex);
  }
  smp_mb();
  ret = (rcu_reader *)pthread_getspecific(tls_reader.key);
  if (unlikely(ret == NULL)) {
    ret = (rcu_reader *)calloc(1, sizeof(rcu_reader));
    pthread_setspecific(tls_reader.key, ret);
  }
  return ret;
}
