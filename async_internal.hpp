#pragma once

#include <chrono>
#include <thread>

/// @cond INTERNAL_DOCS

// generic byte type used throughout
typedef unsigned char byte;

/****************************
 * Macros for repeated idioms
 */

#ifndef mpi_assert
#define mpi_assert(X) assert(X == MPI_SUCCESS);
#endif

#define wait_exp(cycle, t0, tmax)      \
  {                                    \
    for (auto dt = t0; cycle();        \
         dt = std::min(2 * dt, tmax))  \
      std::this_thread::sleep_for(dt); \
  }

/************************************
 * Task message and RMA buffer config
 */

// default max size for the packed async runner argument buffer (bytes)
#ifndef ASYNC_ARGS_SIZE
#define ASYNC_ARGS_SIZE 512
#endif

// inferred async task message size
#define ASYNC_MSG_SIZE sizeof(task_msg)

// length of the async message buffer
#ifndef ASYNC_MSG_BUFFLEN
#define ASYNC_MSG_BUFFLEN 2048
#endif

/************************************
 * Internal queue and message structs
 */

/**
 * Async task messages
 *
 * Used for communicating task data to the target rank
 */
struct task_msg
{
  int origin;
  bool notify;
  handle_t handle;
  fptr task_func;
  byte task_runner_args[ASYNC_ARGS_SIZE];

  task_msg() {}

  task_msg(int o, fptr tf, byte *ta, size_t sz) :
    origin(o), task_func(tf), notify(false)
  {
    assert(sz <= ASYNC_ARGS_SIZE);
    memcpy(task_runner_args, ta, sz);
  }

  void set_notify(handle_t h)
  {
    notify = true;
    handle = h;
  }
} __attribute__ ((packed));

/**
 * Locally enqueued (outgoing) tasks
 *
 * Internal representation of outgoing async tasks (not used for messaging -
 * the \c task_msg type is used the latter)
 */
struct task
{
  int origin, target;
  bool depends, notify;
  handle_t handle;
  std::vector<handle_t> after;
  fptr task_func;
  byte task_runner_args[ASYNC_ARGS_SIZE];
  size_t args_sz;

  task() {}

  task(int o, int t, fptr tf, byte *ta, size_t sz) :
    origin(o), target(t), task_func(tf), args_sz(sz), depends(false), notify(false)
  {
    assert(sz <= ASYNC_ARGS_SIZE);
    memcpy(task_runner_args, ta, sz);
  }

  void set_depends(handle_t a)
  {
    depends = true;
    after.push_back(a);
  }

  void set_notify(handle_t h)
  {
    notify = true;
    handle = h;
  }

  task_msg *to_msg()
  {
    task_msg *m = new task_msg(origin, task_func, task_runner_args, args_sz);
    if (notify)
      m->set_notify(handle);
    return m;
  }
};

/**
 * Locally enqueued (outgoing) completion notifications
 *
 * Internal representation of outgoing task completion notifications (not used
 * for messaging - only the \c handle_t is used for the latter)
 */
struct notify
{
  int target;
  handle_t handle;

  notify(int t, handle_t h) : target(t), handle(h) {}
};

/// @endcond
