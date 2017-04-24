/* pocl-cuda.c - driver for CUDA devices

   Copyright (c) 2016-2017 James Price / University of Bristol

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to
   deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#include "config.h"

#include "common.h"
#include "devices.h"
#include "pocl-cuda.h"
#include "pocl-ptx-gen.h"
#include "pocl_cache.h"
#include "pocl_file_util.h"
#include "pocl_runtime_config.h"
#include "pocl_util.h"

#include <string.h>

#include <cuda.h>

typedef struct pocl_cuda_device_data_s
{
  CUdevice device;
  CUcontext context;
} pocl_cuda_device_data_t;

static void
pocl_cuda_abort_on_error (CUresult result, unsigned line, const char *func,
                          const char *code, const char *api)
{
  if (result != CUDA_SUCCESS)
    {
      const char *err_name;
      const char *err_string;
      cuGetErrorName (result, &err_name);
      cuGetErrorString (result, &err_string);
      POCL_MSG_PRINT2 (func, line, "Error during %s\n", api);
      POCL_ABORT ("%s: %s\n", err_name, err_string);
    }
}

#define CUDA_CHECK(result, api)                                               \
  pocl_cuda_abort_on_error (result, __LINE__, __FUNCTION__, #result, api);

void
pocl_cuda_init_device_ops (struct pocl_device_ops *ops)
{
  pocl_basic_init_device_ops (ops);

  ops->device_name = "CUDA";
  ops->init_device_infos = pocl_cuda_init_device_infos;
  ops->probe = pocl_cuda_probe;
  ops->uninit = pocl_cuda_uninit;
  ops->init = pocl_cuda_init;
  ops->alloc_mem_obj = pocl_cuda_alloc_mem_obj;
  ops->free = pocl_cuda_free;
  ops->compile_kernel = pocl_cuda_compile_kernel;
  ops->read = pocl_cuda_read;
  ops->read_rect = pocl_cuda_read_rect;
  ops->write = pocl_cuda_write;
  ops->write_rect = pocl_cuda_write_rect;
  ops->copy = pocl_cuda_copy;
  ops->copy_rect = pocl_cuda_copy_rect;
  ops->map_mem = pocl_cuda_map_mem;
  ops->unmap_mem = pocl_cuda_unmap_mem;

  ops->run = NULL;
  ops->submit = pocl_cuda_submit;
  ops->join = pocl_cuda_join;
  ops->flush = pocl_cuda_flush;

  /* TODO: implement remaining ops functions: */
  /* get_timer_value */
  /* notify */
  /* broadcast */
  /* wait_event */
  /* update_event */
  /* free_event_data */
}

void
pocl_cuda_init (cl_device_id device, const char *parameters)
{
  CUresult result;

  result = cuInit (0);
  CUDA_CHECK (result, "cuInit");

  if (device->data)
    return;

  pocl_cuda_device_data_t *data = malloc (sizeof (pocl_cuda_device_data_t));
  result = cuDeviceGet (&data->device, 0);
  CUDA_CHECK (result, "cuDeviceGet");

  // Get specific device name
  device->long_name = device->short_name = malloc (256 * sizeof (char));
  cuDeviceGetName (device->long_name, 256, data->device);

  // Get other device properties
  cuDeviceGetAttribute ((int *)&device->max_work_group_size,
                        CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK,
                        data->device);
  cuDeviceGetAttribute ((int *)(device->max_work_item_sizes + 0),
                        CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X, data->device);
  cuDeviceGetAttribute ((int *)(device->max_work_item_sizes + 1),
                        CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y, data->device);
  cuDeviceGetAttribute ((int *)(device->max_work_item_sizes + 2),
                        CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z, data->device);
  cuDeviceGetAttribute (
      (int *)&device->local_mem_size,
      CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR, data->device);
  cuDeviceGetAttribute ((int *)&device->max_compute_units,
                        CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT,
                        data->device);
  cuDeviceGetAttribute ((int *)&device->max_clock_frequency,
                        CU_DEVICE_ATTRIBUTE_CLOCK_RATE, data->device);
  cuDeviceGetAttribute ((int *)&device->error_correction_support,
                        CU_DEVICE_ATTRIBUTE_ECC_ENABLED, data->device);
  cuDeviceGetAttribute ((int *)&device->host_unified_memory,
                        CU_DEVICE_ATTRIBUTE_INTEGRATED, data->device);
  cuDeviceGetAttribute ((int *)&device->max_constant_buffer_size,
                        CU_DEVICE_ATTRIBUTE_TOTAL_CONSTANT_MEMORY,
                        data->device);

  device->preferred_vector_width_char = 1;
  device->preferred_vector_width_short = 1;
  device->preferred_vector_width_int = 1;
  device->preferred_vector_width_long = 1;
  device->preferred_vector_width_float = 1;
  device->preferred_vector_width_double = 1;
  device->preferred_vector_width_half = 0;
  device->native_vector_width_char = 1;
  device->native_vector_width_short = 1;
  device->native_vector_width_int = 1;
  device->native_vector_width_long = 1;
  device->native_vector_width_float = 1;
  device->native_vector_width_double = 1;
  device->native_vector_width_half = 0;

  device->single_fp_config = CL_FP_ROUND_TO_NEAREST | CL_FP_ROUND_TO_ZERO
                             | CL_FP_ROUND_TO_INF | CL_FP_FMA | CL_FP_INF_NAN
                             | CL_FP_DENORM;
  device->double_fp_config = CL_FP_ROUND_TO_NEAREST | CL_FP_ROUND_TO_ZERO
                             | CL_FP_ROUND_TO_INF | CL_FP_FMA | CL_FP_INF_NAN
                             | CL_FP_DENORM;

  device->local_mem_type = CL_LOCAL;
  device->host_unified_memory = 0;

  // Get GPU architecture name
  int sm_maj, sm_min;
  cuDeviceGetAttribute (&sm_maj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
                        data->device);
  cuDeviceGetAttribute (&sm_min, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,
                        data->device);
  char *gpu_arch = malloc (16 * sizeof (char));
  snprintf (gpu_arch, 16, "sm_%d%d", sm_maj, sm_min);
  device->llvm_cpu = pocl_get_string_option ("POCL_CUDA_GPU_ARCH", gpu_arch);
  POCL_MSG_PRINT_INFO ("[CUDA] GPU architecture = %s\n", device->llvm_cpu);

  // Create context
  result = cuCtxCreate (&data->context, CU_CTX_MAP_HOST, data->device);
  CUDA_CHECK (result, "cuCtxCreate");

  // Get global memory size
  size_t memfree, memtotal;
  result = cuMemGetInfo (&memfree, &memtotal);
  device->max_mem_alloc_size = max (memtotal / 4, 128 * 1024 * 1024);
  device->global_mem_size = memtotal;

  device->data = data;
}

void
pocl_cuda_init_device_infos (struct _cl_device_id *dev)
{
  pocl_basic_init_device_infos (dev);

  dev->type = CL_DEVICE_TYPE_GPU;
  dev->address_bits = (sizeof (void *) * 8);
  dev->llvm_target_triplet = (sizeof (void *) == 8) ? "nvptx64" : "nvptx";
  dev->spmd = CL_TRUE;
  dev->workgroup_pass = CL_FALSE;
  dev->execution_capabilities = CL_EXEC_KERNEL;

  dev->global_as_id = 1;
  dev->local_as_id = 3;
  dev->constant_as_id = 1;

  // TODO: Get images working
  dev->image_support = CL_FALSE;
}

unsigned int
pocl_cuda_probe (struct pocl_device_ops *ops)
{
  int env_count = pocl_device_get_env_count (ops->device_name);

  // TODO: Check how many CUDA device available (if any)

  if (env_count < 0)
    return 1;

  return env_count;
}

void
pocl_cuda_uninit (cl_device_id device)
{
  pocl_cuda_device_data_t *data = device->data;

  cuCtxDestroy (data->context);

  POCL_MEM_FREE (data);
  device->data = NULL;

  POCL_MEM_FREE (device->long_name);
}

cl_int
pocl_cuda_alloc_mem_obj (cl_device_id device, cl_mem mem_obj, void *host_ptr)
{
  cuCtxSetCurrent (((pocl_cuda_device_data_t *)device->data)->context);

  CUresult result;
  void *b = NULL;

  /* if memory for this global memory is not yet allocated -> do it */
  if (mem_obj->device_ptrs[device->global_mem_id].mem_ptr == NULL)
    {
      cl_mem_flags flags = mem_obj->flags;

      if (flags & CL_MEM_USE_HOST_PTR)
        {
#if defined __arm__
          // cuMemHostRegister is not supported on ARN
          // Allocate device memory and perform explicit copies
          // before and after running a kernel
          result = cuMemAlloc ((CUdeviceptr *)&b, mem_obj->size);
          CUDA_CHECK (result, "cuMemAlloc");
#else
          result = cuMemHostRegister (host_ptr, mem_obj->size,
                                      CU_MEMHOSTREGISTER_DEVICEMAP);
          if (result != CUDA_SUCCESS
              && result != CUDA_ERROR_HOST_MEMORY_ALREADY_REGISTERED)
            CUDA_CHECK (result, "cuMemHostRegister");
          result = cuMemHostGetDevicePointer ((CUdeviceptr *)&b, host_ptr, 0);
          CUDA_CHECK (result, "cuMemHostGetDevicePointer");
#endif
        }
      else if (flags & CL_MEM_ALLOC_HOST_PTR)
        {
          result = cuMemHostAlloc (&mem_obj->mem_host_ptr, mem_obj->size,
                                   CU_MEMHOSTREGISTER_DEVICEMAP);
          CUDA_CHECK (result, "cuMemHostAlloc");
          result = cuMemHostGetDevicePointer ((CUdeviceptr *)&b,
                                              mem_obj->mem_host_ptr, 0);
          CUDA_CHECK (result, "cuMemHostGetDevicePointer");
        }
      else
        {
          result = cuMemAlloc ((CUdeviceptr *)&b, mem_obj->size);
          if (result != CUDA_SUCCESS)
            {
              const char *err;
              cuGetErrorName (result, &err);
              POCL_MSG_PRINT2 (__FUNCTION__, __LINE__,
                               "-> Failed to allocate memory: %s\n", err);
              return CL_MEM_OBJECT_ALLOCATION_FAILURE;
            }
        }

      if (flags & CL_MEM_COPY_HOST_PTR)
        {
          result = cuMemcpyHtoD ((CUdeviceptr)b, host_ptr, mem_obj->size);
          CUDA_CHECK (result, "cuMemcpyHtoD");
        }

      mem_obj->device_ptrs[device->global_mem_id].mem_ptr = b;
      mem_obj->device_ptrs[device->global_mem_id].global_mem_id
          = device->global_mem_id;
    }

  /* copy already allocated global mem info to devices own slot */
  mem_obj->device_ptrs[device->dev_id]
      = mem_obj->device_ptrs[device->global_mem_id];

  return CL_SUCCESS;
}

void
pocl_cuda_free (cl_device_id device, cl_mem mem_obj)
{
  cuCtxSetCurrent (((pocl_cuda_device_data_t *)device->data)->context);

  if (mem_obj->flags & CL_MEM_ALLOC_HOST_PTR)
    {
      cuMemFreeHost (mem_obj->mem_host_ptr);
      mem_obj->mem_host_ptr = NULL;
    }
  else
    {
      void *ptr = mem_obj->device_ptrs[device->dev_id].mem_ptr;
      cuMemFree ((CUdeviceptr)ptr);
    }
}

void
pocl_cuda_read (void *data, void *host_ptr, const void *device_ptr,
                size_t offset, size_t cb)
{
  CUresult result
      = cuMemcpyDtoH (host_ptr, (CUdeviceptr) (device_ptr + offset), cb);
  CUDA_CHECK (result, "cuMemcpyDtoH");
}

void
pocl_cuda_write (void *data, const void *host_ptr, void *device_ptr,
                 size_t offset, size_t cb)
{
  CUresult result
      = cuMemcpyHtoD ((CUdeviceptr) (device_ptr + offset), host_ptr, cb);
  CUDA_CHECK (result, "cuMemcpyHtoD");
}

void
pocl_cuda_copy (void *data, const void *src_ptr, size_t src_offset,
                void *__restrict__ dst_ptr, size_t dst_offset, size_t cb)
{
  if (src_ptr == dst_ptr)
    return;

  CUresult result = cuMemcpyDtoD ((CUdeviceptr) (dst_ptr + dst_offset),
                                  (CUdeviceptr) (src_ptr + src_offset), cb);
  CUDA_CHECK (result, "cuMemcpyDtoD");
}

void
pocl_cuda_read_rect (void *data, void *__restrict__ const host_ptr,
                     void *__restrict__ const device_ptr,
                     const size_t *__restrict__ const buffer_origin,
                     const size_t *__restrict__ const host_origin,
                     const size_t *__restrict__ const region,
                     size_t const buffer_row_pitch,
                     size_t const buffer_slice_pitch,
                     size_t const host_row_pitch,
                     size_t const host_slice_pitch)
{
  CUDA_MEMCPY3D params = { 0 };

  params.WidthInBytes = region[0];
  params.Height = region[1];
  params.Depth = region[2];

  params.dstMemoryType = CU_MEMORYTYPE_HOST;
  params.dstHost = host_ptr;
  params.dstXInBytes = host_origin[0];
  params.dstY = host_origin[1];
  params.dstZ = host_origin[2];
  params.dstPitch = host_row_pitch;
  params.dstHeight = host_slice_pitch / host_row_pitch;

  params.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  params.srcDevice = (CUdeviceptr)device_ptr;
  params.srcXInBytes = buffer_origin[0];
  params.srcY = buffer_origin[1];
  params.srcZ = buffer_origin[2];
  params.srcPitch = buffer_row_pitch;
  params.srcHeight = buffer_slice_pitch / buffer_row_pitch;

  CUresult result = cuMemcpy3D (&params);
  CUDA_CHECK (result, "cuMemcpy3D");
}

void
pocl_cuda_write_rect (void *data, const void *__restrict__ const host_ptr,
                      void *__restrict__ const device_ptr,
                      const size_t *__restrict__ const buffer_origin,
                      const size_t *__restrict__ const host_origin,
                      const size_t *__restrict__ const region,
                      size_t const buffer_row_pitch,
                      size_t const buffer_slice_pitch,
                      size_t const host_row_pitch,
                      size_t const host_slice_pitch)
{
  CUDA_MEMCPY3D params = { 0 };

  params.WidthInBytes = region[0];
  params.Height = region[1];
  params.Depth = region[2];

  params.srcMemoryType = CU_MEMORYTYPE_HOST;
  params.srcHost = host_ptr;
  params.srcXInBytes = host_origin[0];
  params.srcY = host_origin[1];
  params.srcZ = host_origin[2];
  params.srcPitch = host_row_pitch;
  params.srcHeight = host_slice_pitch / host_row_pitch;

  params.dstMemoryType = CU_MEMORYTYPE_DEVICE;
  params.dstDevice = (CUdeviceptr)device_ptr;
  params.dstXInBytes = buffer_origin[0];
  params.dstY = buffer_origin[1];
  params.dstZ = buffer_origin[2];
  params.dstPitch = buffer_row_pitch;
  params.dstHeight = buffer_slice_pitch / buffer_row_pitch;

  CUresult result = cuMemcpy3D (&params);
  CUDA_CHECK (result, "cuMemcpy3D");
}

void
pocl_cuda_copy_rect (void *data, const void *__restrict const src_ptr,
                     void *__restrict__ const dst_ptr,
                     const size_t *__restrict__ const src_origin,
                     const size_t *__restrict__ const dst_origin,
                     const size_t *__restrict__ const region,
                     size_t const src_row_pitch, size_t const src_slice_pitch,
                     size_t const dst_row_pitch, size_t const dst_slice_pitch)
{
  CUDA_MEMCPY3D params = { 0 };

  params.WidthInBytes = region[0];
  params.Height = region[1];
  params.Depth = region[2];

  params.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  params.srcDevice = (CUdeviceptr)src_ptr;
  params.srcXInBytes = src_origin[0];
  params.srcY = src_origin[1];
  params.srcZ = src_origin[2];
  params.srcPitch = src_row_pitch;
  params.srcHeight = src_slice_pitch / src_row_pitch;

  params.dstMemoryType = CU_MEMORYTYPE_DEVICE;
  params.dstDevice = (CUdeviceptr)dst_ptr;
  params.dstXInBytes = dst_origin[0];
  params.dstY = dst_origin[1];
  params.dstZ = dst_origin[2];
  params.dstPitch = dst_row_pitch;
  params.dstHeight = dst_slice_pitch / dst_row_pitch;

  CUresult result = cuMemcpy3D (&params);
  CUDA_CHECK (result, "cuMemcpy3D");
}

void *
pocl_cuda_map_mem (void *data, void *buf_ptr, size_t offset, size_t size,
                   void *host_ptr)
{
  if (host_ptr != NULL)
    return host_ptr;

  void *ptr = malloc (size);
  CUresult result = cuMemcpyDtoH (ptr, (CUdeviceptr) (buf_ptr + offset), size);
  CUDA_CHECK (result, "cuMemcpyDtoH");
  return ptr;
}

void *
pocl_cuda_unmap_mem (void *data, void *host_ptr, void *device_start_ptr,
                     size_t offset, size_t size)
{
  if (host_ptr)
    {
      // TODO: offset?
      CUresult result = cuMemcpyHtoD (
          (CUdeviceptr) (device_start_ptr + offset), host_ptr, size);
      CUDA_CHECK (result, "cuMemcpyHtoD");
      free (host_ptr);
    }
  return NULL;
}

static void
load_or_generate_kernel (cl_kernel kernel, cl_device_id device)
{
  cuCtxSetCurrent (((pocl_cuda_device_data_t *)device->data)->context);

  CUresult result;

  // Check if we already have a compiled kernel function
  if (kernel->data)
    return;

  char bc_filename[POCL_FILENAME_LENGTH];
  unsigned device_i = pocl_cl_device_to_index (kernel->program, device);
  pocl_cache_work_group_function_path (bc_filename, kernel->program, device_i,
                                       kernel, 0, 0, 0);

  char ptx_filename[POCL_FILENAME_LENGTH];
  strcpy (ptx_filename, bc_filename);
  strncat (ptx_filename, ".ptx", POCL_FILENAME_LENGTH - 1);

  if (!pocl_exists (ptx_filename))
    {
      // Generate PTX from LLVM bitcode
      if (pocl_ptx_gen (bc_filename, ptx_filename, kernel->name,
                        device->llvm_cpu))
        POCL_ABORT ("pocl-cuda: failed to generate PTX\n");
    }

  // Load PTX module
  // TODO: When can we unload the module?
  CUmodule module;
  result = cuModuleLoad (&module, ptx_filename);
  CUDA_CHECK (result, "cuModuleLoad");

  // Get kernel function
  CUfunction function;
  result = cuModuleGetFunction (&function, module, kernel->name);
  CUDA_CHECK (result, "cuModuleGetFunction");

  kernel->data = function;
}

void
pocl_cuda_compile_kernel (_cl_command_node *cmd, cl_kernel kernel,
                          cl_device_id device)
{
  load_or_generate_kernel (kernel, device);
}

void
pocl_cuda_submit (_cl_command_node *node, cl_command_queue cq)
{
  cuCtxSetCurrent (((pocl_cuda_device_data_t *)cq->device->data)->context);

  CUresult result;

  POCL_UPDATE_EVENT_SUBMITTED (&node->event);

  if (node->type != CL_COMMAND_NDRANGE_KERNEL)
    {
      pocl_exec_command (node);
      return;
    }

  cl_device_id device = cq->device;
  cl_kernel kernel = node->command.run.kernel;
  pocl_argument *arguments = node->command.run.arguments;

  // Ensure kernel has been loaded
  load_or_generate_kernel (kernel, device);

  CUfunction function = kernel->data;

  // Prepare kernel arguments
  void *null = NULL;
  unsigned sharedMemBytes = 0;
  void *params[kernel->num_args + kernel->num_locals];
  unsigned sharedMemOffsets[kernel->num_args + kernel->num_locals];

  unsigned i;
  for (i = 0; i < kernel->num_args; i++)
    {
      pocl_argument_type type = kernel->arg_info[i].type;
      switch (type)
        {
        case POCL_ARG_TYPE_NONE:
          params[i] = arguments[i].value;
          break;
        case POCL_ARG_TYPE_POINTER:
          {
            if (kernel->arg_info[i].is_local)
              {
                sharedMemOffsets[i] = sharedMemBytes;
                params[i] = sharedMemOffsets + i;

                sharedMemBytes += arguments[i].size;
              }
            else
              {
                if (arguments[i].value)
                  {
                    cl_mem mem = *(void **)arguments[i].value;
                    params[i] = &mem->device_ptrs[device->dev_id].mem_ptr;

#if defined __arm__
                    // On ARM with USE_HOST_PTR, perform explicit copy to
                    // device
                    if (mem->flags & CL_MEM_USE_HOST_PTR)
                      {
                        cuMemcpyHtoD (*(CUdeviceptr *)(params[i]),
                                      mem->mem_host_ptr, mem->size);
                      }
#endif
                  }
                else
                  {
                    params[i] = &null;
                  }
              }
            break;
          }
        case POCL_ARG_TYPE_IMAGE:
        case POCL_ARG_TYPE_SAMPLER:
          POCL_ABORT ("Unhandled argument type for CUDA");
          break;
        }
    }

  // Deal with automatic local allocations
  // TODO: Would be better to remove arguments and make these static GEPs
  for (i = 0; i < kernel->num_locals; ++i)
    {
      sharedMemOffsets[kernel->num_args + i] = sharedMemBytes;
      sharedMemBytes += arguments[kernel->num_args + i].size;
      params[kernel->num_args + i] = sharedMemOffsets + kernel->num_args + i;
    }

  POCL_UPDATE_EVENT_RUNNING (&node->event);

  // Launch kernel
  struct pocl_context pc = node->command.run.pc;
  result = cuLaunchKernel (
      function, pc.num_groups[0], pc.num_groups[1], pc.num_groups[2],
      node->command.run.local_x, node->command.run.local_y,
      node->command.run.local_z, sharedMemBytes, NULL, params, NULL);
  CUDA_CHECK (result, "cuLaunchKernel");

#if defined __arm__
  // On ARM with USE_HOST_PTR, perform explict copies back from device
  for (i = 0; i < kernel->num_args; i++)
    {
      if (kernel->arg_info[i].type == POCL_ARG_TYPE_POINTER)
        {
          if (!kernel->arg_info[i].is_local && arguments[i].value)
            {
              cl_mem mem = *(void **)arguments[i].value;
              if (mem->flags & CL_MEM_USE_HOST_PTR)
                {
                  CUdeviceptr ptr
                      = (CUdeviceptr)mem->device_ptrs[device->dev_id].mem_ptr;
                  cuMemcpyDtoH (mem->mem_host_ptr, ptr, mem->size);
                }
            }
        }
    }
#endif

  pocl_ndrange_node_cleanup (node);

  POCL_UPDATE_EVENT_COMPLETE (&node->event);
}

void
pocl_cuda_flush (cl_device_id device, cl_command_queue cq)
{
  // TODO: Something here?
}

void
pocl_cuda_join (cl_device_id device, cl_command_queue cq)
{
  cuCtxSetCurrent (((pocl_cuda_device_data_t *)device->data)->context);

  CUresult result = cuStreamSynchronize (0);
  CUDA_CHECK (result, "cuStreamSynchronize");
}
