#include "rma_buff.hpp"

#include "async_templates.hpp"

#include <list>
#include <thread>
#include <mutex>
#include <atomic>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <mpi.h>

#define ASYNC_ARGS_SIZE 512

typedef unsigned char byte;

struct task
{
  int origin, target;
  fptr task_func;
  byte task_args_data[ASYNC_ARGS_SIZE];
  task() {}
  task(int o, int t, fptr tf, byte *ta, size_t sz) :
    origin(o), target(t), task_func(tf)
  {
    assert(sz <= ASYNC_ARGS_SIZE);
    memcpy(task_args_data, ta, sz);
  }
} __attribute__ ((packed));

#define ASYNC_MSG_SIZE sizeof(struct task);
#define ASYNC_MSG_BUFFLEN 128

#define mpi_assert(X) assert(X == MPI_SUCCESS);

// maybe represent task with packed struct?
// src, dst, fp, and data?

static std::thread *th_progress;
static std::thread *th_comm;

static std::mutex task_queue_mtx;
static std::mutex outgoing_task_queue_mtx;

static std::list<task *> task_queue;
static std::list<task *> outgoing_task_queue;

static std::atomic<bool> done(false);

static int my_rank;
static MPI_Comm my_comm;
static MPI_Win cb_win;
static int *cb_count;

static rma_buff<task> *task_buff;

/*
 * Decrement the callback counter on the specified rank
 */
static void cb_dec(int target)
{
  int dec = -1;
  mpi_assert(MPI_Win_lock(MPI_LOCK_SHARED, target, 0, cb_win));
  mpi_assert(MPI_Accumulate(&dec,
                            1, MPI_INT, target, 0,
                            1, MPI_INT, MPI_SUM, cb_win));
  mpi_assert(MPI_Win_unlock(target, cb_win));
}

/*
 * Increment the callback counter on the specified rank
 */
static void cb_inc(int target)
{
  int inc = 1;
  mpi_assert(MPI_Win_lock(MPI_LOCK_SHARED, target, 0, cb_win));
  mpi_assert(MPI_Accumulate(&inc,
                            1, MPI_INT, target, 0,
                            1, MPI_INT, MPI_SUM, cb_win));
  mpi_assert(MPI_Win_unlock(target, cb_win));
}

/*
 * Route the supplied task to the correct queue for exection on the target,
 * fast-pathing to our own local execution queue if we are in fact the target.
 * This is called by the async() task invocation launch functions.
 */
void enqueue(int target, fptr rp, void *tp, size_t sz)
{
  task *t = new task(my_rank, target, rp, tp, sz);
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

void mover()
{
  task task_msg_in[ASYNC_MSG_BUFFLEN];
  while (! done) {
    int nmsg;
    // service incoming tasks (enqueue them for execution)
    if ((nmsg = task_buff->get(task_msg_in)) > 0) {
      for (int i = 0; i < nmsg; i++) {
        task *t = new task();
        memcpy(t, task_msg_in[i], ASYNC_MSG_SIZE);
        task_queue_mtx.lock();
        task_queue.push_back((task *) t);
        task_queue_mtx.unlock();
      }
    }

    // send outgoing tasks
    task *msg = NULL;
    outgoing_task_queue_mtx.lock();
    if (! outgoing_task_queue.empty()) {
      msg = outgoing_task_queue.front();
      outgoing_task_queue.pop_front();
    }
    outgoing_task_queue_mtx.unlock();
    if (msg != NULL) {
      while (! task_buff->put(msg->target, msg)) {
        printf("Retrying put\n");
      }
      delete msg;
    }
  }
}

void executer()
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
      msg->task_func(msg->task_data);
      cb_dec(msg->origin);
      delete msg;
    }
  }
}

/*
 * Collective: enable asynchronous task execution among ranks on the supplied
 * communicator
 */
void async_enable(MPI_Comm comm)
{
  int mpi_init, mpi_thread;

  mpi_assert(MPI_Initialized(&mpi_init));
  assert(mpi_init);
  mpi_assert(MPI_Query_thread(&mpi_thread));
  assert(mpi_thread == MPI_THREAD_MULTIPLE);

  my_comm = comm;
  mpi_assert(MPI_Comm_rank(my_comm, &my_rank));

  th_progress = new std::thread(progress);

  task_buff = new rma_buff<task>(1, ASYNC_MSG_BUFFLEN);

  mpi_assert(MPI_Barrier(my_comm));
}

/*
 * Collective: Stop invoking asynchronous tasks and block until all outstanding
 * tasks have been executed.
 */
void async_disable()
{
  while (cb_count) {};
  mpi_assert(MPI_Barrier(my_comm));
  done = true;
  th_progress->join();
  delete th_progress;
}
