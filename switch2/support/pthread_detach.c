// libounce's pthread_detach returns ENOSYS ("Function not implemented"), which causes
// libstdc++'s std::thread::detach() to throw std::system_error and abort the process.
// Treating detach as a no-op returning 0 lets detached worker threads start normally.
#include <pthread.h>

int __wrap_pthread_detach(pthread_t thread) {
  (void)thread;
  return 0;
}
