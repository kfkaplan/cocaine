/* Threads, through Apple's own pthreads from the system root.
 *
 * Unlike the other tests this one is a normal dynamically linked Mac OS X
 * program, so it needs a system root to run. Four threads bump two shared
 * counters, one with an atomic instruction and one under a mutex; lost
 * updates would show as a short count. */
extern int pthread_create(void*, const void*, void* (*)(void*), void*);
extern int pthread_join(void*, void**);
extern int pthread_mutex_init(void*, const void*);
extern int pthread_mutex_lock(void*);
extern int pthread_mutex_unlock(void*);
extern int printf(const char*, ...);
extern void exit(int);

/* Library functions are called through pointers. A direct call would go
 * through a lazy-binding stub, and those need a helper from the C start-up
 * file, which isn't part of a Mac OS X installation (it comes with Xcode).
 * A pointer in initialised data is simply filled in by dyld at load time. */
__asm__(".globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n hlt\n");
static int (*volatile create)(void*, const void*, void* (*)(void*), void*) = pthread_create;
static int (*volatile join)(void*, void**) = pthread_join;
static int (*volatile mutex_init)(void*, const void*) = pthread_mutex_init;
static int (*volatile mutex_lock)(void*) = pthread_mutex_lock;
static int (*volatile mutex_unlock)(void*) = pthread_mutex_unlock;
static int (*volatile print)(const char*, ...) = printf;
static void (*volatile quit)(int) = exit;

#define THREADS 4
#define ROUNDS 30000

static volatile int atomic_count;
static int locked_count;
static char mutex[64]; /* room for a pthread_mutex_t */

static void* worker(void* arg) {
  for (int i = 0; i < ROUNDS; i++) {
    __sync_fetch_and_add(&atomic_count, 1);
    mutex_lock(mutex);
    locked_count++;
    mutex_unlock(mutex);
  }
  return arg;
}

int main(void) {
  void* thread[THREADS];
  long joined = 0;
  mutex_init(mutex, 0);
  for (long i = 0; i < THREADS; i++) create(&thread[i], 0, worker, (void*)(i + 1));
  for (int i = 0; i < THREADS; i++) {
    void* result;
    join(thread[i], &result);
    joined += (long)result;
  }
  print("atomic %d locked %d joined %ld\n", atomic_count, locked_count, joined);
  quit(0);
  return 0;
}
