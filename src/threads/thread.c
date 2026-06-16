#include "threads/thread.h"

#include <debug.h>
#include <random.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "devices/timer.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define THREAD_MAGIC 0xcd6abf4b

#define FP_SHIFT 14
#define FP_FACTOR (1 << FP_SHIFT)

#define INT_TO_FP(n) ((n) * FP_FACTOR)
#define FP_TO_INT_ZERO(x) ((x) / FP_FACTOR)
#define FP_TO_INT_NEAREST(x) ((x) >= 0 ? ((x) + FP_FACTOR / 2) / FP_FACTOR \
                                      : ((x) - FP_FACTOR / 2) / FP_FACTOR)
#define FP_ADD(x, y) ((x) + (y))
#define FP_SUB(x, y) ((x) - (y))
#define FP_ADD_INT(x, n) ((x) + (n) * FP_FACTOR)
#define FP_SUB_INT(x, n) ((x) - (n) * FP_FACTOR)
#define FP_MUL(x, y) ((int64_t)(x) * (y) / FP_FACTOR)
#define FP_MUL_INT(x, n) ((x) * (n))
#define FP_DIV(x, y) ((int64_t)(x) * FP_FACTOR / (y))
#define FP_DIV_INT(x, n) ((x) / (n))

#define DONATION_DEPTH 32

static struct list ready_list;
static struct list all_list;

static struct thread *idle_thread;
static struct thread *initial_thread;

static struct lock tid_lock;

struct kernel_thread_frame {
  void *eip;
  thread_func *function;
  void *aux;
};

static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

#define TIME_SLICE 4
static unsigned thread_ticks;

bool thread_mlfqs;
static int load_avg;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *running_thread(void);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static bool is_thread(struct thread *) UNUSED;
static void *alloc_frame(struct thread *, size_t size);
static void schedule(void);
void thread_schedule_tail(struct thread *prev);
static tid_t allocate_tid(void);

static int mlfqs_ready_threads(void);
static void mlfqs_increment_recent_cpu(void);
static void mlfqs_update_load_avg(void);
static void mlfqs_update_recent_cpu_func(struct thread *t, void *aux UNUSED);
static void mlfqs_update_priority_func(struct thread *t, void *aux UNUSED);
static void mlfqs_update_priority(struct thread *t);
static void maybe_yield_to_higher_priority(void);

bool thread_priority_greater(const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux UNUSED) {
  const struct thread *ta = list_entry(a, struct thread, elem);
  const struct thread *tb = list_entry(b, struct thread, elem);
  return ta->priority > tb->priority;
}

static void maybe_yield_to_higher_priority(void) {
  if (!list_empty(&ready_list)) {
    struct thread *t = list_entry(list_front(&ready_list),
                                  struct thread, elem);
    if (t->priority > thread_current()->priority)
      thread_yield();
  }
}

void
thread_donate_priority(void) {
  struct thread *cur = thread_current();
  struct lock *lock = cur->waiting_lock;
  int depth = 0;

  while (lock != NULL && lock->holder != NULL && depth < DONATION_DEPTH) {
    struct thread *holder = lock->holder;

    if (holder->priority < cur->priority)
      holder->priority = cur->priority;

    cur = holder;
    lock = cur->waiting_lock;
    depth++;
  }
}

void thread_update_priority(struct thread *t) {
  struct list_elem *e;

  if (thread_mlfqs)
    return;

  t->priority = t->base_priority;

  for (e = list_begin(&t->locks);
       e != list_end(&t->locks);
       e = list_next(e)) {
    struct lock *lock = list_entry(e, struct lock, elem);

    if (!list_empty(&lock->semaphore.waiters)) {
      list_sort(&lock->semaphore.waiters,
                thread_priority_greater, NULL);

      struct thread *waiter =
          list_entry(list_front(&lock->semaphore.waiters),
                     struct thread, elem);

      if (waiter->priority > t->priority)
        t->priority = waiter->priority;
    }
  }
}

void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&ready_list);
  list_init(&all_list);
  load_avg = 0;

  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
}

void thread_start(void) {
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  intr_enable();

  sema_down(&idle_started);
}

void thread_tick(void) {
  struct thread *t = thread_current();

  if (t == idle_thread) idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (thread_mlfqs) {
    int64_t now = timer_ticks();

    mlfqs_increment_recent_cpu();

    if (now % TIMER_FREQ == 0) {
      mlfqs_update_load_avg();
      thread_foreach(mlfqs_update_recent_cpu_func, NULL);
    }

    if (now % TIME_SLICE == 0) {
      thread_foreach(mlfqs_update_priority_func, NULL);
      list_sort(&ready_list, thread_priority_greater, NULL);
    }
  }

  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return();
}

void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
         idle_ticks, kernel_ticks, user_ticks);
}

tid_t thread_create(const char *name, int priority, thread_func *function,
                    void *aux) {
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT(function != NULL);

  t = palloc_get_page(PAL_ZERO);
  if (t == NULL) return TID_ERROR;

  init_thread(t, name, priority);

  if (thread_mlfqs && strcmp(name, "idle") != 0) {
    t->nice = thread_current()->nice;
    t->recent_cpu = thread_current()->recent_cpu;
    mlfqs_update_priority(t);
  }

  tid = t->tid = allocate_tid();

  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  thread_unblock(t);

  if (t->priority > thread_current()->priority)
    thread_yield();

  return tid;
}

void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

void thread_unblock(struct thread *t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  list_insert_ordered(&ready_list, &t->elem,
                      thread_priority_greater, NULL);
  t->status = THREAD_READY;
  intr_set_level(old_level);
}

const char *thread_name(void) { return thread_current()->name; }

struct thread *thread_current(void) {
  struct thread *t = running_thread();

  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

tid_t thread_tid(void) { return thread_current()->tid; }

void thread_exit(void) {
  ASSERT(!intr_context());

#ifdef USERPROG
  process_exit();
#endif

  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

void thread_yield(void) {
  struct thread *cur = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  if (cur != idle_thread)
    list_insert_ordered(&ready_list, &cur->elem,
                        thread_priority_greater, NULL);
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old_level);
}

void thread_foreach(thread_action_func *func, void *aux) {
  struct list_elem *e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

void thread_set_priority(int new_priority) {
  if (thread_mlfqs)
    return;

  enum intr_level old_level = intr_disable();

  struct thread *cur = thread_current();
  cur->base_priority = new_priority;
  thread_update_priority(cur);

  intr_set_level(old_level);

  maybe_yield_to_higher_priority();
}

int thread_get_priority(void) { return thread_current()->priority; }

void thread_set_nice(int nice) {
  enum intr_level old_level = intr_disable();

  thread_current()->nice = nice;
  mlfqs_update_priority(thread_current());

  intr_set_level(old_level);

  maybe_yield_to_higher_priority();
}

int thread_get_nice(void) {
  enum intr_level old_level = intr_disable();
  int nice = thread_current()->nice;
  intr_set_level(old_level);
  return nice;
}

int thread_get_load_avg(void) {
  enum intr_level old_level = intr_disable();
  int result = FP_TO_INT_NEAREST(FP_MUL_INT(load_avg, 100));
  intr_set_level(old_level);
  return result;
}

int thread_get_recent_cpu(void) {
  enum intr_level old_level = intr_disable();
  int result =
      FP_TO_INT_NEAREST(FP_MUL_INT(thread_current()->recent_cpu, 100));
  intr_set_level(old_level);
  return result;
}

static int mlfqs_ready_threads(void) {
  int ready_threads = list_size(&ready_list);

  if (thread_current() != idle_thread)
    ready_threads++;

  return ready_threads;
}

static void mlfqs_increment_recent_cpu(void) {
  struct thread *cur = thread_current();

  if (cur != idle_thread)
    cur->recent_cpu = FP_ADD_INT(cur->recent_cpu, 1);
}

static void mlfqs_update_load_avg(void) {
  int ready_threads = mlfqs_ready_threads();

  load_avg = FP_ADD(FP_MUL(FP_DIV_INT(INT_TO_FP(59), 60), load_avg),
                    FP_MUL_INT(FP_DIV_INT(INT_TO_FP(1), 60),
                               ready_threads));
}

static void mlfqs_update_recent_cpu_func(struct thread *t,
                                         void *aux UNUSED) {
  if (t == idle_thread)
    return;

  int coef = FP_DIV(FP_MUL_INT(load_avg, 2),
                    FP_ADD_INT(FP_MUL_INT(load_avg, 2), 1));

  t->recent_cpu = FP_ADD_INT(FP_MUL(coef, t->recent_cpu), t->nice);
}

static void mlfqs_update_priority_func(struct thread *t,
                                       void *aux UNUSED) {
  mlfqs_update_priority(t);
}

static void mlfqs_update_priority(struct thread *t) {
  if (t == idle_thread)
    return;

  int new_priority =
      PRI_MAX
      - FP_TO_INT_ZERO(FP_DIV_INT(t->recent_cpu, 4))
      - t->nice * 2;

  if (new_priority > PRI_MAX)
    new_priority = PRI_MAX;
  if (new_priority < PRI_MIN)
    new_priority = PRI_MIN;

  t->priority = new_priority;
}

static void idle(void *idle_started_ UNUSED) {
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;) {
    intr_disable();
    thread_block();

    asm volatile("sti; hlt" : : : "memory");
  }
}

static void kernel_thread(thread_func *function, void *aux) {
  ASSERT(function != NULL);

  intr_enable();
  function(aux);
  thread_exit();
}

struct thread *running_thread(void) {
  uint32_t *esp;

  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

static bool is_thread(struct thread *t) {
  return t != NULL && t->magic == THREAD_MAGIC;
}

static void init_thread(struct thread *t, const char *name, int priority) {
  enum intr_level old_level;

  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy(t->name, name, sizeof t->name);
  t->stack = (uint8_t *)t + PGSIZE;

  t->priority = priority;
  t->base_priority = priority;
  t->wake_tick = 0;
  t->nice = 0;
  t->recent_cpu = 0;
  t->waiting_lock = NULL;
  list_init(&t->locks);

#ifdef USERPROG
  t->pagedir = NULL;
#endif

  t->magic = THREAD_MAGIC;

  old_level = intr_disable();
  list_push_back(&all_list, &t->allelem);
  intr_set_level(old_level);
}

static void *alloc_frame(struct thread *t, size_t size) {
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

static struct thread *next_thread_to_run(void) {
  if (list_empty(&ready_list))
    return idle_thread;
  else
    return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

void thread_schedule_tail(struct thread *prev) {
  struct thread *cur = running_thread();

  ASSERT(intr_get_level() == INTR_OFF);

  cur->status = THREAD_RUNNING;

  thread_ticks = 0;

#ifdef USERPROG
  process_activate();
#endif

  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) {
    ASSERT(prev != cur);
    palloc_free_page(prev);
  }
}

static void schedule(void) {
  struct thread *cur = running_thread();
  struct thread *next = next_thread_to_run();
  struct thread *prev = NULL;

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(cur->status != THREAD_RUNNING);
  ASSERT(is_thread(next));

  if (cur != next)
    prev = switch_threads(cur, next);
  thread_schedule_tail(prev);
}

static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

uint32_t thread_stack_ofs = offsetof(struct thread, stack);