#!/bin/bash

set -e

# default max number of function parameters to support
DEFAULT_NUM_PARAMETERS=6

# header file names
ASYNC_TASK_DATA=include/gen.async_task_data.hpp
ASYNC_TASK_RUN=include/gen.async_task_run.hpp
ASYNC_TASK_INVOKE=include/gen.async_task_invoke.hpp

################################################################################
# args

if [ $# -eq 1 ]; then
        NUM_PARAMETERS=$1
else
        NUM_PARAMETERS=$DEFAULT_NUM_PARAMETERS
fi

################################################################################
# setup

# handles preamble and zero parameter counts

# task argument and fptr data
cat << END > $ASYNC_TASK_DATA
// automatically generated $(date)
#pragma once

/// @cond INTERNAL_DOCS

/*
 * Task argument and callback structures
 */

template<typename F>
struct task0
{
  F func;
  task0(F func_) : func(func_) {}
};

END

# task runners (casts to appropriate task data struct type)
cat << END > $ASYNC_TASK_RUN
// automatically generated $(date)
#pragma once

/// @cond INTERNAL_DOCS

/*
 * Run wrappers (overloaded; embeds casting to appropriate types)
 */

template<typename F>
void runner(void *tp)
{
  task0<F> *t = (task0<F> *)tp;
  t->func();
}

END

# task invocation
cat << END > $ASYNC_TASK_INVOKE
/** @file
 * Async task invocation wrappers
 *
 * These have been automatically generated for functions of arity up to $NUM_PARAMETERS.
 */

// automatically generated $(date)
#pragma once

#include <initializer_list>

/*
 * User-facing async invocation calls (overloaded)
 */

/**
 * Invoke an async task of zero arguments
 *
 * This function is automatically generated.
 */
template<typename F>
void async(int target, F f)
{
  task0<F> *t = new task0<F>(f);
  fptr rp = (fptr) runner<F>;
  _enqueue(target, rp, (void *)t, sizeof(*t));
}

/**
 * Invoke an async task of zero arguments, returning a \c handle_t for later
 * use in dependency tracking
 *
 * This function is automatically generated.
 */
template<typename F>
handle_t async_handle(int target, F f)
{
  handle_t h;
  task0<F> *t = new task0<F>(f);
  fptr rp = (fptr) runner<F>;
  _enqueue_handle(target, rp, (void *)t, sizeof(*t), &h);
  return h;
}

/**
 * Invoke an async task of zero arguments, accepting a list of \c handle_t from
 * tasks upon which this one depends
 *
 * This function is automatically generated.
 */
template<typename F>
void async_after(std::initializer_list<handle_t> a, int target, F f)
{
  task0<F> *t = new task0<F>(f);
  fptr rp = (fptr) runner<F>;
  _enqueue_after(target, rp, (void *)t, sizeof(*t), a);
}

/**
 * Invoke an async task of zero arguments, accepting a list of \c handle_t from
 * tasks upon which this one depends and returning yet another \c handle_t for
 * later use in dependency tracking
 *
 * This function is automatically generated.
 */
template<typename F>
handle_t async_chain(std::initializer_list<handle_t> a, int target, F f)
{
  handle_t h;
  task0<F> *t = new task0<F>(f);
  fptr rp = (fptr) runner<F>;
  _enqueue_chain(target, rp, (void *)t, sizeof(*t), &h, a);
  return h;
}

END

################################################################################
# higher parameter counts

# loop over argument counts
for n in `seq 1 $NUM_PARAMETERS`; do
        # name of task data struct
        name="task$n"

        # initialize ...

        # full template parameter list
        template="typename F"

        # template type names
        ttypes="F"

        # function arguments only (members of task data struct)
        fargs=""

        # template-typed arguments: with and without types
        proto="F f"
        proto_vars="f"

        # build them up ...
        for t in `seq 0 $((n-1))`; do
                template="$template, typename T$t"
                ttypes="$ttypes, T$t"
                [ -n "$fargs" ] && fargs="$fargs, "
                fargs="${fargs}t->x$t"
                proto="$proto, T$t x$t"
                proto_vars="$proto_vars, x$t"
        done

        # update runner and invocation header files ...

        cat << END >> $ASYNC_TASK_RUN
template<$template>
void runner(void *tp)
{
  $name<$ttypes> *t = ($name<$ttypes> *)tp;
  t->func($fargs);
}

END
        cat << END >> $ASYNC_TASK_INVOKE
/**
 * Invoke an async task of $n arguments
 *
 * This function is automatically generated.
 */
template<$template>
void async(int target, $proto)
{
  $name<$ttypes> *t = new $name<$ttypes>($proto_vars);
  fptr rp = (fptr) runner<$ttypes>;
  _enqueue(target, rp, (void *)t, sizeof(*t));
}

/**
 * Invoke an async task of $n arguments, returning a \c handle_t for later
 * use in dependency tracking
 *
 * This function is automatically generated.
 */
template<$template>
handle_t async_handle(int target, $proto)
{
  handle_t h;
  $name<$ttypes> *t = new $name<$ttypes>($proto_vars);
  fptr rp = (fptr) runner<$ttypes>;
  _enqueue_handle(target, rp, (void *)t, sizeof(*t), &h);
  return h;
}

/**
 * Invoke an async task of $n arguments, accepting a list of \c handle_t from
 * tasks upon which this one depends
 *
 * This function is automatically generated.
 */
template<$template>
void async_after(std::initializer_list<handle_t> a, int target, $proto)
{
  $name<$ttypes> *t = new $name<$ttypes>($proto_vars);
  fptr rp = (fptr) runner<$ttypes>;
  _enqueue_after(target, rp, (void *)t, sizeof(*t), a);
}

/**
 * Invoke an async task of $n arguments, accepting a list of \c handle_t from
 * tasks upon which this one depends and returning yet another \c handle_t for
 * later use in dependency tracking
 *
 * This function is automatically generated.
 */
template<$template>
handle_t async_chain(std::initializer_list<handle_t> a, int target, $proto)
{
  handle_t h;
  $name<$ttypes> *t = new $name<$ttypes>($proto_vars);
  fptr rp = (fptr) runner<$ttypes>;
  _enqueue_chain(target, rp, (void *)t, sizeof(*t), &h, a);
  return h;
}

END

        # update task data structs ...

        cat << END >> $ASYNC_TASK_DATA
template<$template>
struct $name
{
  F func;
END

        # generate constructor and member vars for task data structs
        targs="F func_"
        init="func(func_)"
        for t in `seq 0 $(($n-1))`; do
                echo "  T$t x$t;" >> $ASYNC_TASK_DATA
                targs="$targs, T$t x${t}_"
                init="$init, x$t(x${t}_)"
        done
        cat << END >> $ASYNC_TASK_DATA
  $name($targs) : $init {}
};

END
done

echo "/// @endcond" >> $ASYNC_TASK_DATA
echo "/// @endcond" >> $ASYNC_TASK_RUN
