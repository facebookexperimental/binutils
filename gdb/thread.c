/* Multi-process/thread control for GDB, the GNU debugger.

   Copyright (C) 1986-2015 Free Software Foundation, Inc.

   Contributed by Lynx Real-Time Systems, Inc.  Los Gatos, CA.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "symtab.h"
#include "frame.h"
#include "inferior.h"
#include "environ.h"
#include "value.h"
#include "target.h"
#include "gdbthread.h"
#include "command.h"
#include "gdbcmd.h"
#include "regcache.h"
#include "gdb.h"
#include "btrace.h"

#include <ctype.h>
#include <sys/types.h>
#include <signal.h>
#include "ui-out.h"
#include "observer.h"
#include "annotate.h"
#include "cli/cli-decode.h"
#include "gdb_regex.h"
#include "cli/cli-utils.h"
#include "continuations.h"

/* Definition of struct thread_info exported to gdbthread.h.  */

/* Prototypes for exported functions.  */

void _initialize_thread (void);

/* Prototypes for local functions.  */

struct thread_info *thread_list = NULL;
static int highest_thread_num;

/* True if any thread is, or may be executing.  We need to track this
   separately because until we fully sync the thread list, we won't
   know whether the target is fully stopped, even if we see stop
   events for all known threads, because any of those threads may have
   spawned new threads we haven't heard of yet.  */
static int threads_executing;

static void thread_apply_all_command (char *, int);
static int thread_alive (struct thread_info *);
static void info_threads_command (char *, int);
static void thread_apply_command (char *, int);
static void restore_current_thread (ptid_t);

/* Data to cleanup thread array.  */

struct thread_array_cleanup
{
  /* Array of thread pointers used to set
     reference count.  */
  struct thread_info **tp_array;

  /* Thread count in the array.  */
  int count;
};


struct thread_info*
inferior_thread (void)
{
  struct thread_info *tp = find_thread_ptid (inferior_ptid);
  gdb_assert (tp);
  return tp;
}

/* Delete the breakpoint pointed at by BP_P, if there's one.  */

static void
delete_thread_breakpoint (struct breakpoint **bp_p)
{
  if (*bp_p != NULL)
    {
      delete_breakpoint (*bp_p);
      *bp_p = NULL;
    }
}

void
delete_step_resume_breakpoint (struct thread_info *tp)
{
  if (tp != NULL)
    delete_thread_breakpoint (&tp->control.step_resume_breakpoint);
}

void
delete_exception_resume_breakpoint (struct thread_info *tp)
{
  if (tp != NULL)
    delete_thread_breakpoint (&tp->control.exception_resume_breakpoint);
}

/* See gdbthread.h.  */

void
delete_single_step_breakpoints (struct thread_info *tp)
{
  if (tp != NULL)
    delete_thread_breakpoint (&tp->control.single_step_breakpoints);
}

/* Delete the breakpoint pointed at by BP_P at the next stop, if
   there's one.  */

static void
delete_at_next_stop (struct breakpoint **bp)
{
  if (*bp != NULL)
    {
      (*bp)->disposition = disp_del_at_next_stop;
      *bp = NULL;
    }
}

/* See gdbthread.h.  */

int
thread_has_single_step_breakpoints_set (struct thread_info *tp)
{
  return tp->control.single_step_breakpoints != NULL;
}

/* See gdbthread.h.  */

int
thread_has_single_step_breakpoint_here (struct thread_info *tp,
					struct address_space *aspace,
					CORE_ADDR addr)
{
  struct breakpoint *ss_bps = tp->control.single_step_breakpoints;

  return (ss_bps != NULL
	  && breakpoint_has_location_inserted_here (ss_bps, aspace, addr));
}

static void
clear_thread_inferior_resources (struct thread_info *tp)
{
  /* NOTE: this will take care of any left-over step_resume breakpoints,
     but not any user-specified thread-specific breakpoints.  We can not
     delete the breakpoint straight-off, because the inferior might not
     be stopped at the moment.  */
  delete_at_next_stop (&tp->control.step_resume_breakpoint);
  delete_at_next_stop (&tp->control.exception_resume_breakpoint);
  delete_at_next_stop (&tp->control.single_step_breakpoints);

  delete_longjmp_breakpoint_at_next_stop (tp->num);

  bpstat_clear (&tp->control.stop_bpstat);

  btrace_teardown (tp);

  do_all_intermediate_continuations_thread (tp, 1);
  do_all_continuations_thread (tp, 1);
}

static void
free_thread (struct thread_info *tp)
{
  if (tp->priv)
    {
      if (tp->private_dtor)
	tp->private_dtor (tp->priv);
      else
	xfree (tp->priv);
    }

  xfree (tp->name);
  xfree (tp);
}

void
init_thread_list (void)
{
  struct thread_info *tp, *tpnext;

  highest_thread_num = 0;

  if (!thread_list)
    return;

  for (tp = thread_list; tp; tp = tpnext)
    {
      tpnext = tp->next;
      free_thread (tp);
    }

  thread_list = NULL;
  threads_executing = 0;
}

/* Allocate a new thread with target id PTID and add it to the thread
   list.  */

static struct thread_info *
new_thread (ptid_t ptid)
{
  struct thread_info *tp;

  tp = xcalloc (1, sizeof (*tp));

  tp->ptid = ptid;
  tp->num = ++highest_thread_num;
  tp->next = thread_list;
  thread_list = tp;

  /* Nothing to follow yet.  */
  tp->pending_follow.kind = TARGET_WAITKIND_SPURIOUS;
  tp->state = THREAD_STOPPED;
  tp->suspend.waitstatus.kind = TARGET_WAITKIND_IGNORE;

  return tp;
}

struct thread_info *
add_thread_silent (ptid_t ptid)
{
  struct thread_info *tp;

  tp = find_thread_ptid (ptid);
  if (tp)
    /* Found an old thread with the same id.  It has to be dead,
       otherwise we wouldn't be adding a new thread with the same id.
       The OS is reusing this id --- delete it, and recreate a new
       one.  */
    {
      /* In addition to deleting the thread, if this is the current
	 thread, then we need to take care that delete_thread doesn't
	 really delete the thread if it is inferior_ptid.  Create a
	 new template thread in the list with an invalid ptid, switch
	 to it, delete the original thread, reset the new thread's
	 ptid, and switch to it.  */

      if (ptid_equal (inferior_ptid, ptid))
	{
	  tp = new_thread (null_ptid);

	  /* Make switch_to_thread not read from the thread.  */
	  tp->state = THREAD_EXITED;
	  switch_to_thread (null_ptid);

	  /* Now we can delete it.  */
	  delete_thread (ptid);

	  /* Now reset its ptid, and reswitch inferior_ptid to it.  */
	  tp->ptid = ptid;
	  tp->state = THREAD_STOPPED;
	  switch_to_thread (ptid);

	  observer_notify_new_thread (tp);

	  /* All done.  */
	  return tp;
	}
      else
	/* Just go ahead and delete it.  */
	delete_thread (ptid);
    }

  tp = new_thread (ptid);
  observer_notify_new_thread (tp);

  return tp;
}

struct thread_info *
add_thread_with_info (ptid_t ptid, struct private_thread_info *priv)
{
  struct thread_info *result = add_thread_silent (ptid);

  result->priv = priv;

  if (print_thread_events)
    printf_unfiltered (_("[New %s]\n"), target_pid_to_str (ptid));

  annotate_new_thread ();
  return result;
}

struct thread_info *
add_thread (ptid_t ptid)
{
  return add_thread_with_info (ptid, NULL);
}

/* Add TP to the end of the step-over chain LIST_P.  */

static void
step_over_chain_enqueue (struct thread_info **list_p, struct thread_info *tp)
{
  gdb_assert (tp->step_over_next == NULL);
  gdb_assert (tp->step_over_prev == NULL);

  if (*list_p == NULL)
    {
      *list_p = tp;
      tp->step_over_prev = tp->step_over_next = tp;
    }
  else
    {
      struct thread_info *head = *list_p;
      struct thread_info *tail = head->step_over_prev;

      tp->step_over_prev = tail;
      tp->step_over_next = head;
      head->step_over_prev = tp;
      tail->step_over_next = tp;
    }
}

/* Remove TP from step-over chain LIST_P.  */

static void
step_over_chain_remove (struct thread_info **list_p, struct thread_info *tp)
{
  gdb_assert (tp->step_over_next != NULL);
  gdb_assert (tp->step_over_prev != NULL);

  if (*list_p == tp)
    {
      if (tp == tp->step_over_next)
	*list_p = NULL;
      else
	*list_p = tp->step_over_next;
    }

  tp->step_over_prev->step_over_next = tp->step_over_next;
  tp->step_over_next->step_over_prev = tp->step_over_prev;
  tp->step_over_prev = tp->step_over_next = NULL;
}

/* See gdbthread.h.  */

struct thread_info *
thread_step_over_chain_next (struct thread_info *tp)
{
  struct thread_info *next = tp->step_over_next;

  return (next == step_over_queue_head ? NULL : next);
}

/* See gdbthread.h.  */

int
thread_is_in_step_over_chain (struct thread_info *tp)
{
  return (tp->step_over_next != NULL);
}

/* See gdbthread.h.  */

void
thread_step_over_chain_enqueue (struct thread_info *tp)
{
  step_over_chain_enqueue (&step_over_queue_head, tp);
}

/* See gdbthread.h.  */

void
thread_step_over_chain_remove (struct thread_info *tp)
{
  step_over_chain_remove (&step_over_queue_head, tp);
}

/* Delete thread PTID.  If SILENT, don't notify the observer of this
   exit.  */
static void
delete_thread_1 (ptid_t ptid, int silent)
{
  struct thread_info *tp, *tpprev;

  tpprev = NULL;

  for (tp = thread_list; tp; tpprev = tp, tp = tp->next)
    if (ptid_equal (tp->ptid, ptid))
      break;

  if (!tp)
    return;

  /* Dead threads don't need to step-over.  Remove from queue.  */
  if (tp->step_over_next != NULL)
    thread_step_over_chain_remove (tp);

  /* If this is the current thread, or there's code out there that
     relies on it existing (refcount > 0) we can't delete yet.  Mark
     it as exited, and notify it.  */
  if (tp->refcount > 0
      || ptid_equal (tp->ptid, inferior_ptid))
    {
      if (tp->state != THREAD_EXITED)
	{
	  observer_notify_thread_exit (tp, silent);

	  /* Tag it as exited.  */
	  tp->state = THREAD_EXITED;

	  /* Clear breakpoints, etc. associated with this thread.  */
	  clear_thread_inferior_resources (tp);
	}

       /* Will be really deleted some other time.  */
       return;
     }

  /* Notify thread exit, but only if we haven't already.  */
  if (tp->state != THREAD_EXITED)
    observer_notify_thread_exit (tp, silent);

  /* Tag it as exited.  */
  tp->state = THREAD_EXITED;
  clear_thread_inferior_resources (tp);

  if (tpprev)
    tpprev->next = tp->next;
  else
    thread_list = tp->next;

  free_thread (tp);
}

/* Delete thread PTID and notify of thread exit.  If this is
   inferior_ptid, don't actually delete it, but tag it as exited and
   do the notification.  If PTID is the user selected thread, clear
   it.  */
void
delete_thread (ptid_t ptid)
{
  delete_thread_1 (ptid, 0 /* not silent */);
}

void
delete_thread_silent (ptid_t ptid)
{
  delete_thread_1 (ptid, 1 /* silent */);
}

struct thread_info *
find_thread_id (int num)
{
  struct thread_info *tp;

  for (tp = thread_list; tp; tp = tp->next)
    if (tp->num == num)
      return tp;

  return NULL;
}

/* Find a thread_info by matching PTID.  */
struct thread_info *
find_thread_ptid (ptid_t ptid)
{
  struct thread_info *tp;

  for (tp = thread_list; tp; tp = tp->next)
    if (ptid_equal (tp->ptid, ptid))
      return tp;

  return NULL;
}

/*
 * Thread iterator function.
 *
 * Calls a callback function once for each thread, so long as
 * the callback function returns false.  If the callback function
 * returns true, the iteration will end and the current thread
 * will be returned.  This can be useful for implementing a 
 * search for a thread with arbitrary attributes, or for applying
 * some operation to every thread.
 *
 * FIXME: some of the existing functionality, such as 
 * "Thread apply all", might be rewritten using this functionality.
 */

struct thread_info *
iterate_over_threads (int (*callback) (struct thread_info *, void *),
		      void *data)
{
  struct thread_info *tp, *next;

  for (tp = thread_list; tp; tp = next)
    {
      next = tp->next;
      if ((*callback) (tp, data))
	return tp;
    }

  return NULL;
}

int
thread_count (void)
{
  int result = 0;
  struct thread_info *tp;

  for (tp = thread_list; tp; tp = tp->next)
    ++result;

  return result;  
}

int
valid_thread_id (int num)
{
  struct thread_info *tp;

  for (tp = thread_list; tp; tp = tp->next)
    if (tp->num == num)
      return 1;

  return 0;
}

int
pid_to_thread_id (ptid_t ptid)
{
  struct thread_info *tp;

  for (tp = thread_list; tp; tp = tp->next)
    if (ptid_equal (tp->ptid, ptid))
      return tp->num;

  return 0;
}

ptid_t
thread_id_to_pid (int num)
{
  struct thread_info *thread = find_thread_id (num);

  if (thread)
    return thread->ptid;
  else
    return pid_to_ptid (-1);
}

int
in_thread_list (ptid_t ptid)
{
  struct thread_info *tp;

  for (tp = thread_list; tp; tp = tp->next)
    if (ptid_equal (tp->ptid, ptid))
      return 1;

  return 0;			/* Never heard of 'im.  */
}

/* Finds the first thread of the inferior given by PID.  If PID is -1,
   return the first thread in the list.  */

struct thread_info *
first_thread_of_process (int pid)
{
  struct thread_info *tp, *ret = NULL;

  for (tp = thread_list; tp; tp = tp->next)
    if (pid == -1 || ptid_get_pid (tp->ptid) == pid)
      if (ret == NULL || tp->num < ret->num)
	ret = tp;

  return ret;
}

struct thread_info *
any_thread_of_process (int pid)
{
  struct thread_info *tp;

  gdb_assert (pid != 0);

  /* Prefer the current thread.  */
  if (ptid_get_pid (inferior_ptid) == pid)
    return inferior_thread ();

  ALL_NON_EXITED_THREADS (tp)
    if (ptid_get_pid (tp->ptid) == pid)
      return tp;

  return NULL;
}

struct thread_info *
any_live_thread_of_process (int pid)
{
  struct thread_info *curr_tp = NULL;
  struct thread_info *tp;
  struct thread_info *tp_executing = NULL;

  gdb_assert (pid != 0);

  /* Prefer the current thread if it's not executing.  */
  if (ptid_get_pid (inferior_ptid) == pid)
    {
      /* If the current thread is dead, forget it.  If it's not
	 executing, use it.  Otherwise, still choose it (below), but
	 only if no other non-executing thread is found.  */
      curr_tp = inferior_thread ();
      if (curr_tp->state == THREAD_EXITED)
	curr_tp = NULL;
      else if (!curr_tp->executing)
	return curr_tp;
    }

  ALL_NON_EXITED_THREADS (tp)
    if (ptid_get_pid (tp->ptid) == pid)
      {
	if (!tp->executing)
	  return tp;

	tp_executing = tp;
      }

  /* If both the current thread and all live threads are executing,
     prefer the current thread.  */
  if (curr_tp != NULL)
    return curr_tp;

  /* Otherwise, just return an executing thread, if any.  */
  return tp_executing;
}

/* Print a list of thread ids currently known, and the total number of
   threads.  To be used from within catch_errors.  */
static int
do_captured_list_thread_ids (struct ui_out *uiout, void *arg)
{
  struct thread_info *tp;
  int num = 0;
  struct cleanup *cleanup_chain;
  int current_thread = -1;

  update_thread_list ();

  cleanup_chain = make_cleanup_ui_out_tuple_begin_end (uiout, "thread-ids");

  for (tp = thread_list; tp; tp = tp->next)
    {
      if (tp->state == THREAD_EXITED)
	continue;

      if (ptid_equal (tp->ptid, inferior_ptid))
	current_thread = tp->num;

      num++;
      ui_out_field_int (uiout, "thread-id", tp->num);
    }

  do_cleanups (cleanup_chain);

  if (current_thread != -1)
    ui_out_field_int (uiout, "current-thread-id", current_thread);
  ui_out_field_int (uiout, "number-of-threads", num);
  return GDB_RC_OK;
}

/* Official gdblib interface function to get a list of thread ids and
   the total number.  */
enum gdb_rc
gdb_list_thread_ids (struct ui_out *uiout, char **error_message)
{
  if (catch_exceptions_with_msg (uiout, do_captured_list_thread_ids, NULL,
				 error_message, RETURN_MASK_ALL) < 0)
    return GDB_RC_FAIL;
  return GDB_RC_OK;
}

/* Return true if TP is an active thread.  */
static int
thread_alive (struct thread_info *tp)
{
  if (tp->state == THREAD_EXITED)
    return 0;
  if (!target_thread_alive (tp->ptid))
    return 0;
  return 1;
}

/* See gdbthreads.h.  */

void
prune_threads (void)
{
  struct thread_info *tp, *tmp;

  ALL_THREADS_SAFE (tp, tmp)
    {
      if (!thread_alive (tp))
	delete_thread (tp->ptid);
    }
}

/* See gdbthreads.h.  */

void
delete_exited_threads (void)
{
  struct thread_info *tp, *tmp;

  ALL_THREADS_SAFE (tp, tmp)
    {
      if (tp->state == THREAD_EXITED)
	delete_thread (tp->ptid);
    }
}

/* Disable storing stack temporaries for the thread whose id is
   stored in DATA.  */

static void
disable_thread_stack_temporaries (void *data)
{
  ptid_t *pd = data;
  struct thread_info *tp = find_thread_ptid (*pd);

  if (tp != NULL)
    {
      tp->stack_temporaries_enabled = 0;
      VEC_free (value_ptr, tp->stack_temporaries);
    }

  xfree (pd);
}

/* Enable storing stack temporaries for thread with id PTID and return a
   cleanup which can disable and clear the stack temporaries.  */

struct cleanup *
enable_thread_stack_temporaries (ptid_t ptid)
{
  struct thread_info *tp = find_thread_ptid (ptid);
  ptid_t  *data;
  struct cleanup *c;

  gdb_assert (tp != NULL);

  tp->stack_temporaries_enabled = 1;
  tp->stack_temporaries = NULL;
  data = (ptid_t *) xmalloc (sizeof (ptid_t));
  *data = ptid;
  c = make_cleanup (disable_thread_stack_temporaries, data);

  return c;
}

/* Return non-zero value if stack temporaies are enabled for the thread
   with id PTID.  */

int
thread_stack_temporaries_enabled_p (ptid_t ptid)
{
  struct thread_info *tp = find_thread_ptid (ptid);

  if (tp == NULL)
    return 0;
  else
    return tp->stack_temporaries_enabled;
}

/* Push V on to the stack temporaries of the thread with id PTID.  */

void
push_thread_stack_temporary (ptid_t ptid, struct value *v)
{
  struct thread_info *tp = find_thread_ptid (ptid);

  gdb_assert (tp != NULL && tp->stack_temporaries_enabled);
  VEC_safe_push (value_ptr, tp->stack_temporaries, v);
}

/* Return 1 if VAL is among the stack temporaries of the thread
   with id PTID.  Return 0 otherwise.  */

int
value_in_thread_stack_temporaries (struct value *val, ptid_t ptid)
{
  struct thread_info *tp = find_thread_ptid (ptid);

  gdb_assert (tp != NULL && tp->stack_temporaries_enabled);
  if (!VEC_empty (value_ptr, tp->stack_temporaries))
    {
      struct value *v;
      int i;

      for (i = 0; VEC_iterate (value_ptr, tp->stack_temporaries, i, v); i++)
	if (v == val)
	  return 1;
    }

  return 0;
}

/* Return the last of the stack temporaries for thread with id PTID.
   Return NULL if there are no stack temporaries for the thread.  */

struct value *
get_last_thread_stack_temporary (ptid_t ptid)
{
  struct value *lastval = NULL;
  struct thread_info *tp = find_thread_ptid (ptid);

  gdb_assert (tp != NULL);
  if (!VEC_empty (value_ptr, tp->stack_temporaries))
    lastval = VEC_last (value_ptr, tp->stack_temporaries);

  return lastval;
}

void
thread_change_ptid (ptid_t old_ptid, ptid_t new_ptid)
{
  struct inferior *inf;
  struct thread_info *tp;

  /* It can happen that what we knew as the target inferior id
     changes.  E.g, target remote may only discover the remote process
     pid after adding the inferior to GDB's list.  */
  inf = find_inferior_ptid (old_ptid);
  inf->pid = ptid_get_pid (new_ptid);

  tp = find_thread_ptid (old_ptid);
  tp->ptid = new_ptid;

  observer_notify_thread_ptid_changed (old_ptid, new_ptid);
}

/* See gdbthread.h.  */

void
set_resumed (ptid_t ptid, int resumed)
{
  struct thread_info *tp;
  int all = ptid_equal (ptid, minus_one_ptid);

  if (all || ptid_is_pid (ptid))
    {
      for (tp = thread_list; tp; tp = tp->next)
	if (all || ptid_get_pid (tp->ptid) == ptid_get_pid (ptid))
	  tp->resumed = resumed;
    }
  else
    {
      tp = find_thread_ptid (ptid);
      gdb_assert (tp != NULL);
      tp->resumed = resumed;
    }
}

/* Helper for set_running, that marks one thread either running or
   stopped.  */

static int
set_running_thread (struct thread_info *tp, int running)
{
  int started = 0;

  if (running && tp->state == THREAD_STOPPED)
    started = 1;
  tp->state = running ? THREAD_RUNNING : THREAD_STOPPED;

  if (!running)
    {
      /* If the thread is now marked stopped, remove it from
	 the step-over queue, so that we don't try to resume
	 it until the user wants it to.  */
      if (tp->step_over_next != NULL)
	thread_step_over_chain_remove (tp);
    }

  return started;
}

void
set_running (ptid_t ptid, int running)
{
  struct thread_info *tp;
  int all = ptid_equal (ptid, minus_one_ptid);
  int any_started = 0;

  /* We try not to notify the observer if no thread has actually changed 
     the running state -- merely to reduce the number of messages to 
     frontend.  Frontend is supposed to handle multiple *running just fine.  */
  if (all || ptid_is_pid (ptid))
    {
      for (tp = thread_list; tp; tp = tp->next)
	if (all || ptid_get_pid (tp->ptid) == ptid_get_pid (ptid))
	  {
	    if (tp->state == THREAD_EXITED)
	      continue;

	    if (set_running_thread (tp, running))
	      any_started = 1;
	  }
    }
  else
    {
      tp = find_thread_ptid (ptid);
      gdb_assert (tp != NULL);
      gdb_assert (tp->state != THREAD_EXITED);
      if (set_running_thread (tp, running))
	any_started = 1;
    }
  if (any_started)
    observer_notify_target_resumed (ptid);
}

static int
is_thread_state (ptid_t ptid, enum thread_state state)
{
  struct thread_info *tp;

  tp = find_thread_ptid (ptid);
  gdb_assert (tp);
  return tp->state == state;
}

int
is_stopped (ptid_t ptid)
{
  return is_thread_state (ptid, THREAD_STOPPED);
}

int
is_exited (ptid_t ptid)
{
  return is_thread_state (ptid, THREAD_EXITED);
}

int
is_running (ptid_t ptid)
{
  return is_thread_state (ptid, THREAD_RUNNING);
}

int
is_executing (ptid_t ptid)
{
  struct thread_info *tp;

  tp = find_thread_ptid (ptid);
  gdb_assert (tp);
  return tp->executing;
}

void
set_executing (ptid_t ptid, int executing)
{
  struct thread_info *tp;
  int all = ptid_equal (ptid, minus_one_ptid);

  if (all || ptid_is_pid (ptid))
    {
      for (tp = thread_list; tp; tp = tp->next)
	if (all || ptid_get_pid (tp->ptid) == ptid_get_pid (ptid))
	  tp->executing = executing;
    }
  else
    {
      tp = find_thread_ptid (ptid);
      gdb_assert (tp);
      tp->executing = executing;
    }

  /* It only takes one running thread to spawn more threads.*/
  if (executing)
    threads_executing = 1;
  /* Only clear the flag if the caller is telling us everything is
     stopped.  */
  else if (ptid_equal (minus_one_ptid, ptid))
    threads_executing = 0;
}

/* See gdbthread.h.  */

int
threads_are_executing (void)
{
  return threads_executing;
}

void
set_stop_requested (ptid_t ptid, int stop)
{
  struct thread_info *tp;
  int all = ptid_equal (ptid, minus_one_ptid);

  if (all || ptid_is_pid (ptid))
    {
      for (tp = thread_list; tp; tp = tp->next)
	if (all || ptid_get_pid (tp->ptid) == ptid_get_pid (ptid))
	  tp->stop_requested = stop;
    }
  else
    {
      tp = find_thread_ptid (ptid);
      gdb_assert (tp);
      tp->stop_requested = stop;
    }

  /* Call the stop requested observer so other components of GDB can
     react to this request.  */
  if (stop)
    observer_notify_thread_stop_requested (ptid);
}

void
finish_thread_state (ptid_t ptid)
{
  struct thread_info *tp;
  int all;
  int any_started = 0;

  all = ptid_equal (ptid, minus_one_ptid);

  if (all || ptid_is_pid (ptid))
    {
      for (tp = thread_list; tp; tp = tp->next)
	{
 	  if (tp->state == THREAD_EXITED)
  	    continue;
	  if (all || ptid_get_pid (ptid) == ptid_get_pid (tp->ptid))
	    {
	      if (set_running_thread (tp, tp->executing))
		any_started = 1;
	    }
	}
    }
  else
    {
      tp = find_thread_ptid (ptid);
      gdb_assert (tp);
      if (tp->state != THREAD_EXITED)
	{
	  if (set_running_thread (tp, tp->executing))
	    any_started = 1;
	}
    }

  if (any_started)
    observer_notify_target_resumed (ptid);
}

void
finish_thread_state_cleanup (void *arg)
{
  ptid_t *ptid_p = arg;

  gdb_assert (arg);

  finish_thread_state (*ptid_p);
}

int
pc_in_thread_step_range (CORE_ADDR pc, struct thread_info *thread)
{
  return (pc >= thread->control.step_range_start
	  && pc < thread->control.step_range_end);
}

/* Prints the list of threads and their details on UIOUT.
   This is a version of 'info_threads_command' suitable for
   use from MI.
   If REQUESTED_THREAD is not -1, it's the GDB id of the thread
   that should be printed.  Otherwise, all threads are
   printed.
   If PID is not -1, only print threads from the process PID.
   Otherwise, threads from all attached PIDs are printed.
   If both REQUESTED_THREAD and PID are not -1, then the thread
   is printed if it belongs to the specified process.  Otherwise,
   an error is raised.  */
void
print_thread_info (struct ui_out *uiout, char *requested_threads, int pid)
{
  struct thread_info *tp;
  ptid_t current_ptid;
  struct cleanup *old_chain;
  char *extra_info, *name, *target_id;
  int current_thread = -1;

  update_thread_list ();
  current_ptid = inferior_ptid;

  /* We'll be switching threads temporarily.  */
  old_chain = make_cleanup_restore_current_thread ();

  /* For backward compatibility, we make a list for MI.  A table is
     preferable for the CLI, though, because it shows table
     headers.  */
  if (ui_out_is_mi_like_p (uiout))
    make_cleanup_ui_out_list_begin_end (uiout, "threads");
  else
    {
      int n_threads = 0;

      for (tp = thread_list; tp; tp = tp->next)
	{
	  if (!number_is_in_list (requested_threads, tp->num))
	    continue;

	  if (pid != -1 && ptid_get_pid (tp->ptid) != pid)
	    continue;

	  if (tp->state == THREAD_EXITED)
	    continue;

	  ++n_threads;
	}

      if (n_threads == 0)
	{
	  if (requested_threads == NULL || *requested_threads == '\0')
	    ui_out_message (uiout, 0, _("No threads.\n"));
	  else
	    ui_out_message (uiout, 0, _("No threads match '%s'.\n"),
			    requested_threads);
	  do_cleanups (old_chain);
	  return;
	}

      make_cleanup_ui_out_table_begin_end (uiout, 4, n_threads, "threads");

      ui_out_table_header (uiout, 1, ui_left, "current", "");
      ui_out_table_header (uiout, 4, ui_left, "id", "Id");
      ui_out_table_header (uiout, 17, ui_left, "target-id", "Target Id");
      ui_out_table_header (uiout, 1, ui_left, "frame", "Frame");
      ui_out_table_body (uiout);
    }

  for (tp = thread_list; tp; tp = tp->next)
    {
      struct cleanup *chain2;
      int core;

      if (!number_is_in_list (requested_threads, tp->num))
	continue;

      if (pid != -1 && ptid_get_pid (tp->ptid) != pid)
	{
	  if (requested_threads != NULL && *requested_threads != '\0')
	    error (_("Requested thread not found in requested process"));
	  continue;
	}

      if (ptid_equal (tp->ptid, current_ptid))
	current_thread = tp->num;

      if (tp->state == THREAD_EXITED)
	continue;

      chain2 = make_cleanup_ui_out_tuple_begin_end (uiout, NULL);

      if (ui_out_is_mi_like_p (uiout))
	{
	  /* Compatibility.  */
	  if (ptid_equal (tp->ptid, current_ptid))
	    ui_out_text (uiout, "* ");
	  else
	    ui_out_text (uiout, "  ");
	}
      else
	{
	  if (ptid_equal (tp->ptid, current_ptid))
	    ui_out_field_string (uiout, "current", "*");
	  else
	    ui_out_field_skip (uiout, "current");
	}

      ui_out_field_int (uiout, "id", tp->num);

      /* For the CLI, we stuff everything into the target-id field.
	 This is a gross hack to make the output come out looking
	 correct.  The underlying problem here is that ui-out has no
	 way to specify that a field's space allocation should be
	 shared by several fields.  For MI, we do the right thing
	 instead.  */

      target_id = target_pid_to_str (tp->ptid);
      extra_info = target_extra_thread_info (tp);
      name = tp->name ? tp->name : target_thread_name (tp);

      if (ui_out_is_mi_like_p (uiout))
	{
	  ui_out_field_string (uiout, "target-id", target_id);
	  if (extra_info)
	    ui_out_field_string (uiout, "details", extra_info);
	  if (name)
	    ui_out_field_string (uiout, "name", name);
	}
      else
	{
	  struct cleanup *str_cleanup;
	  char *contents;

	  if (extra_info && name)
	    contents = xstrprintf ("%s \"%s\" (%s)", target_id,
				   name, extra_info);
	  else if (extra_info)
	    contents = xstrprintf ("%s (%s)", target_id, extra_info);
	  else if (name)
	    contents = xstrprintf ("%s \"%s\"", target_id, name);
	  else
	    contents = xstrdup (target_id);
	  str_cleanup = make_cleanup (xfree, contents);

	  ui_out_field_string (uiout, "target-id", contents);
	  do_cleanups (str_cleanup);
	}

      if (tp->state == THREAD_RUNNING)
	ui_out_text (uiout, "(running)\n");
      else
	{
	  /* The switch below puts us at the top of the stack (leaf
	     frame).  */
	  switch_to_thread (tp->ptid);
	  print_stack_frame (get_selected_frame (NULL),
			     /* For MI output, print frame level.  */
			     ui_out_is_mi_like_p (uiout),
			     LOCATION, 0);
	}

      if (ui_out_is_mi_like_p (uiout))
	{
	  char *state = "stopped";

	  if (tp->state == THREAD_RUNNING)
	    state = "running";
	  ui_out_field_string (uiout, "state", state);
	}

      core = target_core_of_thread (tp->ptid);
      if (ui_out_is_mi_like_p (uiout) && core != -1)
	ui_out_field_int (uiout, "core", core);

      do_cleanups (chain2);
    }

  /* Restores the current thread and the frame selected before
     the "info threads" command.  */
  do_cleanups (old_chain);

  if (pid == -1 && requested_threads == NULL)
    {
      gdb_assert (current_thread != -1
		  || !thread_list
		  || ptid_equal (inferior_ptid, null_ptid));
      if (current_thread != -1 && ui_out_is_mi_like_p (uiout))
	ui_out_field_int (uiout, "current-thread-id", current_thread);

      if (current_thread != -1 && is_exited (current_ptid))
	ui_out_message (uiout, 0, "\n\
The current thread <Thread ID %d> has terminated.  See `help thread'.\n",
			current_thread);
      else if (thread_list
	       && current_thread == -1
	       && ptid_equal (current_ptid, null_ptid))
	ui_out_message (uiout, 0, "\n\
No selected thread.  See `help thread'.\n");
    }
}

/* Print information about currently known threads 

   Optional ARG is a thread id, or list of thread ids.

   Note: this has the drawback that it _really_ switches
         threads, which frees the frame cache.  A no-side
         effects info-threads command would be nicer.  */

static void
info_threads_command (char *arg, int from_tty)
{
  print_thread_info (current_uiout, arg, -1);
}

/* Switch from one thread to another.  */

void
switch_to_thread (ptid_t ptid)
{
  /* Switch the program space as well, if we can infer it from the now
     current thread.  Otherwise, it's up to the caller to select the
     space it wants.  */
  if (!ptid_equal (ptid, null_ptid))
    {
      struct inferior *inf;

      inf = find_inferior_ptid (ptid);
      gdb_assert (inf != NULL);
      set_current_program_space (inf->pspace);
      set_current_inferior (inf);
    }

  if (ptid_equal (ptid, inferior_ptid))
    return;

  inferior_ptid = ptid;
  reinit_frame_cache ();

  /* We don't check for is_stopped, because we're called at times
     while in the TARGET_RUNNING state, e.g., while handling an
     internal event.  */
  if (!ptid_equal (inferior_ptid, null_ptid)
      && !is_exited (ptid)
      && !is_executing (ptid))
    stop_pc = regcache_read_pc (get_thread_regcache (ptid));
  else
    stop_pc = ~(CORE_ADDR) 0;
}

static void
restore_current_thread (ptid_t ptid)
{
  switch_to_thread (ptid);
}

static void
restore_selected_frame (struct frame_id a_frame_id, int frame_level)
{
  struct frame_info *frame = NULL;
  int count;

  /* This means there was no selected frame.  */
  if (frame_level == -1)
    {
      select_frame (NULL);
      return;
    }

  gdb_assert (frame_level >= 0);

  /* Restore by level first, check if the frame id is the same as
     expected.  If that fails, try restoring by frame id.  If that
     fails, nothing to do, just warn the user.  */

  count = frame_level;
  frame = find_relative_frame (get_current_frame (), &count);
  if (count == 0
      && frame != NULL
      /* The frame ids must match - either both valid or both outer_frame_id.
	 The latter case is not failsafe, but since it's highly unlikely
	 the search by level finds the wrong frame, it's 99.9(9)% of
	 the time (for all practical purposes) safe.  */
      && frame_id_eq (get_frame_id (frame), a_frame_id))
    {
      /* Cool, all is fine.  */
      select_frame (frame);
      return;
    }

  frame = frame_find_by_id (a_frame_id);
  if (frame != NULL)
    {
      /* Cool, refound it.  */
      select_frame (frame);
      return;
    }

  /* Nothing else to do, the frame layout really changed.  Select the
     innermost stack frame.  */
  select_frame (get_current_frame ());

  /* Warn the user.  */
  if (frame_level > 0 && !ui_out_is_mi_like_p (current_uiout))
    {
      warning (_("Couldn't restore frame #%d in "
		 "current thread.  Bottom (innermost) frame selected:"),
	       frame_level);
      /* For MI, we should probably have a notification about
	 current frame change.  But this error is not very
	 likely, so don't bother for now.  */
      print_stack_frame (get_selected_frame (NULL), 1, SRC_AND_LOC, 1);
    }
}

/* Data used by the cleanup installed by
   'make_cleanup_restore_current_thread'.  */

struct current_thread_cleanup
{
  /* Next in list of currently installed 'struct
     current_thread_cleanup' cleanups.  See
     'current_thread_cleanup_chain' below.  */
  struct current_thread_cleanup *next;

  ptid_t inferior_ptid;
  struct frame_id selected_frame_id;
  int selected_frame_level;
  int was_stopped;
  int inf_id;
  int was_removable;
};

/* A chain of currently installed 'struct current_thread_cleanup'
   cleanups.  Restoring the previously selected thread looks up the
   old thread in the thread list by ptid.  If the thread changes ptid,
   we need to update the cleanup's thread structure so the look up
   succeeds.  */
static struct current_thread_cleanup *current_thread_cleanup_chain;

/* A thread_ptid_changed observer.  Update all currently installed
   current_thread_cleanup cleanups that want to switch back to
   OLD_PTID to switch back to NEW_PTID instead.  */

static void
restore_current_thread_ptid_changed (ptid_t old_ptid, ptid_t new_ptid)
{
  struct current_thread_cleanup *it;

  for (it = current_thread_cleanup_chain; it != NULL; it = it->next)
    {
      if (ptid_equal (it->inferior_ptid, old_ptid))
	it->inferior_ptid = new_ptid;
    }
}

static void
do_restore_current_thread_cleanup (void *arg)
{
  struct thread_info *tp;
  struct current_thread_cleanup *old = arg;

  tp = find_thread_ptid (old->inferior_ptid);

  /* If the previously selected thread belonged to a process that has
     in the mean time been deleted (due to normal exit, detach, etc.),
     then don't revert back to it, but instead simply drop back to no
     thread selected.  */
  if (tp
      && find_inferior_ptid (tp->ptid) != NULL)
    restore_current_thread (old->inferior_ptid);
  else
    {
      restore_current_thread (null_ptid);
      set_current_inferior (find_inferior_id (old->inf_id));
    }

  /* The running state of the originally selected thread may have
     changed, so we have to recheck it here.  */
  if (!ptid_equal (inferior_ptid, null_ptid)
      && old->was_stopped
      && is_stopped (inferior_ptid)
      && target_has_registers
      && target_has_stack
      && target_has_memory)
    restore_selected_frame (old->selected_frame_id,
			    old->selected_frame_level);
}

static void
restore_current_thread_cleanup_dtor (void *arg)
{
  struct current_thread_cleanup *old = arg;
  struct thread_info *tp;
  struct inferior *inf;

  current_thread_cleanup_chain = current_thread_cleanup_chain->next;

  tp = find_thread_ptid (old->inferior_ptid);
  if (tp)
    tp->refcount--;
  inf = find_inferior_id (old->inf_id);
  if (inf != NULL)
    inf->removable = old->was_removable;
  xfree (old);
}

/* Set the thread reference count.  */

static void
set_thread_refcount (void *data)
{
  int k;
  struct thread_array_cleanup *ta_cleanup = data;

  for (k = 0; k != ta_cleanup->count; k++)
    ta_cleanup->tp_array[k]->refcount--;
}

struct cleanup *
make_cleanup_restore_current_thread (void)
{
  struct thread_info *tp;
  struct frame_info *frame;
  struct current_thread_cleanup *old;

  old = xmalloc (sizeof (struct current_thread_cleanup));
  old->inferior_ptid = inferior_ptid;
  old->inf_id = current_inferior ()->num;
  old->was_removable = current_inferior ()->removable;

  old->next = current_thread_cleanup_chain;
  current_thread_cleanup_chain = old;

  if (!ptid_equal (inferior_ptid, null_ptid))
    {
      old->was_stopped = is_stopped (inferior_ptid);
      if (old->was_stopped
	  && target_has_registers
	  && target_has_stack
	  && target_has_memory)
	{
	  /* When processing internal events, there might not be a
	     selected frame.  If we naively call get_selected_frame
	     here, then we can end up reading debuginfo for the
	     current frame, but we don't generally need the debuginfo
	     at this point.  */
	  frame = get_selected_frame_if_set ();
	}
      else
	frame = NULL;

      old->selected_frame_id = get_frame_id (frame);
      old->selected_frame_level = frame_relative_level (frame);

      tp = find_thread_ptid (inferior_ptid);
      if (tp)
	tp->refcount++;
    }

  current_inferior ()->removable = 0;

  return make_cleanup_dtor (do_restore_current_thread_cleanup, old,
			    restore_current_thread_cleanup_dtor);
}

/* If non-zero tp_array_compar should sort in ascending order, otherwise in
   descending order.  */

static int tp_array_compar_ascending;

/* Sort an array for struct thread_info pointers by their NUM, order is
   determined by TP_ARRAY_COMPAR_ASCENDING.  */

static int
tp_array_compar (const void *ap_voidp, const void *bp_voidp)
{
  const struct thread_info *const *ap = ap_voidp;
  const struct thread_info *const *bp = bp_voidp;

  return ((((*ap)->num > (*bp)->num) - ((*ap)->num < (*bp)->num))
	  * (tp_array_compar_ascending ? +1 : -1));
}

/* Apply a GDB command to a list of threads.  List syntax is a whitespace
   seperated list of numbers, or ranges, or the keyword `all'.  Ranges consist
   of two numbers seperated by a hyphen.  Examples:

   thread apply 1 2 7 4 backtrace       Apply backtrace cmd to threads 1,2,7,4
   thread apply 2-7 9 p foo(1)  Apply p foo(1) cmd to threads 2->7 & 9
   thread apply all p x/i $pc   Apply x/i $pc cmd to all threads.  */

static void
thread_apply_all_command (char *cmd, int from_tty)
{
  struct cleanup *old_chain;
  char *saved_cmd;
  int tc;
  struct thread_array_cleanup ta_cleanup;

  tp_array_compar_ascending = 0;
  if (cmd != NULL
      && check_for_argument (&cmd, "-ascending", strlen ("-ascending")))
    {
      cmd = skip_spaces (cmd);
      tp_array_compar_ascending = 1;
    }

  if (cmd == NULL || *cmd == '\000')
    error (_("Please specify a command following the thread ID list"));

  update_thread_list ();

  old_chain = make_cleanup_restore_current_thread ();

  /* Save a copy of the command in case it is clobbered by
     execute_command.  */
  saved_cmd = xstrdup (cmd);
  make_cleanup (xfree, saved_cmd);

  /* Note this includes exited threads.  */
  tc = thread_count ();
  if (tc != 0)
    {
      struct thread_info **tp_array;
      struct thread_info *tp;
      int i = 0, k;

      /* Save a copy of the thread_list in case we execute detach
         command.  */
      tp_array = xmalloc (sizeof (struct thread_info *) * tc);
      make_cleanup (xfree, tp_array);

      ALL_NON_EXITED_THREADS (tp)
        {
          tp_array[i] = tp;
          tp->refcount++;
          i++;
        }
      /* Because we skipped exited threads, we may end up with fewer
	 threads in the array than the total count of threads.  */
      gdb_assert (i <= tc);

      if (i != 0)
	qsort (tp_array, i, sizeof (*tp_array), tp_array_compar);

      ta_cleanup.tp_array = tp_array;
      ta_cleanup.count = i;
      make_cleanup (set_thread_refcount, &ta_cleanup);

      for (k = 0; k != i; k++)
        if (thread_alive (tp_array[k]))
          {
            switch_to_thread (tp_array[k]->ptid);
            printf_filtered (_("\nThread %d (%s):\n"), 
			     tp_array[k]->num,
			     target_pid_to_str (inferior_ptid));
            execute_command (cmd, from_tty);

            /* Restore exact command used previously.  */
            strcpy (cmd, saved_cmd);
	  }
    }

  do_cleanups (old_chain);
}

static void
thread_apply_command (char *tidlist, int from_tty)
{
  char *cmd;
  struct cleanup *old_chain;
  char *saved_cmd;
  struct get_number_or_range_state state;

  if (tidlist == NULL || *tidlist == '\000')
    error (_("Please specify a thread ID list"));

  for (cmd = tidlist; *cmd != '\000' && !isalpha (*cmd); cmd++);

  if (*cmd == '\000')
    error (_("Please specify a command following the thread ID list"));

  /* Save a copy of the command in case it is clobbered by
     execute_command.  */
  saved_cmd = xstrdup (cmd);
  old_chain = make_cleanup (xfree, saved_cmd);

  init_number_or_range (&state, tidlist);
  while (!state.finished && state.string < cmd)
    {
      struct thread_info *tp;
      int start;

      start = get_number_or_range (&state);

      make_cleanup_restore_current_thread ();

      tp = find_thread_id (start);

      if (!tp)
	warning (_("Unknown thread %d."), start);
      else if (!thread_alive (tp))
	warning (_("Thread %d has terminated."), start);
      else
	{
	  switch_to_thread (tp->ptid);

	  printf_filtered (_("\nThread %d (%s):\n"), tp->num,
			   target_pid_to_str (inferior_ptid));
	  execute_command (cmd, from_tty);

	  /* Restore exact command used previously.  */
	  strcpy (cmd, saved_cmd);
	}
    }

  do_cleanups (old_chain);
}

/* Switch to the specified thread.  Will dispatch off to thread_apply_command
   if prefix of arg is `apply'.  */

void
thread_command (char *tidstr, int from_tty)
{
  if (!tidstr)
    {
      if (ptid_equal (inferior_ptid, null_ptid))
	error (_("No thread selected"));

      if (target_has_stack)
	{
	  if (is_exited (inferior_ptid))
	    printf_filtered (_("[Current thread is %d (%s) (exited)]\n"),
			     pid_to_thread_id (inferior_ptid),
			     target_pid_to_str (inferior_ptid));
	  else
	    printf_filtered (_("[Current thread is %d (%s)]\n"),
			     pid_to_thread_id (inferior_ptid),
			     target_pid_to_str (inferior_ptid));
	}
      else
	error (_("No stack."));
      return;
    }

  gdb_thread_select (current_uiout, tidstr, NULL);
}

/* Implementation of `thread name'.  */

static void
thread_name_command (char *arg, int from_tty)
{
  struct thread_info *info;

  if (ptid_equal (inferior_ptid, null_ptid))
    error (_("No thread selected"));

  arg = skip_spaces (arg);

  info = inferior_thread ();
  xfree (info->name);
  info->name = arg ? xstrdup (arg) : NULL;
}

/* Find thread ids with a name, target pid, or extra info matching ARG.  */

static void
thread_find_command (char *arg, int from_tty)
{
  struct thread_info *tp;
  char *tmp;
  unsigned long match = 0;

  if (arg == NULL || *arg == '\0')
    error (_("Command requires an argument."));

  tmp = re_comp (arg);
  if (tmp != 0)
    error (_("Invalid regexp (%s): %s"), tmp, arg);

  update_thread_list ();
  for (tp = thread_list; tp; tp = tp->next)
    {
      if (tp->name != NULL && re_exec (tp->name))
	{
	  printf_filtered (_("Thread %d has name '%s'\n"),
			   tp->num, tp->name);
	  match++;
	}

      tmp = target_thread_name (tp);
      if (tmp != NULL && re_exec (tmp))
	{
	  printf_filtered (_("Thread %d has target name '%s'\n"),
			   tp->num, tmp);
	  match++;
	}

      tmp = target_pid_to_str (tp->ptid);
      if (tmp != NULL && re_exec (tmp))
	{
	  printf_filtered (_("Thread %d has target id '%s'\n"),
			   tp->num, tmp);
	  match++;
	}

      tmp = target_extra_thread_info (tp);
      if (tmp != NULL && re_exec (tmp))
	{
	  printf_filtered (_("Thread %d has extra info '%s'\n"),
			   tp->num, tmp);
	  match++;
	}
    }
  if (!match)
    printf_filtered (_("No threads match '%s'\n"), arg);
}

/* Print notices when new threads are attached and detached.  */
int print_thread_events = 1;
static void
show_print_thread_events (struct ui_file *file, int from_tty,
                          struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file,
		    _("Printing of thread events is %s.\n"),
                    value);
}

static int
do_captured_thread_select (struct ui_out *uiout, void *tidstr)
{
  int num;
  struct thread_info *tp;
  char* name;

  num = value_as_long (parse_and_eval (tidstr));

  tp = find_thread_id (num);

  if (!tp)
    error (_("Thread ID %d not known."), num);

  if (!thread_alive (tp))
    error (_("Thread ID %d has terminated."), num);

  switch_to_thread (tp->ptid);

  annotate_thread_changed ();

  ui_out_text (uiout, "[Switching to thread ");
  ui_out_field_int (uiout, "new-thread-id", pid_to_thread_id (inferior_ptid));
  name = tp->name ? tp->name : target_thread_name (tp);
  if (name)
    {
      ui_out_text (uiout, " \"");
      ui_out_text (uiout, name);
      ui_out_text (uiout, "\"");
    }
  ui_out_text (uiout, " (");
  ui_out_text (uiout, target_pid_to_str (inferior_ptid));
  ui_out_text (uiout, ")]");

  /* Note that we can't reach this with an exited thread, due to the
     thread_alive check above.  */
  if (tp->state == THREAD_RUNNING)
    ui_out_text (uiout, "(running)\n");
  else
    {
      ui_out_text (uiout, "\n");
      print_stack_frame (get_selected_frame (NULL), 1, SRC_AND_LOC, 1);
    }

  /* Since the current thread may have changed, see if there is any
     exited thread we can now delete.  */
  prune_threads ();

  return GDB_RC_OK;
}

enum gdb_rc
gdb_thread_select (struct ui_out *uiout, char *tidstr, char **error_message)
{
  if (catch_exceptions_with_msg (uiout, do_captured_thread_select, tidstr,
				 error_message, RETURN_MASK_ALL) < 0)
    return GDB_RC_FAIL;
  return GDB_RC_OK;
}

/* Update the 'threads_executing' global based on the threads we know
   about right now.  */

static void
update_threads_executing (void)
{
  struct thread_info *tp;

  threads_executing = 0;
  ALL_NON_EXITED_THREADS (tp)
    {
      if (tp->executing)
	{
	  threads_executing = 1;
	  break;
	}
    }
}

void
update_thread_list (void)
{
  target_update_thread_list ();
  update_threads_executing ();
}

/* Return a new value for the selected thread's id.  Return a value of 0 if
   no thread is selected, or no threads exist.  */

static struct value *
thread_id_make_value (struct gdbarch *gdbarch, struct internalvar *var,
		      void *ignore)
{
  struct thread_info *tp = find_thread_ptid (inferior_ptid);

  return value_from_longest (builtin_type (gdbarch)->builtin_int,
			     (tp ? tp->num : 0));
}

/* Commands with a prefix of `thread'.  */
struct cmd_list_element *thread_cmd_list = NULL;

/* Implementation of `thread' variable.  */

static const struct internalvar_funcs thread_funcs =
{
  thread_id_make_value,
  NULL,
  NULL
};

void
_initialize_thread (void)
{
  static struct cmd_list_element *thread_apply_list = NULL;

  add_info ("threads", info_threads_command, 
	    _("Display currently known threads.\n\
Usage: info threads [ID]...\n\
Optional arguments are thread IDs with spaces between.\n\
If no arguments, all threads are displayed."));

  add_prefix_cmd ("thread", class_run, thread_command, _("\
Use this command to switch between threads.\n\
The new thread ID must be currently known."),
		  &thread_cmd_list, "thread ", 1, &cmdlist);

  add_prefix_cmd ("apply", class_run, thread_apply_command,
		  _("Apply a command to a list of threads."),
		  &thread_apply_list, "thread apply ", 1, &thread_cmd_list);

  add_cmd ("all", class_run, thread_apply_all_command,
	   _("\
Apply a command to all threads.\n\
\n\
Usage: thread apply all [-ascending] <command>\n\
-ascending: Call <command> for all threads in ascending order.\n\
            The default is descending order.\
"),
	   &thread_apply_list);

  add_cmd ("name", class_run, thread_name_command,
	   _("Set the current thread's name.\n\
Usage: thread name [NAME]\n\
If NAME is not given, then any existing name is removed."), &thread_cmd_list);

  add_cmd ("find", class_run, thread_find_command, _("\
Find threads that match a regular expression.\n\
Usage: thread find REGEXP\n\
Will display thread ids whose name, target ID, or extra info matches REGEXP."),
	   &thread_cmd_list);

  add_com_alias ("t", "thread", class_run, 1);

  add_setshow_boolean_cmd ("thread-events", no_class,
         &print_thread_events, _("\
Set printing of thread events (such as thread start and exit)."), _("\
Show printing of thread events (such as thread start and exit)."), NULL,
         NULL,
         show_print_thread_events,
         &setprintlist, &showprintlist);

  create_internalvar_type_lazy ("_thread", &thread_funcs, NULL);

  observer_attach_thread_ptid_changed (restore_current_thread_ptid_changed);
}
