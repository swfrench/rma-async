#pragma once

// generic function pointer used throughout
typedef void (*fptr)(void *);

// public function prototypes
void async_enable(MPI_Comm);
void async_disable();
void _enqueue(int, fptr, void *, size_t);

// automatically generated template types
#include "async_templates.hpp"
