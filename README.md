# Asynchronous remote tasks over MPI-3 RMA

Asynchronous remote task execution implemented on top of MPI-3 RMA.

All supporting communications are one-sided, including the incoming RMA task
buffers (using [rma-buffer](https://github.com/swfrench/rma-buffer)).

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

1.  The current code is designed as a library, with a number of internal static
    data structures and support routines. This could fairly easily be
    integrated into a single class, although I'm not sure this would really
    improve the interface per se.
2.  The automatic generation of async wrapper / runner template structs (which
    encode the type of the asynchronous task function and capture its
    arguments) is a bit awkward. Perhaps we can do something neat with variadic
    templates instead?
3.  Dependency management between async tasks ("events" similar to UPC++?)
