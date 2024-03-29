/* Plugin for NVPTX execution.

   Copyright (C) 2013-2018 Free Software Foundation, Inc.

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

/* Nvidia PTX-specific parts of OpenACC support.  The cuda driver
   library appears to hold some implicit state, but the documentation
   is not clear as to what that state might be.  Or how one might
   propagate it from one thread to another.  */

#include "openacc.h"
#include "config.h"
#include "libgomp-plugin.h"
#include "oacc-plugin.h"
#include "gomp-constants.h"
#include "oacc-int.h"

#include <pthread.h>
#include <cuda.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#if PLUGIN_NVPTX_DYNAMIC
# include <dlfcn.h>

# define CUDA_CALLS \
CUDA_ONE_CALL (cuCtxCreate)		\
CUDA_ONE_CALL (cuCtxDestroy)		\
CUDA_ONE_CALL (cuCtxGetCurrent)		\
CUDA_ONE_CALL (cuCtxGetDevice)		\
CUDA_ONE_CALL (cuCtxPopCurrent)		\
CUDA_ONE_CALL (cuCtxPushCurrent)	\
CUDA_ONE_CALL (cuCtxSynchronize)	\
CUDA_ONE_CALL (cuDeviceGet)		\
CUDA_ONE_CALL (cuDeviceGetAttribute)	\
CUDA_ONE_CALL (cuDeviceGetCount)	\
CUDA_ONE_CALL (cuDeviceGetName)		\
CUDA_ONE_CALL (cuDeviceTotalMem)	\
CUDA_ONE_CALL (cuDriverGetVersion)	\
CUDA_ONE_CALL (cuEventCreate)		\
CUDA_ONE_CALL (cuEventDestroy)		\
CUDA_ONE_CALL (cuEventElapsedTime)	\
CUDA_ONE_CALL (cuEventQuery)		\
CUDA_ONE_CALL (cuEventRecord)		\
CUDA_ONE_CALL (cuEventSynchronize)	\
CUDA_ONE_CALL (cuFuncGetAttribute)	\
CUDA_ONE_CALL (cuGetErrorString)	\
CUDA_ONE_CALL (cuInit)			\
CUDA_ONE_CALL (cuLaunchKernel)		\
CUDA_ONE_CALL (cuLinkAddData)		\
CUDA_ONE_CALL (cuLinkComplete)		\
CUDA_ONE_CALL (cuLinkCreate)		\
CUDA_ONE_CALL (cuLinkDestroy)		\
CUDA_ONE_CALL (cuMemAlloc)		\
CUDA_ONE_CALL (cuMemAllocHost)		\
CUDA_ONE_CALL (cuMemcpy)		\
CUDA_ONE_CALL (cuMemcpyDtoDAsync)	\
CUDA_ONE_CALL (cuMemcpyDtoH)		\
CUDA_ONE_CALL (cuMemcpyDtoHAsync)	\
CUDA_ONE_CALL (cuMemcpyHtoD)		\
CUDA_ONE_CALL (cuMemcpyHtoDAsync)	\
CUDA_ONE_CALL (cuMemFree)		\
CUDA_ONE_CALL (cuMemFreeHost)		\
CUDA_ONE_CALL (cuMemGetAddressRange)	\
CUDA_ONE_CALL (cuMemGetInfo)		\
CUDA_ONE_CALL (cuMemHostGetDevicePointer)\
CUDA_ONE_CALL (cuModuleGetFunction)	\
CUDA_ONE_CALL (cuModuleGetGlobal)	\
CUDA_ONE_CALL (cuModuleLoad)		\
CUDA_ONE_CALL (cuModuleLoadData)	\
CUDA_ONE_CALL (cuModuleUnload)		\
CUDA_ONE_CALL (cuStreamCreate)		\
CUDA_ONE_CALL (cuStreamDestroy)		\
CUDA_ONE_CALL (cuStreamQuery)		\
CUDA_ONE_CALL (cuStreamSynchronize)	\
CUDA_ONE_CALL (cuStreamWaitEvent)
# define CUDA_ONE_CALL(call) \
  __typeof (call) *call;
struct cuda_lib_s {
  CUDA_CALLS
} cuda_lib;

/* -1 if init_cuda_lib has not been called yet, false
   if it has been and failed, true if it has been and succeeded.  */
static signed char cuda_lib_inited = -1;

/* Dynamically load the CUDA runtime library and initialize function
   pointers, return false if unsuccessful, true if successful.  */
static bool
init_cuda_lib (void)
{
  if (cuda_lib_inited != -1)
    return cuda_lib_inited;
  const char *cuda_runtime_lib = "libcuda.so.1";
  void *h = dlopen (cuda_runtime_lib, RTLD_LAZY);
  cuda_lib_inited = false;
  if (h == NULL)
    return false;
# undef CUDA_ONE_CALL
# define CUDA_ONE_CALL(call) CUDA_ONE_CALL_1 (call)
# define CUDA_ONE_CALL_1(call) \
  cuda_lib.call = dlsym (h, #call);	\
  if (cuda_lib.call == NULL)		\
    return false;
  CUDA_CALLS
  cuda_lib_inited = true;
  return true;
}
# undef CUDA_ONE_CALL
# undef CUDA_ONE_CALL_1
# define CUDA_CALL_PREFIX cuda_lib.
#else
# define CUDA_CALL_PREFIX
# define init_cuda_lib() true
#endif

/* Convenience macros for the frequently used CUDA library call and
   error handling sequence as well as CUDA library calls that
   do the error checking themselves or don't do it at all.  */

#define CUDA_CALL_ERET(ERET, FN, ...)		\
  do {						\
    unsigned __r				\
      = CUDA_CALL_PREFIX FN (__VA_ARGS__);	\
    if (__r != CUDA_SUCCESS)			\
      {						\
	GOMP_PLUGIN_error (#FN " error: %s",	\
			   cuda_error (__r));	\
	return ERET;				\
      }						\
  } while (0)

#define CUDA_CALL(FN, ...)			\
  CUDA_CALL_ERET (false, FN, __VA_ARGS__)

#define CUDA_CALL_ASSERT(FN, ...)		\
  do {						\
    unsigned __r				\
      = CUDA_CALL_PREFIX FN (__VA_ARGS__);	\
    if (__r != CUDA_SUCCESS)			\
      {						\
	GOMP_PLUGIN_fatal (#FN " error: %s",	\
			   cuda_error (__r));	\
      }						\
  } while (0)

#define CUDA_CALL_NOCHECK(FN, ...)		\
  CUDA_CALL_PREFIX FN (__VA_ARGS__)

static const char *
cuda_error (CUresult r)
{
#if CUDA_VERSION < 7000
  /* Specified in documentation and present in library from at least
     5.5.  Not declared in header file prior to 7.0.  */
  extern CUresult cuGetErrorString (CUresult, const char **);
#endif
  const char *desc;

  r = CUDA_CALL_NOCHECK (cuGetErrorString, r, &desc);
  if (r != CUDA_SUCCESS)
    desc = "unknown cuda error";

  return desc;
}

/* From gcc/system.h.  */
#undef MIN
#undef MAX
#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X,Y) ((X) > (Y) ? (X) : (Y))

static unsigned int instantiated_devices = 0;
static pthread_mutex_t ptx_dev_lock = PTHREAD_MUTEX_INITIALIZER;

/* NVPTX/CUDA specific definition of asynchronous queues.  */
struct goacc_asyncqueue
{
  CUstream cuda_stream;
  pthread_mutex_t lock;
};

struct nvptx_callback
{
  void (*fn) (void *);
  void *ptr;
  struct goacc_asyncqueue *aq;
  struct nvptx_callback *next;
};

/* Thread-specific data for PTX.  */

struct nvptx_thread
{
  /* We currently have this embedded inside the plugin because libgomp manages
     devices through integer target_ids.  This might be better if using an
     opaque target-specific pointer directly from gomp_device_descr.  */
  struct ptx_device *ptx_dev;
};

/* Target data function launch information.  */

struct targ_fn_launch
{
  const char *fn;
  unsigned short dim[GOMP_DIM_MAX];
};

/* Target PTX object information.  */

struct targ_ptx_obj
{
  const char *code;
  size_t size;
};

/* Target data image information.  */

typedef struct nvptx_tdata
{
  const struct targ_ptx_obj *ptx_objs;
  unsigned ptx_num;

  const char *const *var_names;
  unsigned var_num;

  const struct targ_fn_launch *fn_descs;
  unsigned fn_num;
} nvptx_tdata_t;

/* Descriptor of a loaded function.  */

struct targ_fn_descriptor
{
  CUfunction fn;
  const struct targ_fn_launch *launch;
  int regs_per_thread;
  int max_threads_per_block;
};

/* A loaded PTX image.  */
struct ptx_image_data
{
  const void *target_data;
  CUmodule module;

  struct targ_fn_descriptor *fns;  /* Array of functions.  */
  
  struct ptx_image_data *next;
};

struct ptx_free_block
{
  void *ptr;
  struct ptx_free_block *next;
};

struct ptx_device
{
  CUcontext ctx;
  bool ctx_shared;
  CUdevice dev;

  int ord;
  bool overlap;
  bool map;
  bool concur;
  bool mkern;
  int mode;
  int compute_capability_major;
  int compute_capability_minor;
  int clock_khz;
  int num_sms;
  int regs_per_block;
  int regs_per_sm;
  int max_threads_per_block;
  int warp_size;
  int max_threads_per_multiprocessor;
  int max_shared_memory_per_multiprocessor;

  int binary_version;

  /* register_allocation_unit_size and register_allocation_granularity
     were extracted from the "Register Allocation Granularity" in
     Nvidia's CUDA Occupancy Calculator spreadsheet.  */
  int register_allocation_unit_size;
  int register_allocation_granularity;

  struct ptx_image_data *images;  /* Images loaded on device.  */
  pthread_mutex_t image_lock;     /* Lock for above list.  */

  struct ptx_free_block *free_blocks;
  pthread_mutex_t free_blocks_lock;

  struct ptx_device *next;
};

static struct ptx_device **ptx_devices;

static inline struct nvptx_thread *
nvptx_thread (void)
{
  return (struct nvptx_thread *) GOMP_PLUGIN_acc_thread ();
}

/* Initialize the device.  Return TRUE on success, else FALSE.  PTX_DEV_LOCK
   should be locked on entry and remains locked on exit.  */

static bool
nvptx_init (void)
{
  int ndevs;

  if (instantiated_devices != 0)
    return true;

  if (!init_cuda_lib ())
    return false;

  CUDA_CALL (cuInit, 0);

  CUDA_CALL (cuDeviceGetCount, &ndevs);
  ptx_devices = GOMP_PLUGIN_malloc_cleared (sizeof (struct ptx_device *)
					    * ndevs);
  return true;
}

/* Select the N'th PTX device for the current host thread.  The device must
   have been previously opened before calling this function.  */

static bool
nvptx_attach_host_thread_to_device (int n)
{
  CUdevice dev;
  CUresult r;
  struct ptx_device *ptx_dev;
  CUcontext thd_ctx;

  r = CUDA_CALL_NOCHECK (cuCtxGetDevice, &dev);
  if (r == CUDA_ERROR_NOT_PERMITTED)
    {
      /* Assume we're in a CUDA callback, just return true.  */
      return true;
    }
  if (r != CUDA_SUCCESS && r != CUDA_ERROR_INVALID_CONTEXT)
    {
      GOMP_PLUGIN_error ("cuCtxGetDevice error: %s", cuda_error (r));
      return false;
    }

  if (r != CUDA_ERROR_INVALID_CONTEXT && dev == n)
    return true;
  else
    {
      CUcontext old_ctx;

      ptx_dev = ptx_devices[n];
      if (!ptx_dev)
	{
	  GOMP_PLUGIN_error ("device %d not found", n);
	  return false;
	}

      CUDA_CALL (cuCtxGetCurrent, &thd_ctx);

      /* We don't necessarily have a current context (e.g. if it has been
         destroyed.  Pop it if we do though.  */
      if (thd_ctx != NULL)
	CUDA_CALL (cuCtxPopCurrent, &old_ctx);

      CUDA_CALL (cuCtxPushCurrent, ptx_dev->ctx);
    }
  return true;
}

static struct ptx_device *
nvptx_open_device (int n)
{
  struct ptx_device *ptx_dev;
  CUdevice dev, ctx_dev;
  CUresult r;
  int async_engines, pi;

  CUDA_CALL_ERET (NULL, cuDeviceGet, &dev, n);

  ptx_dev = GOMP_PLUGIN_malloc (sizeof (struct ptx_device));

  ptx_dev->ord = n;
  ptx_dev->dev = dev;
  ptx_dev->ctx_shared = false;
  ptx_dev->binary_version = 0;
  ptx_dev->register_allocation_unit_size = 0;
  ptx_dev->register_allocation_granularity = 0;

  r = CUDA_CALL_NOCHECK (cuCtxGetDevice, &ctx_dev);
  if (r != CUDA_SUCCESS && r != CUDA_ERROR_INVALID_CONTEXT)
    {
      GOMP_PLUGIN_error ("cuCtxGetDevice error: %s", cuda_error (r));
      return NULL;
    }
  
  if (r != CUDA_ERROR_INVALID_CONTEXT && ctx_dev != dev)
    {
      /* The current host thread has an active context for a different device.
         Detach it.  */
      CUcontext old_ctx;
      CUDA_CALL_ERET (NULL, cuCtxPopCurrent, &old_ctx);
    }

  CUDA_CALL_ERET (NULL, cuCtxGetCurrent, &ptx_dev->ctx);

  if (!ptx_dev->ctx)
    CUDA_CALL_ERET (NULL, cuCtxCreate, &ptx_dev->ctx, CU_CTX_SCHED_AUTO, dev);
  else
    ptx_dev->ctx_shared = true;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_GPU_OVERLAP, dev);
  ptx_dev->overlap = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY, dev);
  ptx_dev->map = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_CONCURRENT_KERNELS, dev);
  ptx_dev->concur = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_COMPUTE_MODE, dev);
  ptx_dev->mode = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
  ptx_dev->compute_capability_major = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
  ptx_dev->compute_capability_minor = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_INTEGRATED, dev);
  ptx_dev->mkern = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_CLOCK_RATE, dev);
  ptx_dev->clock_khz = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev);
  ptx_dev->num_sms = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_BLOCK, dev);
  ptx_dev->regs_per_block = pi;

  /* CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_MULTIPROCESSOR = 82 is defined only
     in CUDA 6.0 and newer.  */
  r = CUDA_CALL_NOCHECK (cuDeviceGetAttribute, &pi, 82, dev);
  /* Fallback: use limit of registers per block, which is usually equal.  */
  if (r == CUDA_ERROR_INVALID_VALUE)
    pi = ptx_dev->regs_per_block;
  else if (r != CUDA_SUCCESS)
    {
      GOMP_PLUGIN_error ("cuDeviceGetAttribute error: %s", cuda_error (r));
      return NULL;
    }
  ptx_dev->regs_per_sm = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK, dev);
  ptx_dev->max_threads_per_block = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_WARP_SIZE, dev);
  ptx_dev->warp_size = pi;
  if (pi != 32)
    {
      GOMP_PLUGIN_error ("Only warp size 32 is supported");
      return NULL;
    }

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi, CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_MULTIPROCESSOR, dev);
  ptx_dev->max_threads_per_multiprocessor = pi;

  CUDA_CALL_ERET (NULL, cuDeviceGetAttribute,
		  &pi,
		  CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR,
		  dev);
  ptx_dev->max_shared_memory_per_multiprocessor = pi;

  r = CUDA_CALL_NOCHECK (cuDeviceGetAttribute, &async_engines,
			 CU_DEVICE_ATTRIBUTE_ASYNC_ENGINE_COUNT, dev);
  if (r != CUDA_SUCCESS)
    async_engines = 1;

  ptx_dev->images = NULL;
  pthread_mutex_init (&ptx_dev->image_lock, NULL);

  ptx_dev->free_blocks = NULL;
  pthread_mutex_init (&ptx_dev->free_blocks_lock, NULL);

  GOMP_PLUGIN_debug (0, "Nvidia device %d:\n\tGPU_OVERLAP = %d\n"
		     "\tCAN_MAP_HOST_MEMORY = %d\n\tCONCURRENT_KERNELS = %d\n"
		     "\tCOMPUTE_MODE = %d\n"
		     "\tCU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = %d\n"
		     "\tCU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR = %d\n"
		     "\tINTEGRATED = %d\n"
		     "\tMAX_THREADS_PER_BLOCK = %d\n\tWARP_SIZE = %d\n"
		     "\tMULTIPROCESSOR_COUNT = %d\n"
		     "\tMAX_THREADS_PER_MULTIPROCESSOR = %d\n"
		     "\tMAX_REGISTERS_PER_MULTIPROCESSOR = %d\n"
		     "\tMAX_SHARED_MEMORY_PER_MULTIPROCESSOR = %d\n",
		     ptx_dev->ord, ptx_dev->overlap, ptx_dev->map,
		     ptx_dev->concur, ptx_dev->mode,
		     ptx_dev->compute_capability_major,
		     ptx_dev->compute_capability_minor,
		     ptx_dev->mkern, ptx_dev->max_threads_per_block,
		     ptx_dev->warp_size, ptx_dev->num_sms,
		     ptx_dev->max_threads_per_multiprocessor,
		     ptx_dev->regs_per_sm,
		     ptx_dev->max_shared_memory_per_multiprocessor);

  /* K80 (SM_37) boards contain two physical GPUs.  Consequntly they
     report 2x larger values for MAX_REGISTERS_PER_MULTIPROCESSOR and
     MAX_SHARED_MEMORY_PER_MULTIPROCESSOR.  Those values need to be
     adjusted on order to allow the nvptx_exec to select an
     appropriate num_workers.  */
  if (ptx_dev->compute_capability_major == 3
      && ptx_dev->compute_capability_minor == 7)
    {
      ptx_dev->regs_per_sm /= 2;
      ptx_dev->max_shared_memory_per_multiprocessor /= 2;
    }

  return ptx_dev;
}

static bool
nvptx_close_device (struct ptx_device *ptx_dev)
{
  if (!ptx_dev)
    return true;

  for (struct ptx_free_block *b = ptx_dev->free_blocks; b;)
    {
      struct ptx_free_block *b_next = b->next;
      CUDA_CALL (cuMemFree, (CUdeviceptr) b->ptr);
      free (b);
      b = b_next;
    }

  pthread_mutex_destroy (&ptx_dev->free_blocks_lock);
  pthread_mutex_destroy (&ptx_dev->image_lock);

  if (!ptx_dev->ctx_shared)
    CUDA_CALL (cuCtxDestroy, ptx_dev->ctx);

  free (ptx_dev);
  return true;
}

static int
nvptx_get_num_devices (void)
{
  int n;

  /* PR libgomp/65099: Currently, we only support offloading in 64-bit
     configurations.  */
  if (sizeof (void *) != 8)
    {
      GOMP_PLUGIN_debug (0, "Disabling nvptx offloading;"
			 " only 64-bit configurations are supported\n");
      return 0;
    }

  /* This function will be called before the plugin has been initialized in
     order to enumerate available devices, but CUDA API routines can't be used
     until cuInit has been called.  Just call it now (but don't yet do any
     further initialization).  */
  if (instantiated_devices == 0)
    {
      if (!init_cuda_lib ())
	return 0;
      CUresult r = CUDA_CALL_NOCHECK (cuInit, 0);
      /* This is not an error: e.g. we may have CUDA libraries installed but
         no devices available.  */
      if (r != CUDA_SUCCESS)
	{
	  GOMP_PLUGIN_debug (0, "Disabling nvptx offloading; cuInit: %s\n",
			     cuda_error (r));
	  return 0;
	}
    }

  CUDA_CALL_ERET (-1, cuDeviceGetCount, &n);
  return n;
}

static void
notify_var (const char *var_name, const char *env_var)
{
  if (env_var == NULL)
    GOMP_PLUGIN_debug (0, "%s: <Not defined>\n", var_name);
  else
    GOMP_PLUGIN_debug (0, "%s: '%s'\n", var_name, env_var);
}

static bool
link_ptx (CUmodule *module, const struct targ_ptx_obj *ptx_objs,
	  unsigned num_objs)
{
  CUjit_option opts[6];
  void *optvals[6];
  float elapsed = 0.0;
  char elog[1024];
  char ilog[16384];
  CUlinkState linkstate;
  CUresult r;
  void *linkout;
  size_t linkoutsize __attribute__ ((unused));

  opts[0] = CU_JIT_WALL_TIME;
  optvals[0] = &elapsed;

  opts[1] = CU_JIT_INFO_LOG_BUFFER;
  optvals[1] = &ilog[0];

  opts[2] = CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES;
  optvals[2] = (void *) sizeof ilog;

  opts[3] = CU_JIT_ERROR_LOG_BUFFER;
  optvals[3] = &elog[0];

  opts[4] = CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES;
  optvals[4] = (void *) sizeof elog;

  opts[5] = CU_JIT_LOG_VERBOSE;
  optvals[5] = (void *) 1;

  CUDA_CALL (cuLinkCreate, 6, opts, optvals, &linkstate);

  for (; num_objs--; ptx_objs++)
    {
      /* cuLinkAddData's 'data' argument erroneously omits the const
	 qualifier.  */
      GOMP_PLUGIN_debug (0, "Loading:\n---\n%s\n---\n", ptx_objs->code);
      r = CUDA_CALL_NOCHECK (cuLinkAddData, linkstate, CU_JIT_INPUT_PTX,
			     (char *) ptx_objs->code, ptx_objs->size,
			     0, 0, 0, 0);
      if (r != CUDA_SUCCESS)
	{
	  GOMP_PLUGIN_error ("Link error log %s\n", &elog[0]);
	  GOMP_PLUGIN_error ("cuLinkAddData (ptx_code) error: %s",
			     cuda_error (r));
	  return false;
	}
    }

  GOMP_PLUGIN_debug (0, "Linking\n");
  r = CUDA_CALL_NOCHECK (cuLinkComplete, linkstate, &linkout, &linkoutsize);

  GOMP_PLUGIN_debug (0, "Link complete: %fms\n", elapsed);
  GOMP_PLUGIN_debug (0, "Link log %s\n", &ilog[0]);

  if (r != CUDA_SUCCESS)
    {
      GOMP_PLUGIN_error ("cuLinkComplete error: %s", cuda_error (r));
      return false;
    }

  CUDA_CALL (cuModuleLoadData, module, linkout);
  CUDA_CALL (cuLinkDestroy, linkstate);
  return true;
}

static void
nvptx_exec (void (*fn), size_t mapnum, void **hostaddrs, void **devaddrs,
	    unsigned *dims, void *targ_mem_desc,
	    void **kargs, CUstream stream)
{
  struct targ_fn_descriptor *targ_fn = (struct targ_fn_descriptor *) fn;
  CUfunction function;
  int i;
  int cpu_size = nvptx_thread ()->ptx_dev->max_threads_per_multiprocessor;
  int block_size = nvptx_thread ()->ptx_dev->max_threads_per_block;
  int dev_size = nvptx_thread ()->ptx_dev->num_sms;
  int warp_size = nvptx_thread ()->ptx_dev->warp_size;
  int rf_size = nvptx_thread ()->ptx_dev->regs_per_sm;
  int reg_unit_size = nvptx_thread ()->ptx_dev->register_allocation_unit_size;
  int reg_granularity
    = nvptx_thread ()->ptx_dev->register_allocation_granularity;

  function = targ_fn->fn;

  /* Initialize the launch dimensions.  Typically this is constant,
     provided by the device compiler, but we must permit runtime
     values.  */
  int seen_zero = 0;
  for (i = 0; i != GOMP_DIM_MAX; i++)
    {
      if (targ_fn->launch->dim[i])
       dims[i] = targ_fn->launch->dim[i];
      if (!dims[i])
       seen_zero = 1;
    }

  /* Calculate the optimal number of gangs for the current device.  */
  int reg_used = targ_fn->regs_per_thread;
  int reg_per_warp = ((reg_used * warp_size + reg_unit_size - 1)
		      / reg_unit_size) * reg_unit_size;
  int threads_per_sm = (rf_size / reg_per_warp / reg_granularity)
    * reg_granularity * warp_size;
  int threads_per_block = threads_per_sm > block_size
    ? block_size : threads_per_sm;

  if (threads_per_sm > cpu_size)
    threads_per_sm = cpu_size;

  /* See if the user provided GOMP_OPENACC_DIM environment variable to
     specify runtime defaults. */
  static int default_dims[GOMP_DIM_MAX];

  pthread_mutex_lock (&ptx_dev_lock);
  if (!default_dims[0])
    {
      const char *var_name = "GOMP_OPENACC_DIM";
      /* We only read the environment variable once.  You can't
	 change it in the middle of execution.  The syntax  is
	 the same as for the -fopenacc-dim compilation option.  */
      const char *env_var = getenv (var_name);
      notify_var (var_name, env_var);
      if (env_var)
	for (int i = 0; i < GOMP_DIM_MAX; ++i)
	  default_dims[i] = GOMP_PLUGIN_acc_default_dim (i);

      /* 32 is the default for known hardware.  */
      int gang = 0, worker = 32, vector = 32;

      gang = (cpu_size / block_size) * dev_size;
      vector = warp_size;

      /* If the user hasn't specified the number of gangs, determine
	 it dynamically based on the hardware configuration.  */
      if (default_dims[GOMP_DIM_GANG] == 0)
	default_dims[GOMP_DIM_GANG] = -1;
      /* The worker size must not exceed the hardware.  */
      if (default_dims[GOMP_DIM_WORKER] < 1
	  || (default_dims[GOMP_DIM_WORKER] > worker && gang))
	default_dims[GOMP_DIM_WORKER] = -1;
      /* The vector size must exactly match the hardware.  */
      if (default_dims[GOMP_DIM_VECTOR] < 1
	  || (default_dims[GOMP_DIM_VECTOR] != vector && gang))
	default_dims[GOMP_DIM_VECTOR] = vector;

      GOMP_PLUGIN_debug (0, " default dimensions [%d,%d,%d]\n",
			 default_dims[GOMP_DIM_GANG],
			 default_dims[GOMP_DIM_WORKER],
			 default_dims[GOMP_DIM_VECTOR]);
    }
  pthread_mutex_unlock (&ptx_dev_lock);

  if (seen_zero)
    {
      int vectors = dims[GOMP_DIM_VECTOR] > 0
	? dims[GOMP_DIM_VECTOR] : warp_size;
      int workers
	= MIN (threads_per_block, targ_fn->max_threads_per_block) / vectors;

      for (i = 0; i != GOMP_DIM_MAX; i++)
	if (!dims[i])
	  {
	    if (default_dims[i] > 0)
	      dims[i] = default_dims[i];
	    else
	      switch (i) {
	      case GOMP_DIM_GANG:
		/* The constant 2 was emperically.  The justification
		   behind it is to prevent the hardware from idling by
		   throwing twice the amount of work that it can
		   physically handle.  */
		dims[i] = (reg_granularity > 0)
		  ? 2 * threads_per_sm / warp_size * dev_size
		  : 2 * dev_size;
		break;
	      case GOMP_DIM_WORKER:
		dims[i] = workers;
		break;
	      case GOMP_DIM_VECTOR:
		dims[i] = vectors;
		break;
	      default:
		abort ();
	      }
	  }
    }

  /* Check if the accelerator has sufficient hardware resources to
     launch the offloaded kernel.  */
  if (dims[GOMP_DIM_WORKER] * dims[GOMP_DIM_VECTOR]
      > targ_fn->max_threads_per_block)
    GOMP_PLUGIN_fatal ("The Nvidia accelerator has insufficient resources to"
		       " launch '%s' with num_workers = %d and vector_length ="
		       " %d; recompile the program with 'num_workers = x and"
		       " vector_length = y' on that offloaded region or "
		       "'-fopenacc-dim=-:x:y' where x * y <= %d.\n",
		       targ_fn->launch->fn, dims[GOMP_DIM_WORKER],
		       dims[GOMP_DIM_VECTOR], targ_fn->max_threads_per_block);

  GOMP_PLUGIN_debug (0, "  %s: kernel %s: launch"
		     " gangs=%u, workers=%u, vectors=%u\n",
		     __FUNCTION__, targ_fn->launch->fn, dims[GOMP_DIM_GANG],
		     dims[GOMP_DIM_WORKER], dims[GOMP_DIM_VECTOR]);

  // OpenACC		CUDA
  //
  // num_gangs		nctaid.x
  // num_workers	ntid.y
  // vector length	ntid.x

  struct goacc_thread *thr = GOMP_PLUGIN_goacc_thread ();
  acc_prof_info *prof_info = thr->prof_info;
  acc_event_info enqueue_launch_event_info;
  acc_api_info *api_info = thr->api_info;
  bool profiling_dispatch_p = __builtin_expect (prof_info != NULL, false);
  if (profiling_dispatch_p)
    {
      prof_info->event_type = acc_ev_enqueue_launch_start;

      enqueue_launch_event_info.launch_event.event_type
	= prof_info->event_type;
      enqueue_launch_event_info.launch_event.valid_bytes
	= _ACC_LAUNCH_EVENT_INFO_VALID_BYTES;
      enqueue_launch_event_info.launch_event.parent_construct
	/* TODO = compute_construct_event_info.other_event.parent_construct */
	= acc_construct_parallel; //TODO: kernels...
      enqueue_launch_event_info.launch_event.implicit = 1;
      enqueue_launch_event_info.launch_event.tool_info = NULL;
      enqueue_launch_event_info.launch_event.kernel_name
	= /* TODO */ (char *) /* TODO */ targ_fn->launch->fn;
      enqueue_launch_event_info.launch_event.num_gangs
	= dims[GOMP_DIM_GANG];
      enqueue_launch_event_info.launch_event.num_workers
	= dims[GOMP_DIM_WORKER];
      enqueue_launch_event_info.launch_event.vector_length
	= dims[GOMP_DIM_VECTOR];

      api_info->device_api = acc_device_api_cuda;

      GOMP_PLUGIN_goacc_profiling_dispatch (prof_info, &enqueue_launch_event_info,
					    api_info);
    }
  
  CUDA_CALL_ASSERT (cuLaunchKernel, function,
		    dims[GOMP_DIM_GANG], 1, 1,
		    dims[GOMP_DIM_VECTOR], dims[GOMP_DIM_WORKER], 1,
		    0, stream, kargs, 0);

  if (profiling_dispatch_p)
    {
      prof_info->event_type = acc_ev_enqueue_launch_end;
      enqueue_launch_event_info.launch_event.event_type
	= prof_info->event_type;
      GOMP_PLUGIN_goacc_profiling_dispatch (prof_info, &enqueue_launch_event_info,
					    api_info);
    }

  GOMP_PLUGIN_debug (0, "  %s: kernel %s: finished\n", __FUNCTION__,
		     targ_fn->launch->fn);
}

void * openacc_get_current_cuda_context (void);

static void *
nvptx_alloc (size_t s)
{
  CUdeviceptr d;

  CUDA_CALL_ERET (NULL, cuMemAlloc, &d, s);

  struct goacc_thread *thr = GOMP_PLUGIN_goacc_thread ();
  bool profiling_dispatch_p
    = __builtin_expect (thr != NULL && thr->prof_info != NULL, false);
  if (profiling_dispatch_p)
    {
      acc_prof_info *prof_info = thr->prof_info;
      acc_event_info data_event_info;
      acc_api_info *api_info = thr->api_info;

      prof_info->event_type = acc_ev_alloc;

      data_event_info.data_event.event_type = prof_info->event_type;
      data_event_info.data_event.valid_bytes
	= _ACC_DATA_EVENT_INFO_VALID_BYTES;
      data_event_info.data_event.parent_construct
	= acc_construct_parallel; //TODO
      data_event_info.data_event.implicit = 1; //TODO
      data_event_info.data_event.tool_info = NULL;
      data_event_info.data_event.var_name = NULL; //TODO
      data_event_info.data_event.bytes = s;
      data_event_info.data_event.host_ptr = NULL;
      data_event_info.data_event.device_ptr = (void *) d;

      api_info->device_api = acc_device_api_cuda;

      GOMP_PLUGIN_goacc_profiling_dispatch (prof_info, &data_event_info,
					    api_info);
    }

  return (void *) d;
}

static bool
nvptx_free (void *p, struct ptx_device *ptx_dev)
{
  /* Assume callback context if this is null.  */
  if (GOMP_PLUGIN_goacc_thread () == NULL)
    {
      struct ptx_free_block *n
	= GOMP_PLUGIN_malloc (sizeof (struct ptx_free_block));
      n->ptr = p;
      pthread_mutex_lock (&ptx_dev->free_blocks_lock);
      n->next = ptx_dev->free_blocks;
      ptx_dev->free_blocks = n;
      pthread_mutex_unlock (&ptx_dev->free_blocks_lock);
      return true;
    }

  CUdeviceptr pb;
  size_t ps;

  CUDA_CALL (cuMemGetAddressRange, &pb, &ps, (CUdeviceptr) p);
  if ((CUdeviceptr) p != pb)
    {
      GOMP_PLUGIN_error ("invalid device address");
      return false;
    }

  CUDA_CALL (cuMemFree, (CUdeviceptr) p);
  return true;
}

static void *
nvptx_get_current_cuda_device (void)
{
  struct nvptx_thread *nvthd = nvptx_thread ();

  if (!nvthd || !nvthd->ptx_dev)
    return NULL;

  return &nvthd->ptx_dev->dev;
}

static void *
nvptx_get_current_cuda_context (void)
{
  struct nvptx_thread *nvthd = nvptx_thread ();

  if (!nvthd || !nvthd->ptx_dev)
    return NULL;

  return nvthd->ptx_dev->ctx;
}

/* Plugin entry points.  */

const char *
GOMP_OFFLOAD_get_name (void)
{
  return "nvptx";
}

unsigned int
GOMP_OFFLOAD_get_caps (void)
{
  return GOMP_OFFLOAD_CAP_OPENACC_200 | GOMP_OFFLOAD_CAP_OPENMP_400;
}

int
GOMP_OFFLOAD_get_type (void)
{
  return OFFLOAD_TARGET_TYPE_NVIDIA_PTX;
}

int
GOMP_OFFLOAD_get_num_devices (void)
{
  return nvptx_get_num_devices ();
}

union gomp_device_property_value
GOMP_OFFLOAD_get_property (int n, int prop)
{
  union gomp_device_property_value propval = { .val = 0 };

  pthread_mutex_lock (&ptx_dev_lock);

  if (!nvptx_init () || n >= nvptx_get_num_devices ())
    {
      pthread_mutex_unlock (&ptx_dev_lock);
      return propval;
    }

  switch (prop)
    {
    case GOMP_DEVICE_PROPERTY_MEMORY:
      {
	size_t total_mem;
	CUdevice dev;

	CUDA_CALL_ERET (propval, cuDeviceGet, &dev, n);
	CUDA_CALL_ERET (propval, cuDeviceTotalMem, &total_mem, dev);
	propval.val = total_mem;
      }
      break;
    case GOMP_DEVICE_PROPERTY_FREE_MEMORY:
      {
	size_t total_mem;
	size_t free_mem;
	CUdevice ctxdev;
	CUdevice dev;

	CUDA_CALL_ERET (propval, cuCtxGetDevice, &ctxdev);
	CUDA_CALL_ERET (propval, cuDeviceGet, &dev, n);
	if (dev == ctxdev)
	  CUDA_CALL_ERET (propval, cuMemGetInfo, &free_mem, &total_mem);
	else if (ptx_devices[n])
	  {
	    CUcontext old_ctx;

	    CUDA_CALL_ERET (propval, cuCtxPushCurrent, ptx_devices[n]->ctx);
	    CUDA_CALL_ERET (propval, cuMemGetInfo, &free_mem, &total_mem);
	    CUDA_CALL_ASSERT (cuCtxPopCurrent, &old_ctx);
	  }
	else
	  {
	    CUcontext new_ctx;

	    CUDA_CALL_ERET (propval, cuCtxCreate, &new_ctx, CU_CTX_SCHED_AUTO,
			    dev);
	    CUDA_CALL_ERET (propval, cuMemGetInfo, &free_mem, &total_mem);
	    CUDA_CALL_ASSERT (cuCtxDestroy, new_ctx);
	  }
	propval.val = free_mem;
      }
      break;
    case GOMP_DEVICE_PROPERTY_NAME:
      {
	static char name[256];
	CUdevice dev;

	CUDA_CALL_ERET (propval, cuDeviceGet, &dev, n);
	CUDA_CALL_ERET (propval, cuDeviceGetName, name, sizeof (name), dev);
	propval.ptr = name;
      }
      break;
    case GOMP_DEVICE_PROPERTY_VENDOR:
      propval.ptr = "Nvidia";
      break;
    case GOMP_DEVICE_PROPERTY_DRIVER:
      {
	static char ver[11];
	int v;

	CUDA_CALL_ERET (propval, cuDriverGetVersion, &v);
	snprintf (ver, sizeof (ver) - 1, "%u.%u", v / 1000, v % 1000 / 10);
	propval.ptr = ver;
      }
      break;
    default:
      break;
    }

  pthread_mutex_unlock (&ptx_dev_lock);
  return propval;
}

bool
GOMP_OFFLOAD_init_device (int n)
{
  struct ptx_device *dev;

  pthread_mutex_lock (&ptx_dev_lock);

  if (!nvptx_init () || ptx_devices[n] != NULL)
    {
      pthread_mutex_unlock (&ptx_dev_lock);
      return false;
    }

  dev = nvptx_open_device (n);
  if (dev)
    {
      ptx_devices[n] = dev;
      instantiated_devices++;
    }

  pthread_mutex_unlock (&ptx_dev_lock);

  return dev != NULL;
}

bool
GOMP_OFFLOAD_fini_device (int n)
{
  pthread_mutex_lock (&ptx_dev_lock);

  if (ptx_devices[n] != NULL)
    {
      if (!nvptx_attach_host_thread_to_device (n)
	  || !nvptx_close_device (ptx_devices[n]))
	{
	  pthread_mutex_unlock (&ptx_dev_lock);
	  return false;
	}
      ptx_devices[n] = NULL;
      instantiated_devices--;
    }

  pthread_mutex_unlock (&ptx_dev_lock);
  return true;
}

/* Return the libgomp version number we're compatible with.  There is
   no requirement for cross-version compatibility.  */

unsigned
GOMP_OFFLOAD_version (void)
{
  return GOMP_VERSION;
}

/* Initialize __nvptx_clocktick, if present in MODULE.  */

static void
nvptx_set_clocktick (CUmodule module, struct ptx_device *dev)
{
  CUdeviceptr dptr;
  CUresult r = CUDA_CALL_NOCHECK (cuModuleGetGlobal, &dptr, NULL,
				  module, "__nvptx_clocktick");
  if (r == CUDA_ERROR_NOT_FOUND)
    return;
  if (r != CUDA_SUCCESS)
    GOMP_PLUGIN_fatal ("cuModuleGetGlobal error: %s", cuda_error (r));
  double __nvptx_clocktick = 1e-3 / dev->clock_khz;
  r = CUDA_CALL_NOCHECK (cuMemcpyHtoD, dptr, &__nvptx_clocktick,
			 sizeof (__nvptx_clocktick));
  if (r != CUDA_SUCCESS)
    GOMP_PLUGIN_fatal ("cuMemcpyHtoD error: %s", cuda_error (r));
}

/* Load the (partial) program described by TARGET_DATA to device
   number ORD.  Allocate and return TARGET_TABLE.  */

int
GOMP_OFFLOAD_load_image (int ord, unsigned version, const void *target_data,
			 struct addr_pair **target_table)
{
  CUmodule module;
  const char *const *var_names;
  const struct targ_fn_launch *fn_descs;
  unsigned int fn_entries, var_entries, i, j;
  struct targ_fn_descriptor *targ_fns;
  struct addr_pair *targ_tbl;
  const nvptx_tdata_t *img_header = (const nvptx_tdata_t *) target_data;
  struct ptx_image_data *new_image;
  struct ptx_device *dev;

  if (GOMP_VERSION_DEV (version) > GOMP_VERSION_NVIDIA_PTX)
    {
      GOMP_PLUGIN_error ("Offload data incompatible with PTX plugin"
			 " (expected %u, received %u)",
			 GOMP_VERSION_NVIDIA_PTX, GOMP_VERSION_DEV (version));
      return -1;
    }

  if (!nvptx_attach_host_thread_to_device (ord)
      || !link_ptx (&module, img_header->ptx_objs, img_header->ptx_num))
    return -1;

  dev = ptx_devices[ord];

  /* The mkoffload utility emits a struct of pointers/integers at the
     start of each offload image.  The array of kernel names and the
     functions addresses form a one-to-one correspondence.  */

  var_entries = img_header->var_num;
  var_names = img_header->var_names;
  fn_entries = img_header->fn_num;
  fn_descs = img_header->fn_descs;

  targ_tbl = GOMP_PLUGIN_malloc (sizeof (struct addr_pair)
				 * (fn_entries + var_entries));
  targ_fns = GOMP_PLUGIN_malloc (sizeof (struct targ_fn_descriptor)
				 * fn_entries);

  *target_table = targ_tbl;

  new_image = GOMP_PLUGIN_malloc (sizeof (struct ptx_image_data));
  new_image->target_data = target_data;
  new_image->module = module;
  new_image->fns = targ_fns;

  pthread_mutex_lock (&dev->image_lock);
  new_image->next = dev->images;
  dev->images = new_image;
  pthread_mutex_unlock (&dev->image_lock);

  for (i = 0; i < fn_entries; i++, targ_fns++, targ_tbl++)
    {
      CUfunction function;
      int nregs, mthrs;

      CUDA_CALL_ERET (-1, cuModuleGetFunction, &function, module,
		      fn_descs[i].fn);
      CUDA_CALL_ERET (-1, cuFuncGetAttribute, &nregs,
		      CU_FUNC_ATTRIBUTE_NUM_REGS, function);
      CUDA_CALL_ERET (-1, cuFuncGetAttribute, &mthrs,
		      CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK, function);

      targ_fns->fn = function;
      targ_fns->launch = &fn_descs[i];
      targ_fns->regs_per_thread = nregs;
      targ_fns->max_threads_per_block = mthrs;

      if (!dev->binary_version)
	{
	  int val;
	  CUDA_CALL_ERET (-1, cuFuncGetAttribute, &val,
			  CU_FUNC_ATTRIBUTE_BINARY_VERSION, function);
	  dev->binary_version = val;

	  /* These values were obtained from the CUDA Occupancy Calculator
	     spreadsheet.  */
	  if (dev->binary_version == 20
	      || dev->binary_version == 21)
	    {
	    dev->register_allocation_unit_size = 128;
	    dev->register_allocation_granularity = 2;
	    }
	  else if (dev->binary_version == 60)
	    {
	      dev->register_allocation_unit_size = 256;
	      dev->register_allocation_granularity = 2;
	    }
	  else if (dev->binary_version <= 62)
	    {
	      dev->register_allocation_unit_size = 256;
	      dev->register_allocation_granularity = 4;
	    }
	  else
	    {
	      /* Fallback to -1 to for unknown targets.  */
	      dev->register_allocation_unit_size = -1;
	      dev->register_allocation_granularity = -1;
	    }
	}

      targ_tbl->start = (uintptr_t) targ_fns;
      targ_tbl->end = targ_tbl->start + 1;
    }

  for (j = 0; j < var_entries; j++, targ_tbl++)
    {
      CUdeviceptr var;
      size_t bytes;

      CUDA_CALL_ERET (-1, cuModuleGetGlobal,
		      &var, &bytes, module, var_names[j]);

      targ_tbl->start = (uintptr_t) var;
      targ_tbl->end = targ_tbl->start + bytes;
    }

  nvptx_set_clocktick (module, dev);

  return fn_entries + var_entries;
}

/* Unload the program described by TARGET_DATA.  DEV_DATA is the
   function descriptors allocated by G_O_load_image.  */

bool
GOMP_OFFLOAD_unload_image (int ord, unsigned version, const void *target_data)
{
  struct ptx_image_data *image, **prev_p;
  struct ptx_device *dev = ptx_devices[ord];

  if (GOMP_VERSION_DEV (version) > GOMP_VERSION_NVIDIA_PTX)
    {
      GOMP_PLUGIN_error ("Offload data incompatible with PTX plugin"
			 " (expected %u, received %u)",
			 GOMP_VERSION_NVIDIA_PTX, GOMP_VERSION_DEV (version));
      return false;
    }

  bool ret = true;
  pthread_mutex_lock (&dev->image_lock);
  for (prev_p = &dev->images; (image = *prev_p) != 0; prev_p = &image->next)
    if (image->target_data == target_data)
      {
	*prev_p = image->next;
	if (CUDA_CALL_NOCHECK (cuModuleUnload, image->module) != CUDA_SUCCESS)
	  ret = false;
	free (image->fns);
	free (image);
	break;
      }
  pthread_mutex_unlock (&dev->image_lock);
  return ret;
}

void *
GOMP_OFFLOAD_alloc (int ord, size_t size)
{
  if (!nvptx_attach_host_thread_to_device (ord))
    return NULL;

  struct ptx_device *ptx_dev = ptx_devices[ord];
  struct ptx_free_block *blocks, *tmp;

  pthread_mutex_lock (&ptx_dev->free_blocks_lock);
  blocks = ptx_dev->free_blocks;
  ptx_dev->free_blocks = NULL;
  pthread_mutex_unlock (&ptx_dev->free_blocks_lock);

  while (blocks)
    {
      tmp = blocks->next;
      nvptx_free (blocks->ptr, ptx_dev);
      free (blocks);
      blocks = tmp;
    }

  return nvptx_alloc (size);
}

bool
GOMP_OFFLOAD_free (int ord, void *ptr)
{
  return (nvptx_attach_host_thread_to_device (ord)
	  && nvptx_free (ptr, ptx_devices[ord]));
}

static void
openacc_exec_internal (void (*fn) (void *), int params, size_t mapnum,
		       void **hostaddrs, void **devaddrs,
		       unsigned *dims, void *targ_mem_desc)
{
  GOMP_PLUGIN_debug (0, "  %s: prepare mappings\n", __FUNCTION__);

  void **hp = alloca (mapnum * sizeof (void *));
  CUdeviceptr dp = 0;

  if (mapnum > 0)
    {
      if (params)
	{
	  for (int i = 0; i < mapnum; i++)
	    hp[i] = (devaddrs[i] ? &devaddrs[i] : &hostaddrs[i]);
	}
      else
	{
	  for (int i = 0; i < mapnum; i++)
	    hp[i] = (devaddrs[i] ? devaddrs[i] : hostaddrs[i]);
	  CUDA_CALL_ASSERT (cuMemAlloc, &dp, mapnum * sizeof (void *));
	}
    }

  /* Copy the (device) pointers to arguments to the device (dp and hp might in
     fact have the same value on a unified-memory system).  */
  struct goacc_thread *thr = GOMP_PLUGIN_goacc_thread ();
  acc_prof_info *prof_info = thr->prof_info;
  acc_event_info data_event_info;
  acc_api_info *api_info = thr->api_info;
  bool profiling_dispatch_p = __builtin_expect (prof_info != NULL, false);
  if (profiling_dispatch_p)
    {
      prof_info->event_type = acc_ev_enqueue_upload_start;

      data_event_info.data_event.event_type = prof_info->event_type;
      data_event_info.data_event.valid_bytes
	= _ACC_DATA_EVENT_INFO_VALID_BYTES;
      data_event_info.data_event.parent_construct
	= acc_construct_parallel; //TODO
      /* Always implicit for "data mapping arguments for cuLaunchKernel".  */
      data_event_info.data_event.implicit = 1;
      data_event_info.data_event.tool_info = NULL;
      data_event_info.data_event.var_name = NULL; //TODO
      data_event_info.data_event.bytes = mapnum * sizeof (void *);
      data_event_info.data_event.host_ptr = hp;
      if (!params)
	data_event_info.data_event.device_ptr = (void *) dp;

      api_info->device_api = acc_device_api_cuda;

      GOMP_PLUGIN_goacc_profiling_dispatch (prof_info, &data_event_info,
					    api_info);
    }

  if (!params && mapnum > 0)
    CUDA_CALL_ASSERT (cuMemcpyHtoD, dp, (void *) hp,
		      mapnum * sizeof (void *));

  if (profiling_dispatch_p)
    {
      prof_info->event_type = acc_ev_enqueue_upload_end;
      data_event_info.data_event.event_type = prof_info->event_type;
      GOMP_PLUGIN_goacc_profiling_dispatch (prof_info, &data_event_info,
					    api_info);
    }

  if (params)
    nvptx_exec (fn, mapnum, hostaddrs, devaddrs, dims, targ_mem_desc,
		hp, NULL);
  else
    {
      void *kargs[1] = { &dp };
      nvptx_exec (fn, mapnum, hostaddrs, devaddrs, dims, targ_mem_desc,
		  kargs, NULL);
    }

  CUresult r = cuStreamSynchronize (NULL);
  const char *maybe_abort_msg = "(perhaps abort was called)";
  if (r == CUDA_ERROR_LAUNCH_FAILED)
    GOMP_PLUGIN_fatal ("cuStreamSynchronize error: %s %s\n", cuda_error (r),
		       maybe_abort_msg);
  else if (r != CUDA_SUCCESS)
    GOMP_PLUGIN_fatal ("cuStreamSynchronize error: %s", cuda_error (r));

  if (!params)
    CUDA_CALL_ASSERT (cuMemFree, dp);
}

void
GOMP_OFFLOAD_openacc_exec_params (void (*fn) (void *), size_t mapnum,
			   void **hostaddrs, void **devaddrs,
			   unsigned *dims, void *targ_mem_desc)
{
  openacc_exec_internal (fn, 1, mapnum, hostaddrs, devaddrs, dims,
			 targ_mem_desc);
}

void
GOMP_OFFLOAD_openacc_exec (void (*fn) (void *), size_t mapnum,
			   void **hostaddrs, void **devaddrs,
			   unsigned *dims, void *targ_mem_desc)
{
  openacc_exec_internal (fn, 0, mapnum, hostaddrs, devaddrs, dims,
			 targ_mem_desc);
}

static void
cuda_free_argmem (void *ptr)
{
  void **block = (void **) ptr;
  nvptx_free (block[0], (struct ptx_device *) block[1]);
  free (block);
}

static void
openacc_async_exec_internal (void (*fn) (void *), int params, size_t mapnum,
			     void **hostaddrs, void **devaddrs,
			     unsigned *dims, void *targ_mem_desc,
			     struct goacc_asyncqueue *aq)
{
  GOMP_PLUGIN_debug (0, "  %s: prepare mappings\n", __FUNCTION__);

  void **hp = NULL;
  CUdeviceptr dp = 0;
  void **block = NULL;

  if (mapnum > 0)
    {
      if (params)
	{
	  hp = alloca (sizeof (void *) * mapnum);
	  for (int i = 0; i < mapnum; i++)
	    hp[i] = (devaddrs[i] ? &devaddrs[i] : &hostaddrs[i]);
	}
      else
	{
	  block = (void **) GOMP_PLUGIN_malloc ((mapnum + 2) * sizeof (void *));
	  hp = block + 2;
	  for (int i = 0; i < mapnum; i++)
	    hp[i] = (devaddrs[i] ? devaddrs[i] : hostaddrs[i]);
	  CUDA_CALL_ASSERT (cuMemAlloc, &dp, mapnum * sizeof (void *));
	}
    }

  /* Copy the (device) pointers to arguments to the device (dp and hp might in
     fact have the same value on a unified-memory system).  */
  struct goacc_thread *thr = GOMP_PLUGIN_goacc_thread ();
  acc_prof_info *prof_info = thr->prof_info;
  acc_event_info data_event_info;
  acc_api_info *api_info = thr->api_info;
  bool profiling_dispatch_p = __builtin_expect (prof_info != NULL, false);
  if (profiling_dispatch_p)
    {
      prof_info->event_type = acc_ev_enqueue_upload_start;

      data_event_info.data_event.event_type = prof_info->event_type;
      data_event_info.data_event.valid_bytes
	= _ACC_DATA_EVENT_INFO_VALID_BYTES;
      data_event_info.data_event.parent_construct
	= acc_construct_parallel; //TODO
      /* Always implicit for "data mapping arguments for cuLaunchKernel".  */
      data_event_info.data_event.implicit = 1;
      data_event_info.data_event.tool_info = NULL;
      data_event_info.data_event.var_name = NULL; //TODO
      data_event_info.data_event.bytes = mapnum * sizeof (void *);
      data_event_info.data_event.host_ptr = hp;
      if (!params)
	data_event_info.data_event.device_ptr = (void *) dp;

      api_info->device_api = acc_device_api_cuda;

      GOMP_PLUGIN_goacc_profiling_dispatch (prof_info, &data_event_info,
					    api_info);
    }

  if (!params && mapnum > 0)
    {
      CUDA_CALL_ASSERT (cuMemcpyHtoDAsync, dp, (void *) hp,
			mapnum * sizeof (void *), aq->cuda_stream);
      block[0] = (void *) dp;

      struct goacc_thread *thr = GOMP_PLUGIN_goacc_thread ();
      struct nvptx_thread *nvthd = (struct nvptx_thread *) thr->target_tls;
      block[1] = (void *) nvthd->ptx_dev;
    }

  if (profiling_dispatch_p)
    {
      prof_info->event_type = acc_ev_enqueue_upload_end;
      data_event_info.data_event.event_type = prof_info->event_type;
      GOMP_PLUGIN_goacc_profiling_dispatch (prof_info, &data_event_info,
					    api_info);
    }

  if (params)
    nvptx_exec (fn, mapnum, hostaddrs, devaddrs, dims, targ_mem_desc,
		hp, aq->cuda_stream);
  else
    {
      void *kargs[1] = { &dp };
      nvptx_exec (fn, mapnum, hostaddrs, devaddrs, dims, targ_mem_desc,
		  kargs, aq->cuda_stream);
    }

  if (!params && mapnum > 0)
    GOMP_OFFLOAD_openacc_async_queue_callback (aq, cuda_free_argmem, block);
}

void
GOMP_OFFLOAD_openacc_async_exec_params (void (*fn) (void *), size_t mapnum,
				 void **hostaddrs, void **devaddrs,
				 unsigned *dims, void *targ_mem_desc,
				 struct goacc_asyncqueue *aq)
{
  openacc_async_exec_internal (fn, 1, mapnum, hostaddrs, devaddrs, dims,
			       targ_mem_desc, aq);
}

void
GOMP_OFFLOAD_openacc_async_exec (void (*fn) (void *), size_t mapnum,
				 void **hostaddrs, void **devaddrs,
				 unsigned *dims, void *targ_mem_desc,
				 struct goacc_asyncqueue *aq)
{
  openacc_async_exec_internal (fn, 0, mapnum, hostaddrs, devaddrs, dims,
			       targ_mem_desc, aq);
}


void *
GOMP_OFFLOAD_openacc_create_thread_data (int ord)
{
  struct ptx_device *ptx_dev;
  struct nvptx_thread *nvthd
    = GOMP_PLUGIN_malloc (sizeof (struct nvptx_thread));
  CUcontext thd_ctx;

  ptx_dev = ptx_devices[ord];

  assert (ptx_dev);

  CUDA_CALL_ASSERT (cuCtxGetCurrent, &thd_ctx);

  assert (ptx_dev->ctx);

  if (!thd_ctx)
    CUDA_CALL_ASSERT (cuCtxPushCurrent, ptx_dev->ctx);

  nvthd->ptx_dev = ptx_dev;

  return (void *) nvthd;
}

void
GOMP_OFFLOAD_openacc_destroy_thread_data (void *data)
{
  free (data);
}

void *
GOMP_OFFLOAD_openacc_cuda_get_current_device (void)
{
  return nvptx_get_current_cuda_device ();
}

void *
GOMP_OFFLOAD_openacc_cuda_get_current_context (void)
{
  return nvptx_get_current_cuda_context ();
}

/* NOTE: This returns a CUstream, not a ptx_stream pointer.  */
void *
GOMP_OFFLOAD_openacc_cuda_get_stream (struct goacc_asyncqueue *aq)
{
  return (void *) aq->cuda_stream;
}

/* NOTE: This takes a CUstream, not a ptx_stream pointer.  */
int
GOMP_OFFLOAD_openacc_cuda_set_stream (struct goacc_asyncqueue *aq, void *stream)
{
  if (aq->cuda_stream)
    {
      CUDA_CALL_ASSERT (cuStreamSynchronize, aq->cuda_stream);
      CUDA_CALL_ASSERT (cuStreamDestroy, aq->cuda_stream);
    }

  aq->cuda_stream = (CUstream) stream;
  return 1;
}

struct goacc_asyncqueue *
GOMP_OFFLOAD_openacc_async_construct (void)
{
  struct goacc_asyncqueue *aq
    = GOMP_PLUGIN_malloc (sizeof (struct goacc_asyncqueue));
  CUDA_CALL_ASSERT (cuStreamCreate, &aq->cuda_stream, CU_STREAM_DEFAULT);
  return aq;
}

bool
GOMP_OFFLOAD_openacc_async_destruct (struct goacc_asyncqueue *aq)
{
  CUDA_CALL_ERET (false, cuStreamDestroy, aq->cuda_stream);
  free (aq);
  return true;
}

int
GOMP_OFFLOAD_openacc_async_test (struct goacc_asyncqueue *aq)
{
  CUresult r = cuStreamQuery (aq->cuda_stream);
  if (r == CUDA_SUCCESS)
    return 1;
  if (r == CUDA_ERROR_NOT_READY)
    return 0;

  GOMP_PLUGIN_error ("cuStreamQuery error: %s", cuda_error (r));
  return -1;
}

void
GOMP_OFFLOAD_openacc_async_synchronize (struct goacc_asyncqueue *aq)
{
  CUDA_CALL_ASSERT (cuStreamSynchronize, aq->cuda_stream);
}

void
GOMP_OFFLOAD_openacc_async_serialize (struct goacc_asyncqueue *aq1,
				      struct goacc_asyncqueue *aq2)
{
  CUevent e;
  CUDA_CALL_ASSERT (cuEventCreate, &e, CU_EVENT_DISABLE_TIMING);
  CUDA_CALL_ASSERT (cuEventRecord, e, aq1->cuda_stream);
  CUDA_CALL_ASSERT (cuStreamWaitEvent, aq2->cuda_stream, e, 0);
}

static void
cuda_callback_wrapper (CUstream stream, CUresult res, void *ptr)
{
  if (res != CUDA_SUCCESS)
    GOMP_PLUGIN_fatal ("%s error: %s", __FUNCTION__, cuda_error (res));
  struct nvptx_callback *cb = (struct nvptx_callback *) ptr;
  cb->fn (cb->ptr);
  free (ptr);
}

void
GOMP_OFFLOAD_openacc_async_queue_callback (struct goacc_asyncqueue *aq,
					   void (*callback_fn)(void *),
					   void *userptr)
{
  struct nvptx_callback *b = GOMP_PLUGIN_malloc (sizeof (*b));
  b->fn = callback_fn;
  b->ptr = userptr;
  b->aq = aq;
  CUDA_CALL_ASSERT (cuStreamAddCallback, aq->cuda_stream,
		    cuda_callback_wrapper, (void *) b, 0);
}

static bool
cuda_memcpy_sanity_check (const void *h, const void *d, size_t s)
{
  CUdeviceptr pb;
  size_t ps;
  if (!s)
    return true;
  if (!d)
    {
      GOMP_PLUGIN_error ("invalid device address");
      return false;
    }
  CUDA_CALL (cuMemGetAddressRange, &pb, &ps, (CUdeviceptr) d);
  if (!pb)
    {
      GOMP_PLUGIN_error ("invalid device address");
      return false;
    }
  if (!h)
    {
      GOMP_PLUGIN_error ("invalid host address");
      return false;
    }
  if (d == h)
    {
      GOMP_PLUGIN_error ("invalid host or device address");
      return false;
    }
  if ((void *)(d + s) > (void *)(pb + ps))
    {
      GOMP_PLUGIN_error ("invalid size");
      return false;
    }
  return true;
}

bool
GOMP_OFFLOAD_host2dev (int ord, void *dst, const void *src, size_t n)
{
  if (!nvptx_attach_host_thread_to_device (ord)
      || !cuda_memcpy_sanity_check (src, dst, n))
    return false;
  CUDA_CALL (cuMemcpyHtoD, (CUdeviceptr) dst, src, n);
  return true;
}

bool
GOMP_OFFLOAD_dev2host (int ord, void *dst, const void *src, size_t n)
{
  if (!nvptx_attach_host_thread_to_device (ord)
      || !cuda_memcpy_sanity_check (dst, src, n))
    return false;
  CUDA_CALL (cuMemcpyDtoH, dst, (CUdeviceptr) src, n);
  return true;
}

bool
GOMP_OFFLOAD_dev2dev (int ord, void *dst, const void *src, size_t n)
{
  CUDA_CALL (cuMemcpyDtoDAsync, (CUdeviceptr) dst, (CUdeviceptr) src, n, NULL);
  return true;
}

bool
GOMP_OFFLOAD_openacc_async_host2dev (int ord, void *dst, const void *src,
				     size_t n, struct goacc_asyncqueue *aq)
{
  if (!nvptx_attach_host_thread_to_device (ord)
      || !cuda_memcpy_sanity_check (src, dst, n))
    return false;
  CUDA_CALL (cuMemcpyHtoDAsync, (CUdeviceptr) dst, src, n, aq->cuda_stream);
  return true;
}

bool
GOMP_OFFLOAD_openacc_async_dev2host (int ord, void *dst, const void *src,
				     size_t n, struct goacc_asyncqueue *aq)
{
  if (!nvptx_attach_host_thread_to_device (ord)
      || !cuda_memcpy_sanity_check (dst, src, n))
    return false;
  CUDA_CALL (cuMemcpyDtoHAsync, dst, (CUdeviceptr) src, n, aq->cuda_stream);
  return true;
}

/* Adjust launch dimensions: pick good values for number of blocks and warps
   and ensure that number of warps does not exceed CUDA limits as well as GCC's
   own limits.  */

static void
nvptx_adjust_launch_bounds (struct targ_fn_descriptor *fn,
			    struct ptx_device *ptx_dev,
			    int *teams_p, int *threads_p)
{
  int max_warps_block = fn->max_threads_per_block / 32;
  /* Maximum 32 warps per block is an implementation limit in NVPTX backend
     and libgcc, which matches documented limit of all GPUs as of 2015.  */
  if (max_warps_block > 32)
    max_warps_block = 32;
  if (*threads_p <= 0)
    *threads_p = 8;
  if (*threads_p > max_warps_block)
    *threads_p = max_warps_block;

  int regs_per_block = fn->regs_per_thread * 32 * *threads_p;
  /* This is an estimate of how many blocks the device can host simultaneously.
     Actual limit, which may be lower, can be queried with "occupancy control"
     driver interface (since CUDA 6.0).  */
  int max_blocks = ptx_dev->regs_per_sm / regs_per_block * ptx_dev->num_sms;
  if (*teams_p <= 0 || *teams_p > max_blocks)
    *teams_p = max_blocks;
}

/* Return the size of per-warp stacks (see gcc -msoft-stack) to use for OpenMP
   target regions.  */

static size_t
nvptx_stacks_size ()
{
  return 128 * 1024;
}

/* Return contiguous storage for NUM stacks, each SIZE bytes.  */

static void *
nvptx_stacks_alloc (size_t size, int num)
{
  CUdeviceptr stacks;
  CUresult r = CUDA_CALL_NOCHECK (cuMemAlloc, &stacks, size * num);
  if (r != CUDA_SUCCESS)
    GOMP_PLUGIN_fatal ("cuMemAlloc error: %s", cuda_error (r));
  return (void *) stacks;
}

/* Release storage previously allocated by nvptx_stacks_alloc.  */

static void
nvptx_stacks_free (void *p, int num)
{
  CUresult r = CUDA_CALL_NOCHECK (cuMemFree, (CUdeviceptr) p);
  if (r != CUDA_SUCCESS)
    GOMP_PLUGIN_fatal ("cuMemFree error: %s", cuda_error (r));
}

void
GOMP_OFFLOAD_run (int ord, void *tgt_fn, void *tgt_vars, void **args)
{
  CUfunction function = ((struct targ_fn_descriptor *) tgt_fn)->fn;
  CUresult r;
  struct ptx_device *ptx_dev = ptx_devices[ord];
  const char *maybe_abort_msg = "(perhaps abort was called)";
  int teams = 0, threads = 0;

  if (!args)
    GOMP_PLUGIN_fatal ("No target arguments provided");
  while (*args)
    {
      intptr_t id = (intptr_t) *args++, val;
      if (id & GOMP_TARGET_ARG_SUBSEQUENT_PARAM)
	val = (intptr_t) *args++;
      else
        val = id >> GOMP_TARGET_ARG_VALUE_SHIFT;
      if ((id & GOMP_TARGET_ARG_DEVICE_MASK) != GOMP_TARGET_ARG_DEVICE_ALL)
	continue;
      val = val > INT_MAX ? INT_MAX : val;
      id &= GOMP_TARGET_ARG_ID_MASK;
      if (id == GOMP_TARGET_ARG_NUM_TEAMS)
	teams = val;
      else if (id == GOMP_TARGET_ARG_THREAD_LIMIT)
	threads = val;
    }
  nvptx_adjust_launch_bounds (tgt_fn, ptx_dev, &teams, &threads);

  size_t stack_size = nvptx_stacks_size ();
  void *stacks = nvptx_stacks_alloc (stack_size, teams * threads);
  void *fn_args[] = {tgt_vars, stacks, (void *) stack_size};
  size_t fn_args_size = sizeof fn_args;
  void *config[] = {
    CU_LAUNCH_PARAM_BUFFER_POINTER, fn_args,
    CU_LAUNCH_PARAM_BUFFER_SIZE, &fn_args_size,
    CU_LAUNCH_PARAM_END
  };
  r = CUDA_CALL_NOCHECK (cuLaunchKernel, function, teams, 1, 1,
			 32, threads, 1, 0, NULL, NULL, config);
  if (r != CUDA_SUCCESS)
    GOMP_PLUGIN_fatal ("cuLaunchKernel error: %s", cuda_error (r));

  r = CUDA_CALL_NOCHECK (cuCtxSynchronize, );
  if (r == CUDA_ERROR_LAUNCH_FAILED)
    GOMP_PLUGIN_fatal ("cuCtxSynchronize error: %s %s\n", cuda_error (r),
		       maybe_abort_msg);
  else if (r != CUDA_SUCCESS)
    GOMP_PLUGIN_fatal ("cuCtxSynchronize error: %s", cuda_error (r));
  nvptx_stacks_free (stacks, teams * threads);
}

void
GOMP_OFFLOAD_async_run (int ord, void *tgt_fn, void *tgt_vars, void **args,
			void *async_data)
{
  GOMP_PLUGIN_fatal ("GOMP_OFFLOAD_async_run unimplemented");
}
