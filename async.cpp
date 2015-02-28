/**
 * \mainpage Asynchronous remote tasks over MPI-3 RMA
 *
 * This is a simple, proof-of-concept implementation of asynchronous remote
 * tasks (a la UPC++) on top of MPI-3 RMA.
 */

#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_set>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <mpi.h>

#include "rma-buffer/rma_buff.hpp"

#include "async.hpp"

#ifndef mpi_assert
#define mpi_assert(X) assert(X == MPI_SUCCESS);
#endif

// generic byte
typedef unsigned char byte;

// default max size for the packed async runner argument buffer
#ifndef ASYNC_ARGS_SIZE
#define ASYNC_ARGS_SIZE 512
#endif

/// @cond INTERNAL_DOCS

struct task
{
  int origin, target;
  bool depends, notify;
  handle_t handle, after;
  fptr task_func;
  byte task_runner_args[ASYNC_ARGS_SIZE];

  task() {}

  task(int o, int t, fptr tf, byte *ta, size_t sz) :
    origin(o), target(t), task_func(tf), depends(false), notify(false)
  {
    assert(sz <= ASYNC_ARGS_SIZE);
    memcpy(task_runner_args, ta, sz);
  }

  void set_depends(handle_t a)
  {
    depends = true;
    after = a;
  }

  void set_notify(handle_t h)
  {
    notify = true;
    handle = h;
  }
} __attribute__ ((packed));


// representation of outgoing task completion notification messages
struct notify
{
  int target;
  handle_t handle;

  notify(int t, handle_t h) : target(t), handle(h) {}
};

/// @endcond

// inferred async task message size
#define ASYNC_MSG_SIZE sizeof(struct task)

// length of the async message buffer
#ifndef ASYNC_MSG_BUFFLEN
#define ASYNC_MSG_BUFFLEN 2048
#endif


// task buffer object: handles incoming / outgoing task messages; managed by
// the mover thread
static rma_buff<task> *task_buff;

// handle buffer object: handles incoming / outgoing completion notifications;
// also managed by the mover thread
static rma_buff<handle_t> *notify_buff;

// progress threads: message mover and task executor
static std::thread *th_mover;
static std::thread *th_executor;

// exit flag for progress threads
static std::atomic<bool> done(false);

// counter for async handles
static std::atomic<handle_t> handle_source(0);

// mutual exclusion for task queues, updates to cb_count, and the
// completed_tasks set
static std::mutex task_queue_mtx;
static std::mutex outgoing_task_queue_mtx;
static std::mutex outgoing_notify_queue_mtx;
static std::mutex completed_tasks_mtx;
static std::mutex cb_mutex;

// task queues
static std::list<task *> task_queue;
static std::list<task *> outgoing_task_queue;
static std::list<notify *> outgoing_notify_queue;

// general mpi
static int my_rank;
static MPI_Comm my_comm;

// callback counter: tracks the number of outstanding tasks
static int *cb_count;
static MPI_Win cb_win;

// completed task handle set
static std::unordered_set<handle_t> completed_tasks;


/*
 * Decrement the callback counter on the specified rank
 */
static void cb_dec(int target)
{
  int dec = -1;
  if (target == my_rank)
    cb_mutex.lock();
  mpi_assert(MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target, 0, cb_win));
  mpi_assert(MPI_Accumulate(&dec,
                            1, MPI_INT, target, 0,
                            1, MPI_INT, MPI_SUM, cb_win));
  mpi_assert(MPI_Win_unlock(target, cb_win));
  if (target == my_rank)
    cb_mutex.unlock();
}

/*
 * Increment the callback counter on the specified rank
 */
static void cb_inc(int target)
{
  int inc = 1;
  if (target == my_rank)
    cb_mutex.lock();
  mpi_assert(MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target, 0, cb_win));
  mpi_assert(MPI_Accumulate(&inc,
                            1, MPI_INT, target, 0,
                            1, MPI_INT, MPI_SUM, cb_win));
  mpi_assert(MPI_Win_unlock(target, cb_win));
  if (target == my_rank)
    cb_mutex.unlock();
}

/*
 * Get the callback counter on the specified rank
 */
static int cb_get(int target)
{
  int val;
  if (target == my_rank)
    cb_mutex.lock();
  mpi_assert(MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target, 0, cb_win));
  mpi_assert(MPI_Get(&val,
                     1, MPI_INT, target, 0,
                     1, MPI_INT, cb_win));
  mpi_assert(MPI_Win_unlock(target, cb_win));
  if (target == my_rank)
    cb_mutex.unlock();
  return val;
}


/*
 * Action performed by the mover progress thread: moves async tasks in and out
 * of the rma_buff object to / from their respective queues.
 */
static void mover()
{
  task *task_msg_in = new task[ASYNC_MSG_BUFFLEN];
  handle_t *notify_msg_in = new handle_t[ASYNC_MSG_BUFFLEN];
  while (! done) {
    int nmsg;

    //
    // service incoming / outgoing tasks
    //

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
      bool success = false;
      if (! msg->depends) {
        success = task_buff->put(msg->target, msg);
      } else {
        int completed;
        completed_tasks_mtx.lock();
        completed = completed_tasks.count(msg->after);
        completed_tasks_mtx.unlock();
        if (completed)
          success = task_buff->put(msg->target, msg);
      }
      if (success) {
        // success: clean up
        delete msg;
      } else {
        // failure: the target buffer must be full or the dependency has not
        // yet executed; put the message back in the queue and retry later
        outgoing_task_queue_mtx.lock();
        outgoing_task_queue.push_back(msg);
        outgoing_task_queue_mtx.unlock();
      }
    }

    //
    // service incoming / outgoing completion notifications
    //

    // service incoming completion notifications
    if ((nmsg = notify_buff->get(notify_msg_in, 1)) > 0) {
      completed_tasks_mtx.lock();
      for (int i = 0; i < nmsg; i++)
        completed_tasks.insert(notify_msg_in[i]);
      completed_tasks_mtx.unlock();
    }

    // place outgoing notifications on their respective targets
    notify *n = NULL;
    outgoing_notify_queue_mtx.lock();
    if (! outgoing_notify_queue.empty()) {
      n = outgoing_notify_queue.front();
      outgoing_notify_queue.pop_front();
    }
    outgoing_notify_queue_mtx.unlock();
    if (n != NULL) {
      if (notify_buff->put(n->target, &n->handle)) {
        // success: clean up
        delete n;
      } else {
        // failure: the target buffer must be full; put the message back in the
        // queue and retry later
        outgoing_notify_queue_mtx.lock();
        outgoing_notify_queue.push_back(n);
        outgoing_notify_queue_mtx.unlock();
      }
    }
  }
  delete [] notify_msg_in;
  delete [] task_msg_in;
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
      if (msg->notify) {
        notify *n = new notify(msg->origin, msg->handle);
        outgoing_notify_queue_mtx.lock();
        outgoing_notify_queue.push_back(n);
        outgoing_notify_queue_mtx.unlock();
      }
      delete msg;
    } else
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
} /* static void executor() */


/**
 * Route a task to the correct queue for exection on the target: variant 1
 *
 * This routine will fast-path to our own local execution queue if we are in
 * fact the target. While it is not a user-facing function per se, it is called
 * by the \c async() task invocation launch functions (and thus cannot have
 * static linkage).
 *
 * In this particular variant, we include no logic for async dependency
 * enforcement (i.e. ordering either before or after other tasks).
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
 * Route a task to the correct queue for exection on the target: variant 2
 *
 * This routine will fast-path to our own local execution queue if we are in
 * fact the target. While it is not a user-facing function per se, it is called
 * by the \c async() task invocation launch functions (and thus cannot have
 * static linkage).
 *
 * In this particular variant, we include logic for async dependency
 * enforcement that allows subsequent tasks to be ordered after this one.
 *
 * \param target target rank for task execution
 * \param rp pointer to the appropriate runner function
 * \param tp pointer to the packed task data (function pointer and args)
 * \param sz size (bytes) of the packed task data
 * \param h pointer to which this task's handle will be written
 */
void _enqueue_handle(int target, fptr rp, void *tp, size_t sz, handle_t *h)
{
  *h = handle_source++;
  task *t = new task(my_rank, target, rp, (byte *)tp, sz);
  t->set_notify(*h);
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
 * Route a task to the correct queue for exection on the target: variant 3
 *
 * This routine will fast-path to our own local execution queue if we are in
 * fact the target. While it is not a user-facing function per se, it is called
 * by the \c async() task invocation launch functions (and thus cannot have
 * static linkage).
 *
 * In this particular variant, we include logic for async dependency
 * enforcement that allows this task to execute only after a specific earlier
 * one.
 *
 * \param target target rank for task execution
 * \param rp pointer to the appropriate runner function
 * \param tp pointer to the packed task data (function pointer and args)
 * \param sz size (bytes) of the packed task data
 * \param a the handle for the async after which this one may execute
 */
void _enqueue_after(int target, fptr rp, void *tp, size_t sz, handle_t a)
{
  task *t = new task(my_rank, target, rp, (byte *)tp, sz);
  t->set_depends(a);
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
 * Route a task to the correct queue for exection on the target: variant 3
 *
 * This routine will fast-path to our own local execution queue if we are in
 * fact the target. While it is not a user-facing function per se, it is called
 * by the \c async() task invocation launch functions (and thus cannot have
 * static linkage).
 *
 * In this particular variant, we include logic for async dependency
 * enforcement that allows both subsequent tasks to be ordered after this one
 * and for this task to execute only after a specific earlier one.
 *
 * \param target target rank for task execution
 * \param rp pointer to the appropriate runner function
 * \param tp pointer to the packed task data (function pointer and args)
 * \param sz size (bytes) of the packed task data
 * \param a the handle for the async after which this one may execute
 * \param h pointer to which this task's handle will be written
 */
void _enqueue_chain(int target, fptr rp, void *tp, size_t sz, handle_t a, handle_t *h)
{
  *h = handle_source++;
  task *t = new task(my_rank, target, rp, (byte *)tp, sz);
  t->set_depends(a);
  t->set_notify(*h);
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
  mpi_assert(MPI_Comm_dup(comm, &my_comm));
  mpi_assert(MPI_Comm_rank(my_comm, &my_rank));

  // setup callback counter
  mpi_assert(MPI_Alloc_mem(sizeof(int), MPI_INFO_NULL, &cb_count));
  *cb_count = 0;
  mpi_assert(MPI_Win_create(cb_count, sizeof(int), sizeof(int),
                            MPI_INFO_NULL,
                            comm, &cb_win));

  // setup the task and notify buffers
  task_buff = new rma_buff<task>(1, ASYNC_MSG_BUFFLEN, comm);
  notify_buff = new rma_buff<handle_t>(1, ASYNC_MSG_BUFFLEN, comm);

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
  delete notify_buff;
  delete task_buff;
}

/**
 * Block until we receive notification that the task associated with the
 * supplied handle has executed on the target.
 *
 * Progress will still be made on enqueued tasks and communications while
 * blocking, owing to the multithreaded nature of the library.
 *
 * \param h the \c handle_t associated with the task to wait for
 */
void async_wait(handle_t h)
{
  for (;;) {
    int completed;
    completed_tasks_mtx.lock();
    completed = completed_tasks.count(h);
    completed_tasks_mtx.unlock();
    if (completed)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/**
 * Block until all previously invoked tasks have executed (regardless of
 * whether they have associated handles)
 */
void async_barrier()
{
  // wait for all outstanding tasks to complete
  while (cb_get(my_rank))
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  mpi_assert(MPI_Barrier(my_comm));
}
