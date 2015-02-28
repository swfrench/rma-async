# Asynchronous remote tasks over MPI-3 RMA

Asynchronous remote task execution implemented on top of MPI-3 RMA.
All supporting communications are one-sided, including the incoming task
buffers (using [rma-buffer](https://github.com/swfrench/rma-buffer)).

This is largely a proof-of-concept and remains a work in progress. The basic
idea is to adopt a model of asynchronous task execution similar to that in
UPC++ [1] and to demonstrate one particular way it could be implemented on
top of MPI-3.

## Interface

Here's a quick look at what actually using this might look like, using C++11
lambdas (regular functions should work as well):

    async_enable(MPI_COMM_WORLD);

    async(target, [rank] () {
      printf("i'm shipped from rank %i and no one depends on me\n", rank);
    });

    async_barrier();

    handle_t h1 = async_handle(target, [rank] () {
      printf("i'm shipped from rank %i\n", rank);
    });

    handle_t h2 = async_chain({h1}, target, [rank] () {
      printf("i'm shipped from rank %i, but i had to wait to execute\n", rank);
    });

    async_after({h2}, target, [rank] (int x, int y) {
      printf("i'm shipped from rank %i and i will be the last (oh and %i + %i = %i)\n", rank, x, y, x + y);
    }, 3, 4);

    async_disable();

There's also `async_chain`, which combines the ordering functionality of
`async_handle` and `async_after`, as well as `async_wait` for explicitly
waiting on completion of an ealier task. Note also that task dependencies are
specified within C++11 `initialization_list`s (a syntax-light way of specifying
multiple dependencies).

Note that due to the use of progress threads under the hood, we will continue
to make progress on executing and communicating asynchronous tasks while
waiting in `async_wait`.

## Implementation

More details soon.

### Communications

All communications are based on MPI-3 RMA operations, using a combination of
shared counters implemented with `MPI_Accumulate` / `MPI_Fetch_and_op` and the
RMA-based remote buffer implementation described
[here](https://github.com/swfrench/rma-buffer) (a dependency and thus
sub-module of this repo).

### Threading

At present, two progress threads are used in order to facilitate timely
execution of the asynchronous tasks:

* `mover`: responsible for managing the one-sided buffer, enqueueing incoming
  tasks in the local execution queue and placing outgoing tasks in the
  corresponding buffer on the target
* `executor`: responsible for executing tasks in the local execution queue and
  notifying the origin of their completion (at the moment, simply by
  decrementing the callback counter on the origin)

This may change later on. For example, in the interest of having fewer threads
running and if the async tasks are expected to be fairly light-weight, one
could add the option of using only a single progress thread, thereby merging
the `mover` and `executor`. Conversely, if async tasks are expected to be
heavier-weight, one could instead implement a thread *pool* dedicated to task
execution.

## TODO

There are a number of potential areas for improvement:

1.  The current code is designed as a library, with a number of internal static
    data structures and support routines. This could fairly easily be
    integrated into a single class, although I'm not sure this would really
    improve the interface per se.
2.  The automatic generation of async wrapper / runner template structs (which
    encode the type of the asynchronous task function and capture its
    arguments) is a bit awkward. Perhaps we can do something neat with variadic
    templates instead?
3.  ~~Dependency management between async tasks ("events" similar to UPC++?).~~
    A preliminary implementation of inter-task dependency management has been
    added.
4.  ~~The dependency model is currently tree-like (an async can have only one
    dependency but any number of dependents). Should this instead support more
    general DAGs?~~ Each task can now have multiple dependencies, see `example.cpp`.

## References

[1] [https://bitbucket.org/upcxx/upcxx](https://bitbucket.org/upcxx/upcxx)
