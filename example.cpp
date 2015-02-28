#include <stdio.h>
#include <assert.h>
#include <mpi.h>

#include "async.hpp"

void
func0()
{
  printf("[%s] hello!\n", __func__);
}

void
func2(int x, int y)
{
  printf("[%s] x + y = %i\n", __func__, x + y);
}

int
main(int argc, char **argv)
{
  int requested, provided, rank, size;

  requested = MPI_THREAD_MULTIPLE;
  MPI_Init_thread(&argc, &argv, requested, &provided);
  assert(provided >= requested);

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  async_enable(MPI_COMM_WORLD);

  async((rank + 1) % size, func0);

  async((rank + 1) % size, func2, 1, 2);

  MPI_Barrier(MPI_COMM_WORLD); // this is safe to do if you want

  async((rank + 1) % size, [rank] () {
    printf("i'm a lambda shipped from rank %i!\n", rank);
  });

  async_barrier(); // wait for all previously invoked tasks

  handle_t h1 = async_handle((rank + 1) % size, [rank] () {
    printf("i'm a lambda shipped from rank %i, but i have to happen first!\n",
           rank);
  });

  handle_t h2 = async_chain(h1, (rank + 1) % size, [rank,h1] () {
    printf("i'm a lambda shipped from rank %i, and i'm running after %lli!\n",
           rank, h1);
  });

  async_after(h2, (rank + 1) % size, [rank,h2] () {
    printf("i'm a lambda shipped from rank %i, and i'm running after %lli!\n",
           rank, h2);
  });

  async_disable();

  MPI_Finalize();

  return 0;
}
