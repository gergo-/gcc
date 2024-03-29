/* Copyright (C) 2013-2018 Free Software Foundation, Inc.

   Contributed by Mentor Embedded.

   This file is part of the GNU Offloading and Multi Processing Library
   (libgomp).

   Libgomp is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   Libgomp is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
   more details.

   Under Section 7 of GPL version 3, you are granted additional
   permissions described in the GCC Runtime Library Exception, version
   3.1, as published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License and
   a copy of the GCC Runtime Library Exception along with this program;
   see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
   <http://www.gnu.org/licenses/>.  */

/* This file handles OpenACC constructs.  */

#include "openacc.h"
#include "libgomp.h"
#include "libgomp_g.h"
#include "gomp-constants.h"
#include "oacc-int.h"
#if USE_LIBFFI
# include "ffi.h"
#endif
#ifdef HAVE_INTTYPES_H
# include <inttypes.h>  /* For PRIu64.  */
#endif
#include <string.h>
#include <stdarg.h>
#include <assert.h>


/* In the ABI, the GOACC_FLAGs are encoded as an inverted bitmask, so that we
   continue to support the following two legacy values.  */
_Static_assert (GOACC_FLAGS_UNMARSHAL (GOMP_DEVICE_ICV) == 0,
		"legacy GOMP_DEVICE_ICV broken");
_Static_assert (GOACC_FLAGS_UNMARSHAL (GOMP_DEVICE_HOST_FALLBACK)
		== GOACC_FLAG_HOST_FALLBACK,
		"legacy GOMP_DEVICE_HOST_FALLBACK broken");


/* Returns the number of mappings associated with the pointer or pset. PSET
   have three mappings, whereas pointer have two.  */

static int
find_pointer (int pos, size_t mapnum, unsigned short *kinds)
{
  if (pos + 1 >= mapnum)
    return 0;

  unsigned char kind0 = kinds[pos] & 0xff;

  switch (kind0)
    {
    case GOMP_MAP_TO:
    case GOMP_MAP_FORCE_TO:
    case GOMP_MAP_FROM:
    case GOMP_MAP_FORCE_FROM:
    case GOMP_MAP_TOFROM:
    case GOMP_MAP_FORCE_TOFROM:
    case GOMP_MAP_ALLOC:
    case GOMP_MAP_RELEASE:
    case GOMP_MAP_DECLARE_ALLOCATE:
    case GOMP_MAP_DECLARE_DEALLOCATE:
      {
	unsigned char kind1 = kinds[pos + 1] & 0xff;
	if (kind1 == GOMP_MAP_POINTER
	    || kind1 == GOMP_MAP_ALWAYS_POINTER
	    || kind1 == GOMP_MAP_ATTACH
	    || kind1 == GOMP_MAP_DETACH
	    || kind1 == GOMP_MAP_FORCE_DETACH)
	  return 2;
	else if (kind1 == GOMP_MAP_TO_PSET)
	  return 3;
      }
    default:
      /* empty.  */;
    }

  return 0;
}

/* Handle the mapping pair that are presented when a
   deviceptr clause is used with Fortran.  */

static void
handle_ftn_pointers (size_t mapnum, void **hostaddrs, size_t *sizes,
		     unsigned short *kinds)
{
  int i;

  for (i = 0; i < mapnum; i++)
    {
      unsigned short kind1 = kinds[i] & 0xff;

      /* Handle Fortran deviceptr clause.  */
      if (kind1 == GOMP_MAP_FORCE_DEVICEPTR)
	{
	  unsigned short kind2;

	  if (i < (signed)mapnum - 1)
	    kind2 = kinds[i + 1] & 0xff;
	  else
	    kind2 = 0xffff;

	  if (sizes[i] == sizeof (void *))
	    continue;

	  /* At this point, we're dealing with a Fortran deviceptr.
	     If the next element is not what we're expecting, then
	     this is an instance of where the deviceptr variable was
	     not used within the region and the pointer was removed
	     by the gimplifier.  */
	  if (kind2 == GOMP_MAP_POINTER
	      && sizes[i + 1] == 0
	      && hostaddrs[i] == *(void **)hostaddrs[i + 1])
	    {
	      kinds[i+1] = kinds[i];
	      sizes[i+1] = sizeof (void *);
	    }

	  /* Invalidate the entry.  */
	  hostaddrs[i] = NULL;
	}
    }
}

static void goacc_wait (int async, int num_waits, va_list *ap);

static void
goacc_call_host_fn (void (*fn) (void *), size_t mapnum, void **hostaddrs,
		    int params)
{
#ifdef USE_LIBFFI
  ffi_cif cif;
  ffi_type *arg_types[mapnum];
  void *arg_values[mapnum];
  ffi_arg result;
  int i;

  if (params)
    {
      for (i = 0; i < mapnum; i++)
	{
	  arg_types[i] = &ffi_type_pointer;
	  arg_values[i] = &hostaddrs[i];
	}

      if (ffi_prep_cif (&cif, FFI_DEFAULT_ABI, mapnum,
			&ffi_type_void, arg_types) == FFI_OK)
	ffi_call (&cif, FFI_FN (fn), &result, arg_values);
      else
	abort ();
    }
  else
#endif
  fn (hostaddrs);
}

/* Launch a possibly offloaded function with FLAGS.  FN is the host fn
   address.  MAPNUM, HOSTADDRS, SIZES & KINDS  describe the memory
   blocks to be copied to/from the device.  Varadic arguments are
   keyed optional parameters terminated with a zero.  */

static void
GOACC_parallel_keyed_internal (int flags_m, int params, void (*fn) (void *),
			       size_t mapnum, void **hostaddrs, size_t *sizes,
			       unsigned short *kinds, va_list *ap)
{
  int flags = GOACC_FLAGS_UNMARSHAL (flags_m);

  struct goacc_thread *thr;
  struct gomp_device_descr *acc_dev;
  struct target_mem_desc *tgt;
  void **devaddrs;
  unsigned int i;
  struct splay_tree_key_s k;
  splay_tree_key tgt_fn_key;
  void (*tgt_fn);
  int async = GOMP_ASYNC_SYNC;
  unsigned dims[GOMP_DIM_MAX];
  unsigned tag;

#ifdef HAVE_INTTYPES_H
  gomp_debug (0, "%s: mapnum=%"PRIu64", hostaddrs=%p, size=%p, kinds=%p\n",
	      __FUNCTION__, (uint64_t) mapnum, hostaddrs, sizes, kinds);
#else
  gomp_debug (0, "%s: mapnum=%lu, hostaddrs=%p, sizes=%p, kinds=%p\n",
	      __FUNCTION__, (unsigned long) mapnum, hostaddrs, sizes, kinds);
#endif
  goacc_lazy_initialize ();

  thr = goacc_thread ();
  acc_dev = thr->dev;

  bool profiling_dispatch_p
    = __builtin_expect (goacc_profiling_dispatch_p (true), false);

  acc_prof_info prof_info;
  if (profiling_dispatch_p)
    {
      thr->prof_info = &prof_info;

      prof_info.event_type = acc_ev_compute_construct_start;
      prof_info.valid_bytes = _ACC_PROF_INFO_VALID_BYTES;
      prof_info.version = _ACC_PROF_INFO_VERSION;
      prof_info.device_type = acc_device_type (acc_dev->type);
      prof_info.device_number = acc_dev->target_id;
      prof_info.thread_id = -1; //TODO
      prof_info.async = async;
      /* See <https://github.com/OpenACC/openacc-spec/issues/71>.  */
      prof_info.async_queue = prof_info.async;
      prof_info.src_file = NULL; //TODO
      prof_info.func_name = NULL; //TODO
      prof_info.line_no = -1; //TODO
      prof_info.end_line_no = -1; //TODO
      prof_info.func_line_no = -1; //TODO
      prof_info.func_end_line_no = -1; //TODO
    }
  acc_event_info compute_construct_event_info;
  if (profiling_dispatch_p)
    {
      compute_construct_event_info.other_event.event_type
	= prof_info.event_type;
      compute_construct_event_info.other_event.valid_bytes
	= _ACC_OTHER_EVENT_INFO_VALID_BYTES;
      compute_construct_event_info.other_event.parent_construct
	= acc_construct_parallel; //TODO: kernels...
      compute_construct_event_info.other_event.implicit = 0;
      compute_construct_event_info.other_event.tool_info = NULL;
    }
  acc_api_info api_info;
  if (profiling_dispatch_p)
    {
      thr->api_info = &api_info;

      api_info.device_api = acc_device_api_none;
      api_info.valid_bytes = _ACC_API_INFO_VALID_BYTES;
      api_info.device_type = prof_info.device_type;
      api_info.vendor = -1; //TODO
      api_info.device_handle = NULL; //TODO
      api_info.context_handle = NULL; //TODO
      api_info.async_handle = NULL; //TODO
    }

  if (profiling_dispatch_p)
    goacc_profiling_dispatch (&prof_info, &compute_construct_event_info,
			      &api_info);

  handle_ftn_pointers (mapnum, hostaddrs, sizes, kinds);

  /* Host fallback if "if" clause is false or if the current device is set to
     the host.  */
  if (flags & GOACC_FLAG_HOST_FALLBACK)
    {
      //TODO
      prof_info.device_type = acc_device_host;
      api_info.device_type = prof_info.device_type;
      goacc_save_and_set_bind (acc_device_host);
      goacc_call_host_fn (fn, mapnum, hostaddrs, params);
      goacc_restore_bind ();
      goto out;
    }
  else if (acc_device_type (acc_dev->type) == acc_device_host)
    {
      goacc_call_host_fn (fn, mapnum, hostaddrs, params);
      goto out;
    }
  else if (profiling_dispatch_p)
    api_info.device_api = acc_device_api_cuda;

  /* Default: let the runtime choose.  */
  for (i = 0; i != GOMP_DIM_MAX; i++)
    dims[i] = 0;

  /* TODO: This will need amending when device_type is implemented.  */
  while ((tag = va_arg (*ap, unsigned)) != 0)
    {
      if (GOMP_LAUNCH_DEVICE (tag))
	gomp_fatal ("device_type '%d' offload parameters, libgomp is too old",
		    GOMP_LAUNCH_DEVICE (tag));

      switch (GOMP_LAUNCH_CODE (tag))
	{
	case GOMP_LAUNCH_DIM:
	  {
	    unsigned mask = GOMP_LAUNCH_OP (tag);

	    for (i = 0; i != GOMP_DIM_MAX; i++)
	      if (mask & GOMP_DIM_MASK (i))
		dims[i] = va_arg (*ap, unsigned);
	  }
	  break;

	case GOMP_LAUNCH_ASYNC:
	  {
	    /* Small constant values are encoded in the operand.  */
	    async = GOMP_LAUNCH_OP (tag);

	    if (async == GOMP_LAUNCH_OP_MAX)
	      async = va_arg (*ap, unsigned);

	    if (profiling_dispatch_p)
	      {
		prof_info.async = async;
		/* See <https://github.com/OpenACC/openacc-spec/issues/71>.  */
		prof_info.async_queue = prof_info.async;
	      }

	    break;
	  }

	case GOMP_LAUNCH_WAIT:
	  {
	    /* Be careful to cast the op field as a signed 16-bit, and
	       sign-extend to full integer.  */
	    int num_waits = ((signed short) GOMP_LAUNCH_OP (tag));

	    if (num_waits > 0)
	      goacc_wait (async, num_waits, ap);
	    else if (num_waits == acc_async_noval)
	      acc_wait_all_async (async);
	    break;
	  }

	default:
	  gomp_fatal ("unrecognized offload code '%d',"
		      " libgomp is too old", GOMP_LAUNCH_CODE (tag));
	}
    }
  
  if (!(acc_dev->capabilities & GOMP_OFFLOAD_CAP_NATIVE_EXEC))
    {
      k.host_start = (uintptr_t) fn;
      k.host_end = k.host_start + 1;
      gomp_mutex_lock (&acc_dev->lock);
      tgt_fn_key = splay_tree_lookup (&acc_dev->mem_map, &k);
      gomp_mutex_unlock (&acc_dev->lock);

      if (tgt_fn_key == NULL)
	gomp_fatal ("target function wasn't mapped");

      tgt_fn = (void (*)) tgt_fn_key->tgt_offset;
    }
  else
    tgt_fn = (void (*)) fn;

  acc_event_info enter_exit_data_event_info;
  if (profiling_dispatch_p)
    {
      prof_info.event_type = acc_ev_enter_data_start;
      enter_exit_data_event_info.other_event.event_type
	= prof_info.event_type;
      enter_exit_data_event_info.other_event.valid_bytes
	= _ACC_OTHER_EVENT_INFO_VALID_BYTES;
      enter_exit_data_event_info.other_event.parent_construct
	= compute_construct_event_info.other_event.parent_construct;
      enter_exit_data_event_info.other_event.implicit = 1;
      enter_exit_data_event_info.other_event.tool_info = NULL;
      goacc_profiling_dispatch (&prof_info, &enter_exit_data_event_info,
				&api_info);
    }

  goacc_aq aq = get_goacc_asyncqueue (async);

  tgt = gomp_map_vars_async (acc_dev, aq, mapnum, hostaddrs, NULL, sizes, kinds,
			     true, GOMP_MAP_VARS_OPENACC);
  if (profiling_dispatch_p)
    {
      prof_info.event_type = acc_ev_enter_data_end;
      enter_exit_data_event_info.other_event.event_type
	= prof_info.event_type;
      goacc_profiling_dispatch (&prof_info, &enter_exit_data_event_info,
				&api_info);
    }

#ifdef RC_CHECKING
  gomp_mutex_lock (&acc_dev->lock);
  assert (tgt);
  dump_tgt (__FUNCTION__, tgt);
  tgt->prev = thr->mapped_data;
  gomp_rc_check (acc_dev, tgt);
  gomp_mutex_unlock (&acc_dev->lock);
#endif

  devaddrs = gomp_alloca (sizeof (void *) * mapnum);
  for (i = 0; i < mapnum; i++)
    devaddrs[i] = (void *) gomp_map_val (tgt, hostaddrs, i);

  if (aq == NULL)
    {
      if (params)
	acc_dev->openacc.exec_params_func (tgt_fn, mapnum, hostaddrs, devaddrs,
					   dims, tgt);
      else
	acc_dev->openacc.exec_func (tgt_fn, mapnum, hostaddrs, devaddrs,
				    dims, tgt);
      if (profiling_dispatch_p)
	{
	  prof_info.event_type = acc_ev_exit_data_start;
	  enter_exit_data_event_info.other_event.event_type
	    = prof_info.event_type;
	  enter_exit_data_event_info.other_event.tool_info = NULL;
	  goacc_profiling_dispatch (&prof_info, &enter_exit_data_event_info,
				    &api_info);
	}
      /* If running synchronously, unmap immediately.  */
      gomp_unmap_vars (tgt, true);
      if (profiling_dispatch_p)
	{
	  prof_info.event_type = acc_ev_exit_data_end;
	  enter_exit_data_event_info.other_event.event_type
	    = prof_info.event_type;
	  goacc_profiling_dispatch (&prof_info, &enter_exit_data_event_info,
				    &api_info);
	}
    }
  else
    {
      if (params)
	acc_dev->openacc.async.exec_params_func (tgt_fn, mapnum, hostaddrs,
						 devaddrs, dims, tgt, aq);
      else
	acc_dev->openacc.async.exec_func (tgt_fn, mapnum, hostaddrs,
					  devaddrs, dims, tgt, aq);
      goacc_async_copyout_unmap_vars (tgt, aq);
    }

#ifdef RC_CHECKING
  gomp_mutex_lock (&acc_dev->lock);
  gomp_rc_check (acc_dev, thr->mapped_data);
  gomp_mutex_unlock (&acc_dev->lock);
#endif

 out:
  if (profiling_dispatch_p)
    {
      prof_info.event_type = acc_ev_compute_construct_end;
      compute_construct_event_info.other_event.event_type
	= prof_info.event_type;
      goacc_profiling_dispatch (&prof_info, &compute_construct_event_info,
				&api_info);

      thr->prof_info = NULL;
      thr->api_info = NULL;
    }
}

void
GOACC_parallel_keyed (int flags_m, void (*fn) (void *),
		      size_t mapnum, void **hostaddrs, size_t *sizes,
		      unsigned short *kinds, ...)
{
  va_list ap;
  va_start (ap, kinds);
  GOACC_parallel_keyed_internal (flags_m, 0, fn, mapnum, hostaddrs, sizes,
				 kinds, &ap);
  va_end (ap);
}

void
GOACC_parallel_keyed_v2 (int flags_m, int args, void (*fn) (void *),
			 size_t mapnum, void **hostaddrs, size_t *sizes,
			 unsigned short *kinds, ...)
{
  va_list ap;
  va_start (ap, kinds);
  GOACC_parallel_keyed_internal (flags_m, args, fn, mapnum, hostaddrs, sizes,
				 kinds, &ap);
  va_end (ap);
}

/* Legacy entry point, only provide host execution.  */

void
GOACC_parallel (int flags_m, void (*fn) (void *),
		size_t mapnum, void **hostaddrs, size_t *sizes,
		unsigned short *kinds,
		int num_gangs, int num_workers, int vector_length,
		int async, int num_waits, ...)
{
  goacc_save_and_set_bind (acc_device_host);
  fn (hostaddrs);
  goacc_restore_bind ();
}

void
GOACC_data_start (int flags_m, size_t mapnum,
		  void **hostaddrs, size_t *sizes, unsigned short *kinds)
{
  int flags = GOACC_FLAGS_UNMARSHAL (flags_m);

  struct target_mem_desc *tgt;

#ifdef HAVE_INTTYPES_H
  gomp_debug (0, "%s: mapnum=%"PRIu64", hostaddrs=%p, size=%p, kinds=%p\n",
	      __FUNCTION__, (uint64_t) mapnum, hostaddrs, sizes, kinds);
#else
  gomp_debug (0, "%s: mapnum=%lu, hostaddrs=%p, sizes=%p, kinds=%p\n",
	      __FUNCTION__, (unsigned long) mapnum, hostaddrs, sizes, kinds);
#endif

  goacc_lazy_initialize ();

  struct goacc_thread *thr = goacc_thread ();
  struct gomp_device_descr *acc_dev = thr->dev;

  bool profiling_dispatch_p
    = __builtin_expect (goacc_profiling_dispatch_p (true), false);

  acc_prof_info prof_info;
  if (profiling_dispatch_p)
    {
      thr->prof_info = &prof_info;

      prof_info.event_type = acc_ev_enter_data_start;
      prof_info.valid_bytes = _ACC_PROF_INFO_VALID_BYTES;
      prof_info.version = _ACC_PROF_INFO_VERSION;
      prof_info.device_type = acc_device_type (acc_dev->type);
      prof_info.device_number = acc_dev->target_id;
      prof_info.thread_id = -1; //TODO
      prof_info.async = acc_async_sync; /* Always synchronous.  */
      /* See <https://github.com/OpenACC/openacc-spec/issues/71>.  */
      prof_info.async_queue = prof_info.async;
      prof_info.src_file = NULL; //TODO
      prof_info.func_name = NULL; //TODO
      prof_info.line_no = -1; //TODO
      prof_info.end_line_no = -1; //TODO
      prof_info.func_line_no = -1; //TODO
      prof_info.func_end_line_no = -1; //TODO
    }
  acc_event_info enter_data_event_info;
  if (profiling_dispatch_p)
    {
      enter_data_event_info.other_event.event_type
	= prof_info.event_type;
      enter_data_event_info.other_event.valid_bytes
	= _ACC_OTHER_EVENT_INFO_VALID_BYTES;
      enter_data_event_info.other_event.parent_construct = acc_construct_data;
      for (int i = 0; i < mapnum; ++i)
	if (kinds[i] == GOMP_MAP_USE_DEVICE_PTR)
	  {
	    /* If there is one such data mapping kind, then this is actually an
	       OpenACC host_data construct.  (GCC maps the OpenACC host_data
	       construct to the OpenACC data construct.)  Apart from artificial
	       test cases (such as an OpenACC host_data construct's (implicit)
	       device initialization when there hasn't been any device data be
	       set up before...), there can't really any meaningful events be
	       generated from OpenACC host_data constructs, though.  */
	    enter_data_event_info.other_event.parent_construct
	      = acc_construct_host_data;
	    break;
	  }
      enter_data_event_info.other_event.implicit = 0;
      enter_data_event_info.other_event.tool_info = NULL;
    }
  acc_api_info api_info;
  if (profiling_dispatch_p)
    {
      thr->api_info = &api_info;

      api_info.device_api = acc_device_api_none;
      api_info.valid_bytes = _ACC_API_INFO_VALID_BYTES;
      api_info.device_type = prof_info.device_type;
      api_info.vendor = -1; //TODO
      api_info.device_handle = NULL; //TODO
      api_info.context_handle = NULL; //TODO
      api_info.async_handle = NULL; //TODO
    }

  if (profiling_dispatch_p)
    goacc_profiling_dispatch (&prof_info, &enter_data_event_info, &api_info);

  handle_ftn_pointers (mapnum, hostaddrs, sizes, kinds);

  enum gomp_map_vars_kind pragma_kind;
  if (flags & GOACC_FLAG_HOST_DATA_IF_PRESENT)
    pragma_kind = GOMP_MAP_VARS_OPENACC_IF_PRESENT;
  else
    pragma_kind = GOMP_MAP_VARS_OPENACC;

  /* Host fallback or 'do nothing'.  */
  if ((acc_dev->capabilities & GOMP_OFFLOAD_CAP_SHARED_MEM)
      || (flags & GOACC_FLAG_HOST_FALLBACK))
    {
      //TODO
      prof_info.device_type = acc_device_host;
      api_info.device_type = prof_info.device_type;
      tgt = gomp_map_vars (NULL, 0, NULL, NULL, NULL, NULL, true, pragma_kind);
      tgt->prev = thr->mapped_data;
      thr->mapped_data = tgt;
      goto out;
    }

  gomp_debug (0, "  %s: prepare mappings\n", __FUNCTION__);
  tgt = gomp_map_vars (acc_dev, mapnum, hostaddrs, NULL, sizes, kinds, true,
		       pragma_kind);
  gomp_debug (0, "  %s: mappings prepared\n", __FUNCTION__);
  tgt->prev = thr->mapped_data;
  thr->mapped_data = tgt;

 out:
  if (profiling_dispatch_p)
    {
      prof_info.event_type = acc_ev_enter_data_end;
      enter_data_event_info.other_event.event_type = prof_info.event_type;
      goacc_profiling_dispatch (&prof_info, &enter_data_event_info, &api_info);

      thr->prof_info = NULL;
      thr->api_info = NULL;
    }

#ifdef RC_CHECKING
  gomp_mutex_lock (&acc_dev->lock);
  gomp_rc_check (acc_dev, thr->mapped_data);
  gomp_mutex_unlock (&acc_dev->lock);
#endif
}

void
GOACC_data_end (void)
{
  struct goacc_thread *thr = goacc_thread ();
  struct gomp_device_descr *acc_dev = thr->dev;
  struct target_mem_desc *tgt = thr->mapped_data;

  bool profiling_dispatch_p
    = __builtin_expect (goacc_profiling_dispatch_p (true), false);

  acc_prof_info prof_info;
  if (profiling_dispatch_p)
    {
      thr->prof_info = &prof_info;

      prof_info.event_type = acc_ev_exit_data_start;
      prof_info.valid_bytes = _ACC_PROF_INFO_VALID_BYTES;
      prof_info.version = _ACC_PROF_INFO_VERSION;
      prof_info.device_type = acc_device_type (acc_dev->type);
      prof_info.device_number = acc_dev->target_id;
      prof_info.thread_id = -1; //TODO
      prof_info.async = acc_async_sync; /* Always synchronous.  */
      /* See <https://github.com/OpenACC/openacc-spec/issues/71>.  */
      prof_info.async_queue = prof_info.async;
      prof_info.src_file = NULL; //TODO
      prof_info.func_name = NULL; //TODO
      prof_info.line_no = -1; //TODO
      prof_info.end_line_no = -1; //TODO
      prof_info.func_line_no = -1; //TODO
      prof_info.func_end_line_no = -1; //TODO
    }
  acc_event_info exit_data_event_info;
  if (profiling_dispatch_p)
    {
      exit_data_event_info.other_event.event_type
	= prof_info.event_type;
      exit_data_event_info.other_event.valid_bytes
	= _ACC_OTHER_EVENT_INFO_VALID_BYTES;
      exit_data_event_info.other_event.parent_construct = acc_construct_data;
      exit_data_event_info.other_event.implicit = 0;
      exit_data_event_info.other_event.tool_info = NULL;
    }
  acc_api_info api_info;
  if (profiling_dispatch_p)
    {
      thr->api_info = &api_info;

      api_info.device_api = acc_device_api_none;
      api_info.valid_bytes = _ACC_API_INFO_VALID_BYTES;
      api_info.device_type = prof_info.device_type;
      api_info.vendor = -1; //TODO
      api_info.device_handle = NULL; //TODO
      api_info.context_handle = NULL; //TODO
      api_info.async_handle = NULL; //TODO
    }

  if (profiling_dispatch_p)
    goacc_profiling_dispatch (&prof_info, &exit_data_event_info, &api_info);

  gomp_debug (0, "  %s: restore mappings\n", __FUNCTION__);
  thr->mapped_data = tgt->prev;
  gomp_unmap_vars (tgt, true);
  gomp_debug (0, "  %s: mappings restored\n", __FUNCTION__);

  if (profiling_dispatch_p)
    {
      prof_info.event_type = acc_ev_exit_data_end;
      exit_data_event_info.other_event.event_type = prof_info.event_type;
      goacc_profiling_dispatch (&prof_info, &exit_data_event_info, &api_info);

      thr->prof_info = NULL;
      thr->api_info = NULL;
    }

#ifdef RC_CHECKING
  gomp_mutex_lock (&acc_dev->lock);
  gomp_rc_check (acc_dev, thr->mapped_data);
  gomp_mutex_unlock (&acc_dev->lock);
#endif
}

void
GOACC_enter_exit_data (int flags_m, size_t mapnum,
		       void **hostaddrs, size_t *sizes, unsigned short *kinds,
		       int async, int num_waits, ...)
{
  int flags = GOACC_FLAGS_UNMARSHAL (flags_m);

  struct goacc_thread *thr;
  struct gomp_device_descr *acc_dev;
  bool data_enter = false;
  size_t i;

  /* Determine whether "finalize" semantics apply to all mappings of this
     OpenACC directive.  */
  bool finalize = false;
  if (mapnum > 0)
    {
      unsigned char kind = kinds[0] & 0xff;

      if (kind == GOMP_MAP_STRUCT || kind == GOMP_MAP_FORCE_PRESENT)
        kind = kinds[1] & 0xff;

      if (kind == GOMP_MAP_DELETE
	  || kind == GOMP_MAP_FORCE_FROM)
	finalize = true;
    }

  /* Determine if this is an "acc enter data".  */
  for (i = 0; i < mapnum; ++i)
    {
      unsigned char kind = kinds[i] & 0xff;

      if (kind == GOMP_MAP_POINTER
	  || kind == GOMP_MAP_TO_PSET
	  || kind == GOMP_MAP_STRUCT
	  || kind == GOMP_MAP_FORCE_PRESENT)
	continue;

      if (kind == GOMP_MAP_FORCE_ALLOC
	  || kind == GOMP_MAP_ATTACH
	  || kind == GOMP_MAP_FORCE_TO
	  || kind == GOMP_MAP_TO
	  || kind == GOMP_MAP_ALLOC
	  || kind == GOMP_MAP_DECLARE_ALLOCATE)
	{
	  data_enter = true;
	  break;
	}

      if (kind == GOMP_MAP_RELEASE
	  || kind == GOMP_MAP_DELETE
	  || kind == GOMP_MAP_DETACH
	  || kind == GOMP_MAP_FORCE_DETACH
	  || kind == GOMP_MAP_FROM
	  || kind == GOMP_MAP_FORCE_FROM
	  || kind == GOMP_MAP_DECLARE_DEALLOCATE)
	break;

      gomp_fatal (">>>> GOACC_enter_exit_data UNHANDLED kind 0x%.2x",
		      kind);
    }

  goacc_lazy_initialize ();

  thr = goacc_thread ();
  acc_dev = thr->dev;

  bool profiling_dispatch_p
    = __builtin_expect (goacc_profiling_dispatch_p (true), false);

  acc_prof_info prof_info;
  if (profiling_dispatch_p)
    {
      thr->prof_info = &prof_info;

      prof_info.event_type
	= data_enter ? acc_ev_enter_data_start : acc_ev_exit_data_start;
      prof_info.valid_bytes = _ACC_PROF_INFO_VALID_BYTES;
      prof_info.version = _ACC_PROF_INFO_VERSION;
      prof_info.device_type = acc_device_type (acc_dev->type);
      prof_info.device_number = acc_dev->target_id;
      prof_info.thread_id = -1; //TODO
      prof_info.async = async;
      /* See <https://github.com/OpenACC/openacc-spec/issues/71>.  */
      prof_info.async_queue = prof_info.async;
      prof_info.src_file = NULL; //TODO
      prof_info.func_name = NULL; //TODO
      prof_info.line_no = -1; //TODO
      prof_info.end_line_no = -1; //TODO
      prof_info.func_line_no = -1; //TODO
      prof_info.func_end_line_no = -1; //TODO
    }
  acc_event_info enter_exit_data_event_info;
  if (profiling_dispatch_p)
    {
      enter_exit_data_event_info.other_event.event_type
	= prof_info.event_type;
      enter_exit_data_event_info.other_event.valid_bytes
	= _ACC_OTHER_EVENT_INFO_VALID_BYTES;
      enter_exit_data_event_info.other_event.parent_construct
	= data_enter ? acc_construct_enter_data : acc_construct_exit_data;
      enter_exit_data_event_info.other_event.implicit = 0;
      enter_exit_data_event_info.other_event.tool_info = NULL;
    }
  acc_api_info api_info;
  if (profiling_dispatch_p)
    {
      thr->api_info = &api_info;

      api_info.device_api = acc_device_api_none;
      api_info.valid_bytes = _ACC_API_INFO_VALID_BYTES;
      api_info.device_type = prof_info.device_type;
      api_info.vendor = -1; //TODO
      api_info.device_handle = NULL; //TODO
      api_info.context_handle = NULL; //TODO
      api_info.async_handle = NULL; //TODO
    }

  if (profiling_dispatch_p)
    goacc_profiling_dispatch (&prof_info, &enter_exit_data_event_info,
			      &api_info);

  if ((acc_dev->capabilities & GOMP_OFFLOAD_CAP_SHARED_MEM)
      || (flags & GOACC_FLAG_HOST_FALLBACK))
    {
      //TODO
      prof_info.device_type = acc_device_host;
      api_info.device_type = prof_info.device_type;
      goto out;
    }

  if (num_waits > 0)
    {
      va_list ap;

      va_start (ap, num_waits);
      goacc_wait (async, num_waits, &ap);
      va_end (ap);
    }
  else if (num_waits == acc_async_noval)
    acc_wait_all_async (async);

  /* In c, non-pointers and arrays are represented by a single data clause.
     Dynamically allocated arrays and subarrays are represented by a data
     clause followed by an internal GOMP_MAP_POINTER.

     In fortran, scalars and not allocated arrays are represented by a
     single data clause. Allocated arrays and subarrays have three mappings:
     1) the original data clause, 2) a PSET 3) a pointer to the array data.
  */

  if (data_enter)
    {
      for (i = 0; i < mapnum; i++)
	{
	  unsigned char kind = kinds[i] & 0xff;

	  /* Scan for pointers and PSETs.  */
	  int pointer = find_pointer (i, mapnum, kinds);

	  if (!pointer)
	    {
	      switch (kind)
		{
		case GOMP_MAP_DECLARE_ALLOCATE:
		case GOMP_MAP_ALLOC:
		  acc_present_or_create (hostaddrs[i], sizes[i]);
		  break;
		case GOMP_MAP_ATTACH:
		case GOMP_MAP_FORCE_PRESENT:
		  break;
		case GOMP_MAP_FORCE_ALLOC:
		  acc_create (hostaddrs[i], sizes[i]);
		  break;
		case GOMP_MAP_TO:
		  acc_present_or_copyin (hostaddrs[i], sizes[i]);
		  break;
		case GOMP_MAP_FORCE_TO:
		  acc_copyin (hostaddrs[i], sizes[i]);
		  break;
		case GOMP_MAP_STRUCT:
		  {
		    int elems = sizes[i];
		    goacc_aq aq = get_goacc_asyncqueue (async);
		    gomp_map_vars_async (acc_dev, aq, elems + 1, &hostaddrs[i],
					 NULL, &sizes[i], &kinds[i], true,
					 GOMP_MAP_VARS_OPENACC_ENTER_DATA);
		    i += elems;
		  }
		  break;
		default:
		  gomp_fatal (">>>> GOACC_enter_exit_data UNHANDLED kind 0x%.2x",
			      kind);
		  break;
		}
	    }
	  else
	    {
	      if (kind == GOMP_MAP_DECLARE_ALLOCATE)
		gomp_acc_declare_allocate (true, pointer, &hostaddrs[i],
					   &sizes[i], &kinds[i]);
	      else
	        {
		  goacc_aq aq = get_goacc_asyncqueue (async);
	          for (int j = 0; j < 2; j++)
		    gomp_map_vars_async (acc_dev, aq,
					 (j == 0 || pointer == 2) ? 1 : 2,
					 &hostaddrs[i + j], NULL,
					 &sizes[i + j], &kinds[i + j], true,
					 GOMP_MAP_VARS_OPENACC_ENTER_DATA);
		}
	      /* Increment 'i' by two because OpenACC requires fortran
		 arrays to be contiguous, so each PSET is associated with
		 one of MAP_FORCE_ALLOC/MAP_FORCE_PRESET/MAP_FORCE_TO, and
		 one MAP_POINTER.  */
	      i += pointer - 1;
	    }
	}

      /* This loop only handles explicit "attach" clauses that are not an
	 implicit part of a copy{,in,out}, etc. mapping.  */
      for (i = 0; i < mapnum; i++)
        {
	  unsigned char kind = kinds[i] & 0xff;

	  /* Scan for pointers and PSETs.  */
	  int pointer = find_pointer (i, mapnum, kinds);

	  if (!pointer)
	    {
	      if (kind == GOMP_MAP_ATTACH)
		acc_attach (hostaddrs[i]);
	      else if (kind == GOMP_MAP_STRUCT)
	        i += sizes[i];
	    }
	  else
	    i += pointer - 1;
	}
    }
  else
    {
      /* Handle "detach" before copyback/deletion of mapped data.  */
      for (i = 0; i < mapnum; i++)
        {
	  unsigned char kind = kinds[i] & 0xff;

	  int pointer = find_pointer (i, mapnum, kinds);

	  if (!pointer)
	    {
	      if (kind == GOMP_MAP_DETACH)
		acc_detach (hostaddrs[i]);
	      else if (kind == GOMP_MAP_FORCE_DETACH)
		acc_detach_finalize (hostaddrs[i]);
	      else if (kind == GOMP_MAP_STRUCT)
	        i += sizes[i];
	    }
	  else
	    {
	      unsigned char kind2 = kinds[i + pointer - 1] & 0xff;

	      if (kind2 == GOMP_MAP_DETACH)
		acc_detach (hostaddrs[i + pointer - 1]);
	      else if (kind2 == GOMP_MAP_FORCE_DETACH)
	        acc_detach_finalize (hostaddrs[i + pointer - 1]);

	      i += pointer - 1;
	    }
	}

      for (i = 0; i < mapnum; ++i)
	{
	  unsigned char kind = kinds[i] & 0xff;

	  int pointer = find_pointer (i, mapnum, kinds);

	  if (!pointer)
	    switch (kind)
	      {
	      case GOMP_MAP_RELEASE:
	      case GOMP_MAP_DELETE:
		if (acc_is_present (hostaddrs[i], sizes[i]))
		  {
		    if (finalize)
		      acc_delete_finalize_async (hostaddrs[i], sizes[i], async);
		    else
		      acc_delete_async (hostaddrs[i], sizes[i], async);
		  }
		break;
	      case GOMP_MAP_DETACH:
	      case GOMP_MAP_FORCE_DETACH:
	      case GOMP_MAP_FORCE_PRESENT:
		break;
	      case GOMP_MAP_DECLARE_DEALLOCATE:
	      case GOMP_MAP_FROM:
	      case GOMP_MAP_FORCE_FROM:
		if (finalize)
		  acc_copyout_finalize_async (hostaddrs[i], sizes[i], async);
		else
		  acc_copyout_async (hostaddrs[i], sizes[i], async);
		break;
	      case GOMP_MAP_STRUCT:
		{
		  int elems = sizes[i];
		  goacc_aq aq = get_goacc_asyncqueue (async);
		  for (int j = 1; j <= elems; j++)
		    {
		      struct splay_tree_key_s k;
		      k.host_start = (uintptr_t) hostaddrs[i + j];
		      k.host_end = k.host_start + sizes[i + j];
		      splay_tree_key str;
		      gomp_mutex_lock (&acc_dev->lock);
		      str = splay_tree_lookup (&acc_dev->mem_map, &k);
		      gomp_mutex_unlock (&acc_dev->lock);
		      if (str)
		        {
			  if (finalize)
			    {
			      str->refcount -= str->virtual_refcount;
			      str->virtual_refcount = 0;
			    }
			  if (str->virtual_refcount > 0)
			    {
			      str->refcount--;
			      str->virtual_refcount--;
			    }
			  else if (str->refcount > 0)
			    str->refcount--;
			  if (str->refcount == 0)
			    {
			      if (aq)
				goacc_remove_var_async (acc_dev, str, aq);
			      else
				gomp_remove_var (acc_dev, str);
			    }
			}
		    }
		  i += elems;
		}
		break;
	      default:
		gomp_fatal (">>>> GOACC_enter_exit_data UNHANDLED kind 0x%.2x",
			    kind);
		break;
	      }
	  else
	    {
	      if (kind == GOMP_MAP_DECLARE_DEALLOCATE)
		gomp_acc_declare_allocate (false, pointer, &hostaddrs[i],
					   &sizes[i], &kinds[i]);
	      else
		gomp_acc_remove_pointer (acc_dev, &hostaddrs[i], &sizes[i],
					 &kinds[i], async, finalize, pointer);
	      i += pointer - 1;
	    }
	}
    }

#ifdef RC_CHECKING
  gomp_mutex_lock (&acc_dev->lock);
  gomp_rc_check (acc_dev, thr->mapped_data);
  gomp_mutex_unlock (&acc_dev->lock);
#endif

 out:
  if (profiling_dispatch_p)
    {
      prof_info.event_type = data_enter ? acc_ev_enter_data_end: acc_ev_exit_data_end;
      enter_exit_data_event_info.other_event.event_type = prof_info.event_type;
      goacc_profiling_dispatch (&prof_info, &enter_exit_data_event_info,
				&api_info);

      thr->prof_info = NULL;
      thr->api_info = NULL;
    }
}

static void
goacc_wait (int async, int num_waits, va_list *ap)
{
  struct goacc_thread *thr = goacc_thread ();
  struct gomp_device_descr *acc_dev = thr->dev;

  while (num_waits--)
    {
      int qid = va_arg (*ap, int);
      goacc_aq aq = get_goacc_asyncqueue (qid);
      if (acc_dev->openacc.async.test_func (aq))
	continue;
      if (async == acc_async_sync)
	acc_dev->openacc.async.synchronize_func (aq);
      else if (qid == async)
      /* If we're waiting on the same asynchronous queue as we're
	    launching on, the queue itself will order work as
	    required, so there's no need to wait explicitly.  */
	;
      else
	{
	  goacc_aq aq2 = get_goacc_asyncqueue (async);
	  acc_dev->openacc.async.synchronize_func (aq);
	  acc_dev->openacc.async.serialize_func (aq, aq2);
	}
    }
}

void
GOACC_update (int flags_m, size_t mapnum,
	      void **hostaddrs, size_t *sizes, unsigned short *kinds,
	      int async, int num_waits, ...)
{
  int flags = GOACC_FLAGS_UNMARSHAL (flags_m);

  size_t i;

  goacc_lazy_initialize ();

  struct goacc_thread *thr = goacc_thread ();
  struct gomp_device_descr *acc_dev = thr->dev;

  bool profiling_dispatch_p
    = __builtin_expect (goacc_profiling_dispatch_p (true), false);

  acc_prof_info prof_info;
  if (profiling_dispatch_p)
    {
      thr->prof_info = &prof_info;

      prof_info.event_type = acc_ev_update_start;
      prof_info.valid_bytes = _ACC_PROF_INFO_VALID_BYTES;
      prof_info.version = _ACC_PROF_INFO_VERSION;
      prof_info.device_type = acc_device_type (acc_dev->type);
      prof_info.device_number = acc_dev->target_id;
      prof_info.thread_id = -1; //TODO
      prof_info.async = async;
      /* See <https://github.com/OpenACC/openacc-spec/issues/71>.  */
      prof_info.async_queue = prof_info.async;
      prof_info.src_file = NULL; //TODO
      prof_info.func_name = NULL; //TODO
      prof_info.line_no = -1; //TODO
      prof_info.end_line_no = -1; //TODO
      prof_info.func_line_no = -1; //TODO
      prof_info.func_end_line_no = -1; //TODO
    }
  acc_event_info update_event_info;
  if (profiling_dispatch_p)
    {
      update_event_info.other_event.event_type
	= prof_info.event_type;
      update_event_info.other_event.valid_bytes
	= _ACC_OTHER_EVENT_INFO_VALID_BYTES;
      update_event_info.other_event.parent_construct = acc_construct_update;
      update_event_info.other_event.implicit = 0;
      update_event_info.other_event.tool_info = NULL;
    }
  acc_api_info api_info;
  if (profiling_dispatch_p)
    {
      thr->api_info = &api_info;

      api_info.device_api = acc_device_api_none;
      api_info.valid_bytes = _ACC_API_INFO_VALID_BYTES;
      api_info.device_type = prof_info.device_type;
      api_info.vendor = -1; //TODO
      api_info.device_handle = NULL; //TODO
      api_info.context_handle = NULL; //TODO
      api_info.async_handle = NULL; //TODO
    }

  if (profiling_dispatch_p)
    goacc_profiling_dispatch (&prof_info, &update_event_info, &api_info);

  if ((acc_dev->capabilities & GOMP_OFFLOAD_CAP_SHARED_MEM)
      || (flags & GOACC_FLAG_HOST_FALLBACK))
    {
      //TODO
      prof_info.device_type = acc_device_host;
      api_info.device_type = prof_info.device_type;
      goto out;
    }

  if (num_waits > 0)
    {
      va_list ap;

      va_start (ap, num_waits);
      goacc_wait (async, num_waits, &ap);
      va_end (ap);
    }
  else if (num_waits == acc_async_noval)
    acc_wait_all_async (async);

  bool update_device = false;
  for (i = 0; i < mapnum; ++i)
    {
      unsigned char kind = kinds[i] & 0xff;

      switch (kind)
	{
	case GOMP_MAP_POINTER:
	case GOMP_MAP_TO_PSET:
	  break;

	case GOMP_MAP_ALWAYS_POINTER:
	  if (update_device)
	    {
	      /* Save the contents of the host pointer.  */
	      void *dptr = acc_deviceptr (hostaddrs[i-1]);
	      uintptr_t t = *(uintptr_t *) hostaddrs[i];

	      /* Update the contents of the host pointer to reflect
		 the value of the allocated device memory in the
		 previous pointer.  */
	      *(uintptr_t *) hostaddrs[i] = (uintptr_t)dptr;
	      acc_update_device (hostaddrs[i], sizeof (uintptr_t));

	      /* Restore the host pointer.  */
	      *(uintptr_t *) hostaddrs[i] = t;
	      update_device = false;
	    }
	  break;

	case GOMP_MAP_TO:
	  if (!acc_is_present (hostaddrs[i], sizes[i]))
	    {
	      update_device = false;
	      break;
	    }
	  /* Fallthru  */
	case GOMP_MAP_FORCE_TO:
	  update_device = true;
	  acc_update_device_async (hostaddrs[i], sizes[i], async);
	  break;

	case GOMP_MAP_FROM:
	  if (!acc_is_present (hostaddrs[i], sizes[i]))
	    {
	      update_device = false;
	      break;
	    }
	  /* Fallthru  */
	case GOMP_MAP_FORCE_FROM:
	  update_device = false;
	  acc_update_self_async (hostaddrs[i], sizes[i], async);
	  break;

	default:
	  gomp_fatal (">>>> GOACC_update UNHANDLED kind 0x%.2x", kind);
	  break;
	}
    }

 out:
  if (profiling_dispatch_p)
    {
      prof_info.event_type = acc_ev_update_end;
      update_event_info.other_event.event_type = prof_info.event_type;
      goacc_profiling_dispatch (&prof_info, &update_event_info, &api_info);

      thr->prof_info = NULL;
      thr->api_info = NULL;
    }
}

void
GOACC_wait (int async, int num_waits, ...)
{
  goacc_lazy_initialize ();

  struct goacc_thread *thr = goacc_thread ();

  /* No nesting.  */
  assert (thr->prof_info == NULL);
  assert (thr->api_info == NULL);
  acc_prof_info prof_info;
  acc_api_info api_info;
  bool profiling_setup_p
    = __builtin_expect (goacc_profiling_setup_p (thr, &prof_info, &api_info),
			false);
  if (profiling_setup_p)
    {
      prof_info.async = async;
      /* See <https://github.com/OpenACC/openacc-spec/issues/71>.  */
      prof_info.async_queue = prof_info.async;
    }

  if (num_waits)
    {
      va_list ap;

      va_start (ap, num_waits);
      goacc_wait (async, num_waits, &ap);
      va_end (ap);
    }
  else if (async == acc_async_sync)
    acc_wait_all ();
  else if (async == acc_async_noval)
    acc_wait_all_async (async);

  if (profiling_setup_p)
    {
      thr->prof_info = NULL;
      thr->api_info = NULL;
    }
}

int
GOACC_get_num_threads (void)
{
  return 1;
}

int
GOACC_get_thread_num (void)
{
  return 0;
}

void
GOACC_declare (int flags_m, size_t mapnum,
	       void **hostaddrs, size_t *sizes, unsigned short *kinds)
{
  int i;

  for (i = 0; i < mapnum; i++)
    {
      unsigned char kind = kinds[i] & 0xff;

      if (kind == GOMP_MAP_POINTER || kind == GOMP_MAP_TO_PSET)
	continue;

      switch (kind)
	{
	  case GOMP_MAP_FORCE_ALLOC:
	  case GOMP_MAP_FORCE_FROM:
	  case GOMP_MAP_FORCE_TO:
	  case GOMP_MAP_POINTER:
	  case GOMP_MAP_RELEASE:
	  case GOMP_MAP_DELETE:
	    GOACC_enter_exit_data (flags_m, 1, &hostaddrs[i], &sizes[i],
				   &kinds[i], 0, 0);
	    break;

	  case GOMP_MAP_FORCE_DEVICEPTR:
	    break;

	  case GOMP_MAP_ALLOC:
	    if (!acc_is_present (hostaddrs[i], sizes[i]))
	      GOACC_enter_exit_data (flags_m, 1, &hostaddrs[i], &sizes[i],
				     &kinds[i], 0, 0);
	    break;

	  case GOMP_MAP_TO:
	    GOACC_enter_exit_data (flags_m, 1, &hostaddrs[i], &sizes[i],
				   &kinds[i], 0, 0);

	    break;

	  case GOMP_MAP_FROM:
	    GOACC_enter_exit_data (flags_m, 1, &hostaddrs[i], &sizes[i],
				   &kinds[i], 0, 0);
	    break;

	  case GOMP_MAP_FORCE_PRESENT:
	    if (!acc_is_present (hostaddrs[i], sizes[i]))
	      gomp_fatal ("[%p,%ld] is not mapped", hostaddrs[i],
			  (unsigned long) sizes[i]);
	    break;

	  default:
	    assert (0);
	    break;
	}
    }
}
