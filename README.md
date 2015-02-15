# Asynchronous remote tasks over MPI-3 RMA

Asynchronous remote task execution implemented on top of MPI-3 RMA. All
supporting communications are one-sided, including the incoming RMA task
buffers (which use the minimal
[rma-buffer](https://github.com/swfrench/rma-buffer)).

## Implementation

More soon

### Communications

### Threading

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
