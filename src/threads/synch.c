#include "threads/synch.h"

#include <stdio.h>
#include <string.h>

#include "threads/interrupt.h"
#include "threads/thread.h"

/** Initializes semaphore SEMA to VALUE. */
void sema_init(struct semaphore *sema, unsigned value) {
  ASSERT(sema != NULL);

  sema->value = value;
  list_init(&sema->waiters);
}

/** Down or "P" operation on a semaphore. */
void sema_down(struct semaphore *sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);
  ASSERT(!intr_context());

  old_level = intr_disable();
  while (sema->value == 0) {
    list_insert_ordered(&sema->waiters,
                        &thread_current()->elem,
                        thread_priority_greater,
                        NULL);
    thread_block();
  }
  sema->value--;
  intr_set_level(old_level);
}

/** Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0. */
bool sema_try_down(struct semaphore *sema) {
  enum intr_level old_level;
  bool success;

  ASSERT(sema != NULL);

  old_level = intr_disable();
  if (sema->value > 0) {
    sema->value--;
    success = true;
  } else {
    success = false;
  }
  intr_set_level(old_level);

  return success;
}

/** Up or "V" operation on a semaphore. */
void
sema_up(struct semaphore *sema) {
  enum intr_level old_level;
  struct thread *unblocked = NULL;
  bool need_yield = false;

  ASSERT(sema != NULL);

  old_level = intr_disable();

  if (!list_empty(&sema->waiters)) {
    list_sort(&sema->waiters, thread_priority_greater, NULL);
    unblocked = list_entry(list_pop_front(&sema->waiters),
                           struct thread, elem);
    thread_unblock(unblocked);

    if (unblocked->priority > thread_current()->priority)
      need_yield = true;
  }

  sema->value++;

  intr_set_level(old_level);

  if (need_yield && !intr_context())
    thread_yield();
  else if (need_yield && intr_context())
    intr_yield_on_return();
}

static void sema_test_helper(void *sema_);

/** Self-test for semaphores. */
void sema_self_test(void) {
  struct semaphore sema[2];
  int i;

  printf("Testing semaphores...");
  sema_init(&sema[0], 0);
  sema_init(&sema[1], 0);
  thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) {
    sema_up(&sema[0]);
    sema_down(&sema[1]);
  }
  printf("done.\n");
}

static void sema_test_helper(void *sema_) {
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) {
    sema_down(&sema[0]);
    sema_up(&sema[1]);
  }
}

/** Initializes LOCK. */
void lock_init(struct lock *lock) {
  ASSERT(lock != NULL);

  lock->holder = NULL;
  sema_init(&lock->semaphore, 1);
}

/** Acquires LOCK. */
void lock_acquire(struct lock *lock) {
  struct thread *cur = thread_current();

  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(!lock_held_by_current_thread(lock));

  if (!thread_mlfqs && lock->holder != NULL) {
    enum intr_level old_level = intr_disable();
    cur->waiting_lock = lock;
    thread_donate_priority();
    intr_set_level(old_level);
  }

  sema_down(&lock->semaphore);

  cur->waiting_lock = NULL;
  lock->holder = cur;
  list_push_back(&cur->locks, &lock->elem);
}

/** Tries to acquire LOCK. */
bool lock_try_acquire(struct lock *lock) {
  bool success;

  ASSERT(lock != NULL);
  ASSERT(!lock_held_by_current_thread(lock));

  success = sema_try_down(&lock->semaphore);
  if (success) {
    lock->holder = thread_current();
    list_push_back(&thread_current()->locks, &lock->elem);
  }

  return success;
}

/** Releases LOCK. */
void
lock_release(struct lock *lock) {
  ASSERT(lock != NULL);
  ASSERT(lock_held_by_current_thread(lock));

  list_remove(&lock->elem);
  lock->holder = NULL;

  if (!thread_mlfqs)
    thread_update_priority(thread_current());

  sema_up(&lock->semaphore);
}

bool lock_held_by_current_thread(const struct lock *lock) {
  ASSERT(lock != NULL);

  return lock->holder == thread_current();
}

/** One semaphore in a list. */
struct semaphore_elem {
  struct list_elem elem;
  struct semaphore semaphore;
};

static bool sema_priority_greater(const struct list_elem *a,
                                  const struct list_elem *b,
                                  void *aux UNUSED) {
  struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
  struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);

  if (list_empty(&sa->semaphore.waiters))
    return false;
  if (list_empty(&sb->semaphore.waiters))
    return true;

  struct thread *ta = list_entry(list_front(&sa->semaphore.waiters),
                                 struct thread, elem);
  struct thread *tb = list_entry(list_front(&sb->semaphore.waiters),
                                 struct thread, elem);

  return ta->priority > tb->priority;
}

/** Initializes condition variable COND. */
void cond_init(struct condition *cond) {
  ASSERT(cond != NULL);

  list_init(&cond->waiters);
}

/** Atomically releases LOCK and waits for COND to be signaled. */
void cond_wait(struct condition *cond, struct lock *lock) {
  struct semaphore_elem waiter;

  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  sema_init(&waiter.semaphore, 0);
  list_push_back(&cond->waiters, &waiter.elem);
  lock_release(lock);
  sema_down(&waiter.semaphore);
  lock_acquire(lock);
}

/** Signals one waiter on COND. */
void cond_signal(struct condition *cond, struct lock *lock UNUSED) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  if (!list_empty(&cond->waiters)) {
    list_sort(&cond->waiters, sema_priority_greater, NULL);
    sema_up(&list_entry(list_pop_front(&cond->waiters),
                        struct semaphore_elem, elem)->semaphore);
  }
}

/** Wakes up all waiters on COND. */
void cond_broadcast(struct condition *cond, struct lock *lock) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);

  while (!list_empty(&cond->waiters))
    cond_signal(cond, lock);
}