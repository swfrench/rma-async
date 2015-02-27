#pragma once

#include <cstdlib>
#include <cstdint>

// generic function pointer used throughout
typedef void (*fptr)(void *);

// type for identifying asyncs
typedef uint64_t handle_t;

// public function prototypes
void async_enable(MPI_Comm);
void async_disable();
void _enqueue(int, fptr, void *, size_t);
void _enqueue_handle(int, fptr, void *, size_t, handle_t *);
void _enqueue_after(int, fptr, void *, size_t, handle_t);
void _enqueue_chain(int, fptr, void *, size_t, handle_t, handle_t *);

// automatically generated template types
#include "async_templates.hpp"
