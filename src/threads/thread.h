#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

struct lock;

/** States in a thread's life cycle. */
enum thread_status {
  THREAD_RUNNING,
  THREAD_READY,
  THREAD_BLOCKED,
  THREAD_DYING
};

typedef int tid_t;
#define TID_ERROR ((tid_t) - 1)

/** Thread priorities. */
#define PRI_MIN 0
#define PRI_DEFAULT 31
#define PRI_MAX 63

struct thread {
  /* Owned by thread.c. */
  tid_t tid;
  enum thread_status status;
  char name[16];
  uint8_t *stack;

  int priority;
  int base_priority;

  int64_t wake_tick;

  int nice;
  int recent_cpu;

  struct lock *waiting_lock;
  struct list locks;

  struct list_elem allelem;

  /* Shared between thread.c and synch.c. */
  struct list_elem elem;

#ifdef USERPROG
  uint32_t *pagedir;
#endif

  unsigned magic;
};

extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

bool thread_priority_greater(const struct list_elem *,
                             const struct list_elem *,
                             void *);

void thread_donate_priority(void);
void thread_update_priority(struct thread *);

typedef void thread_action_func(struct thread *t, void *aux);
void thread_foreach(thread_action_func *, void *);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

#endif