/**
 * \mainpage Asynchronous remote tasks over MPI-3 RMA
 *
 * This is a simple, proof-of-concept implementation of asynchronous remote
 * tasks (a la UPC++) on top of MPI-3 RMA.
 */

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <mpi.h>

#include "rma-buffer/rma_buff.hpp"

#include "async.hpp"
#include "async_internal.hpp"


/***************************************
 * Internal data structures (file scope)
 */

// task buffer object: handles incoming / outgoing task messages; managed by
// the mover thread
static rma_buff<task_msg> *task_buff;

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

// mutual exclusion for task queues and the completed_tasks set
static std::mutex task_queue_mtx;
static std::mutex outgoing_task_queue_mtx;
static std::mutex outgoing_notify_queue_mtx;
static std::mutex completed_tasks_mtx;

// task queues
static std::list<task_msg *> task_queue;
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


/**************************
 * RMA shared counter utils
 */

/**
 * Decrement the callback counter on the specified rank
 *
 * \param target target rank on which to increment the counter
 */
static void cb_dec(int target)
{
  int dec = -1;
  mpi_assert(MPI_Accumulate(&dec,
                            1, MPI_INT, target, 0,
                            1, MPI_INT, MPI_SUM, cb_win));
  mpi_assert(MPI_Win_flush(target, cb_win));
}

/**
 * Increment the callback counter on the specified rank
 *
 * \param target target rank on which to increment the counter
 */
static void cb_inc(int target)
{
  int inc = 1;
  mpi_assert(MPI_Accumulate(&inc,
                            1, MPI_INT, target, 0,
                            1, MPI_INT, MPI_SUM, cb_win));
  mpi_assert(MPI_Win_flush(target, cb_win));
}

/**
 * Get the callback counter on the specified rank
 *
 * \param target target rank from which to retrieve that counter value
 */
static int cb_get(int target)
{
  int val;
  mpi_assert(MPI_Fetch_and_op(NULL, &val, MPI_INT, target, 0,
                              MPI_NO_OP, cb_win));
  mpi_assert(MPI_Win_flush(target, cb_win));
  return val;
}


/*************************
 * Progress thread actions
 */

/**
 * Action performed by the mover progress thread
 *
 * Moves async tasks and completion notifications in and out of the rma_buff
 * objects to / from their respective queues.
 */
static void mover()
{
  task_msg *task_msg_in = new task_msg[ASYNC_MSG_BUFFLEN];
  handle_t *notify_msg_in = new handle_t[ASYNC_MSG_BUFFLEN];
  while (! done) {
    int nmsg;

    //
    // service incoming / outgoing tasks
    //

    // service incoming tasks (enqueue them for execution)
    if ((nmsg = task_buff->get(task_msg_in, 1)) > 0) {
      for (int i = 0; i < nmsg; i++) {
        task_msg *t = new task_msg();
        memcpy(t, (void *)&task_msg_in[i], ASYNC_MSG_SIZE);
        task_queue_mtx.lock();
        task_queue.push_back(t);
        task_queue_mtx.unlock();
      }
    }

    // place outgoing tasks on their respective targets
    task *task_out = NULL;
    outgoing_task_queue_mtx.lock();
    if (! outgoing_task_queue.empty()) {
      task_out = outgoing_task_queue.front();
      outgoing_task_queue.pop_front();
    }
    outgoing_task_queue_mtx.unlock();
    if (task_out != NULL) {
      bool success = false;
      if (! task_out->depends) {
        task_msg *msg = task_out->to_msg();
        success = task_buff->put(task_out->target, msg);
        delete msg;
      } else {
        int completed = 0;
        completed_tasks_mtx.lock();
        for (std::vector<handle_t>::iterator it = task_out->after.begin();
             it != task_out->after.end();
             it += 1)
          completed += completed_tasks.count(*it);
        completed_tasks_mtx.unlock();
        if (completed == task_out->after.size()) {
          task_msg *msg = task_out->to_msg();
          success = task_buff->put(task_out->target, msg);
          delete msg;
        }
      }
      if (success) {
        // success: clean up
        delete task_out;
      } else {
        // failure: the target buffer must be full or the dependency has not
        // yet executed; put the message back in the queue and retry later
        outgoing_task_queue_mtx.lock();
        outgoing_task_queue.push_back(task_out);
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

/**
 * Action performed by the executor thread
 *
 * Remove tasks from the queue and run them (remembering to decrement the
 * origin callback counter and enqueue associated notifications thereafter)
 */
static void executor()
{
  while (! done) {
    task_msg *msg = NULL;
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


/************************
 * Utils to enqueue tasks
 */

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
    task_msg *m = t->to_msg();
    task_queue_mtx.lock();
    task_queue.push_back(m);
    task_queue_mtx.unlock();
    delete t;
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
    task_msg *m = t->to_msg();
    task_queue_mtx.lock();
    task_queue.push_back(m);
    task_queue_mtx.unlock();
    delete t;
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
 * \param a the list of handles for asyncs after which this one may execute
 */
void _enqueue_after(int target, fptr rp, void *tp, size_t sz, std::initializer_list<handle_t> a)
{
  task *t = new task(my_rank, target, rp, (byte *)tp, sz);
  for (handle_t e : a)
    t->set_depends(e);
  if (target == my_rank) {
    task_msg *m = t->to_msg();
    task_queue_mtx.lock();
    task_queue.push_back(m);
    task_queue_mtx.unlock();
    delete t;
  } else {
    outgoing_task_queue_mtx.lock();
    outgoing_task_queue.push_back(t);
    outgoing_task_queue_mtx.unlock();
  }
  cb_inc(my_rank);
}

/**
 * Route a task to the correct queue for exection on the target: variant 4
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
 * \param h pointer to which this task's handle will be written
 * \param a the list of handles for asyncs after which this one may execute
 */
void _enqueue_chain(int target, fptr rp, void *tp, size_t sz, handle_t *h, std::initializer_list<handle_t> a)
{
  *h = handle_source++;
  task *t = new task(my_rank, target, rp, (byte *)tp, sz);
  for (handle_t e : a)
    t->set_depends(e);
  t->set_notify(*h);
  if (target == my_rank) {
    task_msg *m = t->to_msg();
    task_queue_mtx.lock();
    task_queue.push_back(m);
    task_queue_mtx.unlock();
    delete t;
  } else {
    outgoing_task_queue_mtx.lock();
    outgoing_task_queue.push_back(t);
    outgoing_task_queue_mtx.unlock();
  }
  cb_inc(my_rank);
}


/****************************************
 * Library init / deinit, synchronization
 */

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

  // start the shared passive access epoch (given the semantics of
  // MPI_Accumulate and MPI_Fetch_and_op, none of these accesses should be
  // classified as "conflicting" so MPI_MODE_NOCHECK is appropriate)
  mpi_assert(MPI_Win_lock_all(MPI_MODE_NOCHECK, cb_win));

  // setup the task and notify buffers
  task_buff = new rma_buff<task_msg>(1, ASYNC_MSG_BUFFLEN, comm);
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
  wait_exp([] () { return cb_get(my_rank) > 0; },
           std::chrono::milliseconds(1),
           std::chrono::milliseconds(1000));

  // synchronize
  mpi_assert(MPI_Barrier(my_comm));

  // no more work to do; shut down the progress threads
  done = true;
  th_mover->join();
  th_executor->join();

  // end the shared passive access epoch
  mpi_assert(MPI_Win_unlock_all(cb_win));

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
 * \param h the list of \c handle_t associated with the tasks to wait for
 */
void async_wait(std::initializer_list<handle_t> h)
{
  wait_exp([h] () {
      int completed = 0;
      completed_tasks_mtx.lock();
      for (handle_t e : h)
        completed += completed_tasks.count(e);
      completed_tasks_mtx.unlock();
      return completed < h.size();
    },
    std::chrono::milliseconds(1),
    std::chrono::milliseconds(1000));
}

/**
 * Block until all previously invoked tasks have executed (regardless of
 * whether they have associated handles)
 */
void async_barrier()
{
  // wait for all outstanding tasks to complete
  wait_exp([] () { return cb_get(my_rank) > 0; },
           std::chrono::milliseconds(1),
           std::chrono::milliseconds(1000));
  mpi_assert(MPI_Barrier(my_comm));
}
