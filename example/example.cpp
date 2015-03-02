#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cassert>

#include <mpi.h>

#include "async.hpp"

/*
 * a simple arity-two function to be executed asynchronously
 */
void func2(int x, int y)
{
  printf("[%s] x + y = %i\n", __func__, x + y);
}

/*
 * select a random target (yes, we could do this a lot better)
 */
int random_target(int size)
{
  static bool seeded = false;
  if (! seeded) {
    srand(time(NULL));
    seeded = true;
  }
  return rand() % size;
}

/*
 * example invocation of async tasks
 */
void example(int rank, int size)
{
  int target;

  // async exection starts ...
  async_enable(MPI_COMM_WORLD);

  // here we invoke func2 for execution on target with args {1,2}
  target = random_target(size);
  async(target, func2, 1, 2);

  // this is safe to do if you want ... progress is made asynchronously on a
  // duplicate communicator
  MPI_Barrier(MPI_COMM_WORLD);

  // you can also use lambdas in lieu of regular functions
  target = random_target(size);
  async(target, [rank, target] () {
    printf("lambda shipped from rank %i to rank %i!\n", rank, target);
  });

  // if needed, you can sync across ranks and wait for all previously invoked
  // tasks to execute
  async_barrier();

  // you can also have asyncs depend on earlier ones

  // - this one has no dependencies, but others will depend on it
  target = random_target(size);
  handle_t h1 = async_handle(target, [rank, target] () {
    printf("lambda shipped from rank %i to rank %i!\n", rank, target);
  });

  // - this one also has no dependencies, but others will depend on it
  target = random_target(size);
  handle_t h2 = async_handle(target, [rank, target] () {
    printf("lambda shipped from rank %i to rank %i!\n", rank, target);
  });

  // - this one depends on both of the earlier ones, and the next one is a
  //   dependent of this one
  target = random_target(size);
  handle_t h3 = async_chain({h1, h2}, target, [rank, target, h1, h2] () {
    printf("lambda shipped from rank %i to rank %i, that must run after "
           "asyncs with handles " FMT_HANDLE " and " FMT_HANDLE "\n",
           rank, target, h1, h2);
  });

  // - this one only depends on the last async (it has no dependents)
  target = random_target(size);
  async_after({h3}, target, [rank, target, h3] (int x, int y) {
    printf("lambda shipped from rank %i to rank %i, that must run after the "
           "async with handle " FMT_HANDLE " (oh and here's some computation: "
           "%i + %i = %i)\n",
           rank, target, h3, x, y, x + y);
  }, 3, 7);

  // async execution stops
  async_disable();
}


int main(int argc, char **argv)
{
  int requested, provided, rank, size;

  // ensure proper threading support is enabled
  requested = MPI_THREAD_MULTIPLE;
  MPI_Init_thread(&argc, &argv, requested, &provided);
  assert(provided >= requested);

  // get communicator details
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  // run the example
  example(rank, size);

  MPI_Finalize();

  return 0;
}
