/**
 * Asynchronous remote tasks over MPI
 */
#pragma once

#include <stdlib.h>
#include <mpi.h>

// generic function pointer used throughout
typedef void (*fptr)(void *);

/*
 * Task argument and callback structures
 */

#include "gen.async_task_data.hpp"

/*
 * Callback wrappers (overloaded; embeds casting to appropriate types)
 */

#include "gen.async_task_run.hpp"

/*
 * User-facing async invocation calls (overloaded)
 */

#include "gen.async_task_invoke.hpp"
