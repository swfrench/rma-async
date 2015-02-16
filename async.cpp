#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <thread>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <mpi.h>

#include "rma-buffer/rma_buff.hpp"

#include "async.hpp"

typedef unsigned char byte;

// do concurrent (across progress threads) RMA calls for updating callback
// counters need to be protected with a mutex
#define REQUIRE_CB_MUTEX

// default max size for the packed async runner argument buffer
#define ASYNC_ARGS_SIZE 512

// task representation for messaging and internal queues
struct task
{
  int origin, target;
  fptr task_func;
  byte task_runner_args[ASYNC_ARGS_SIZE];
  task() {}
  task(int o, int t, fptr tf, byte *ta, size_t sz) :
    origin(o), target(t), task_func(tf)
  {
    assert(sz <= ASYNC_ARGS_SIZE);
    memcpy(task_runner_args, ta, sz);
  }
} __attribute__ ((packed));

// inferred async task message size
#define ASYNC_MSG_SIZE sizeof(struct task)

// length of the async message buffer
#define ASYNC_MSG_BUFFLEN 2048

#ifndef mpi_assert
#define mpi_assert(X) assert(X == MPI_SUCCESS);
#endif

// message buffer object: handles incoming / outgoing task messages; managed
// by the mover thread
static rma_buff<task> *task_buff;

// progress threads: message mover and task executor
static std::thread *th_mover;
static std::thread *th_executor;

// exit flag for progress threads
static std::atomic<bool> done(false);

// mutual exclusion for task queues and (maybe) local updates to cb_count
static std::mutex task_queue_mtx;
static std::mutex outgoing_task_queue_mtx;
#ifdef REQUIRE_CB_MUTEX
static std::mutex cb_mutex;
#endif

// task queues
static std::list<task *> task_queue;
static std::list<task *> outgoing_task_queue;

// general mpi
static int my_rank;
static MPI_Comm my_comm;

// callback counter: tracks the number of outstanding tasks
static int *cb_count;
static MPI_Win cb_win;

/*
 * Decrement the callback counter on the specified rank
 */
static void cb_dec(int target)
{
  int dec = -1;
#ifdef REQUIRE_CB_MUTEX
  if (target == my_rank)
    cb_mutex.lock();
#endif
  mpi_assert(MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target, 0, cb_win));
  mpi_assert(MPI_Accumulate(&dec,
                            1, MPI_INT, target, 0,
                            1, MPI_INT, MPI_SUM, cb_win));
  mpi_assert(MPI_Win_unlock(target, cb_win));
#ifdef REQUIRE_CB_MUTEX
  if (target == my_rank)
    cb_mutex.unlock();
#endif
}

/*
 * Increment the callback counter on the specified rank
 */
static void cb_inc(int target)
{
  int inc = 1;
#ifdef REQUIRE_CB_MUTEX
  if (target == my_rank)
    cb_mutex.lock();
#endif
  mpi_assert(MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target, 0, cb_win));
  mpi_assert(MPI_Accumulate(&inc,
                            1, MPI_INT, target, 0,
                            1, MPI_INT, MPI_SUM, cb_win));
  mpi_assert(MPI_Win_unlock(target, cb_win));
#ifdef REQUIRE_CB_MUTEX
  if (target == my_rank)
    cb_mutex.unlock();
#endif
}

/*
 * Get the callback counter on the specified rank
 */
static int cb_get(int target)
{
  int val;
#ifdef REQUIRE_CB_MUTEX
  if (target == my_rank)
    cb_mutex.lock();
#endif
  mpi_assert(MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target, 0, cb_win));
  mpi_assert(MPI_Get(&val,
                     1, MPI_INT, target, 0,
                     1, MPI_INT, cb_win));
  mpi_assert(MPI_Win_unlock(target, cb_win));
#ifdef REQUIRE_CB_MUTEX
  if (target == my_rank)
    cb_mutex.unlock();
#endif
  return val;
}

/*
 * Action performed by the mover progress thread: moves async tasks in and out
 * of the rma_buff object to / from their respective queues.
 */
static void mover()
{
  task task_msg_in[ASYNC_MSG_BUFFLEN];
  while (! done) {
    int nmsg;
    // service incoming tasks (enqueue them for execution)
    if ((nmsg = task_buff->get(task_msg_in, 1)) > 0) {
      for (int i = 0; i < nmsg; i++) {
        task *t = new task();
        memcpy(t, (void *)&task_msg_in[i], ASYNC_MSG_SIZE);
        task_queue_mtx.lock();
        task_queue.push_back((task *) t);
        task_queue_mtx.unlock();
      }
    }

    // place outgoing tasks on their respective targets
    task *msg = NULL;
    outgoing_task_queue_mtx.lock();
    if (! outgoing_task_queue.empty()) {
      msg = outgoing_task_queue.front();
      outgoing_task_queue.pop_front();
    }
    outgoing_task_queue_mtx.unlock();
    if (msg != NULL) {
      // try to place task on the target (non-blocking)
      if (task_buff->put(msg->target, msg)) {
        // success: clean up
        delete msg;
      } else {
        // failure: the target buffer must be full; put the message back in
        // the queue and retry later
        outgoing_task_queue_mtx.lock();
        outgoing_task_queue.push_back(msg);
        outgoing_task_queue_mtx.unlock();
      }
    }

    // possibly sleep if there was no work to do
    if (nmsg == 0 && msg == NULL)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
} /* static void mover() */

/*
 * Action performed by the executor thread: remove tasks from the queue and run
 * them (remembering to decrement the origin callback counter thereafter)
 */
static void executor()
{
  while (! done) {
    task *msg = NULL;
    task_queue_mtx.lock();
    if (! task_queue.empty()) {
      msg = task_queue.front();
      task_queue.pop_front();
    }
    task_queue_mtx.unlock();
    if (msg != NULL) {
      msg->task_func(msg->task_runner_args);
      cb_dec(msg->origin);
      delete msg;
    } else
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
} /* static void executor() */

/**
 * Route a task to the correct queue for exection on the target
 *
 * This routine will fast-path to our own local execution queue if we are in
 * fact the target. While it is not a user-facing function per se, it is called
 * by the \c async() task invocation launch functions (and thus cannot have
 * static linkage).
 *
 * \param target target rank for task execution
 * \param rp pointer to the appropriate runner function
 * \param tp pointer to the packed task data (function pointer and args)
 * \param sz size (bytes) of the packed task data
 */
void _enqueue(int target, fptr rp, void *tp, size_t sz)
{
  task *t = new task(my_rank, target, rp, (byte *)tp, sz);
  if (target == my_rank) {
    task_queue_mtx.lock();
    task_queue.push_back(t);
    task_queue_mtx.unlock();
  } else {
    outgoing_task_queue_mtx.lock();
    outgoing_task_queue.push_back(t);
    outgoing_task_queue_mtx.unlock();
  }
  cb_inc(my_rank);
}

/**
 * Enable asynchronous task execution among ranks on the supplied communicator
 *
 * This call is collective.
 *
 * \param comm MPI communicator over participating ranks
 */
void async_enable(MPI_Comm comm)
{
  int mpi_init, mpi_thread;

  // sanity check on mpi threading support
  mpi_assert(MPI_Initialized(&mpi_init));
  assert(mpi_init);
  mpi_assert(MPI_Query_thread(&mpi_thread));
  assert(mpi_thread == MPI_THREAD_MULTIPLE);

  // set communicator and rank
  my_comm = comm;
  mpi_assert(MPI_Comm_rank(my_comm, &my_rank));

  // setup callback counter
  mpi_assert(MPI_Alloc_mem(sizeof(int), MPI_INFO_NULL, &cb_count));
  *cb_count = 0;
  mpi_assert(MPI_Win_create(cb_count, sizeof(int), sizeof(int),
                            MPI_INFO_NULL,
                            comm, &cb_win));

  // setup the task buffer
  task_buff = new rma_buff<task>(1, ASYNC_MSG_BUFFLEN, comm);

  // setup the progress threads
  th_mover = new std::thread(mover);
  th_executor = new std::thread(executor);

  // synchronize and return
  mpi_assert(MPI_Barrier(my_comm));
}

/**
 * Stop invocation of asynchronous tasks and block until all outstanding
 * tasks have been executed
 *
 * This call is collective.
 */
void async_disable()
{
  // wait for all outstanding tasks to complete
  while (cb_get(my_rank))
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // synchronize
  mpi_assert(MPI_Barrier(my_comm));

  // no more work to do; shut down the progress threads
  done = true;
  th_mover->join();
  th_executor->join();

  // clean up
  mpi_assert(MPI_Win_free(&cb_win));
  mpi_assert(MPI_Free_mem(cb_count));
  delete th_mover;
  delete th_executor;
  delete task_buff;
}
