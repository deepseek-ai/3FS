#include "common/app/Thread.h"

#include <sys/signal.h>

namespace hf3fs {

/**
 * Block signals SIGINT and SIGTERM for the calling pthread.
 *
 * Note: Blocked signals will be inherited by child threads.
 *
 * Note: This is important to make, because Linux can direct a SIGINT to any thread that has a
 * handler registered; so we need this to make sure that only the main thread (and not the worker
 * threads) receive a SIGINT, e.g. if a user presses CTRL+C.
 */
bool Thread::blockInterruptSignals() {
  sigset_t signalMask;  // signals to block

  sigemptyset(&signalMask);

  sigaddset(&signalMask, SIGINT);
  sigaddset(&signalMask, SIGTERM);

  int sigmaskRes = pthread_sigmask(SIG_BLOCK, &signalMask, nullptr);

  return (sigmaskRes == 0);
}

/**
 * Unblock the signals (e.g. SIGINT) that were blocked with blockInterruptSignals().
 */
bool Thread::unblockInterruptSignals() {
  sigset_t signalMask;  // signals to unblock

  sigemptyset(&signalMask);

  sigaddset(&signalMask, SIGINT);
  sigaddset(&signalMask, SIGTERM);

  int sigmaskRes = pthread_sigmask(SIG_UNBLOCK, &signalMask, nullptr);

  return (sigmaskRes == 0);
}

}  // namespace hf3fs
