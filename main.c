#include "urcu.h"
#include <stdio.h>

long long val = 1;

void *reader_function(void *arg)
{
  rcu_register_thread();
  unsigned long sum = 0;
  for (size_t i = 0; i < 1000; ++i) {
    rcu_read_lock();
    sum += rcu_dereference(val);
    poll(NULL, 0, 10);
    rcu_read_unlock();
  }
  printf("%lu: %lu\n", pthread_self(), sum);
  rcu_unregister_thread();
  return NULL;
}

void *writer_function(void *arg)
{
  for (size_t i = 0; i < 1000; ++i) {
    rcu_assgin_pointer(val, i);
    synchronize_rcu();
  }
  return NULL;
}

int main(int argc, char *argv[])
{
  size_t reader_num = 10;
  size_t writer_num = 3;
  pthread_t reader_threads[reader_num];
  pthread_t writer_threads[writer_num];
  for (size_t i = 0; i < reader_num; ++i) {
    pthread_create(&reader_threads[i], NULL, reader_function, NULL);
    poll(NULL, 0, 10);
  }
  for (size_t i = 0; i < writer_num; ++i) {
    pthread_create(&writer_threads[i], NULL, writer_function, NULL);
    poll(NULL, 0, 10);
  }
  for (size_t i = 0; i < reader_num; ++i) {
    pthread_join(reader_threads[i], NULL);
  }
  for (size_t i = 0; i < writer_num; ++i) {
    pthread_join(writer_threads[i], NULL);
  }
  return 0;
}
