/** @file */

#pragma once

#include <initializer_list>

#include <cstdlib>
#include <cstdint>

// generic function pointer used throughout
typedef void (*fptr)(void *);

// type for identifying asyncs (and helper fmt)
typedef uint64_t handle_t;
#define FMT_HANDLE "%llu"

// public function prototypes
void async_enable(MPI_Comm);
void async_disable();
void async_wait(std::initializer_list<handle_t>);
void async_barrier();

/// @cond INTERNAL_DOCS

void _enqueue(int, fptr, void *, size_t);
void _enqueue_handle(int, fptr, void *, size_t, handle_t *);
void _enqueue_after(int, fptr, void *, size_t, std::initializer_list<handle_t>);
void _enqueue_chain(int, fptr, void *, size_t, handle_t *, std::initializer_list<handle_t>);

/// @endcond

// automatically generated template types
#include "async_templates.hpp"
