// Copyright (c) 2019-2021, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#pragma once

#include <list>
#include <memory>
#include <string>
#include <vector>
#include "triton/backend/backend_memory.h"
#include "triton/core/tritonbackend.h"

#ifdef TRITON_ENABLE_GPU
#include <cuda_runtime_api.h>
#endif  // TRITON_ENABLE_GPU

namespace triton { namespace backend {

#ifndef TRITON_ENABLE_GPU
using cudaStream_t = void*;
using cudaEvent_t = void*;
#endif  // !TRITON_ENABLE_GPU

//
// BackendInputCollector
//
class BackendInputCollector {
 public:
  // The caller can optionally provide 'event' for internal synchronization
  // instead of using 'stream'.
  explicit BackendInputCollector(
      TRITONBACKEND_Request** requests, const uint32_t request_count,
      std::vector<TRITONBACKEND_Response*>* responses,
      TRITONBACKEND_MemoryManager* memory_manager, const bool pinned_enabled,
      cudaStream_t stream, cudaEvent_t event = nullptr)
      : need_sync_(false), requests_(requests), request_count_(request_count),
        responses_(responses), memory_manager_(memory_manager),
        pinned_enabled_(pinned_enabled), stream_(stream), event_(event),
        pending_pinned_byte_size_(0)
  {
  }

  ~BackendInputCollector() = default;

  // Process all requests for a named input tensor.
  void ProcessTensor(
      const char* input_name, char* buffer, const size_t buffer_byte_size,
      const TRITONSERVER_MemoryType memory_type, const int64_t memory_type_id);

  // Process all requests for a named input tensor and returns the contiguous
  // buffer of the input tensor. This overload of the function can avoid data
  // copy if the input buffer is already contiguous and the caller doesn't
  // provide a designated buffer.
  // 'buffer' is used to determine whether the input should be placed at the
  //   'buffer' provided by the caller. If 'buffer' == nullptr, the returned
  //   buffer will be managed by the BackendInputCollector object and
  //   has the same lifecycle as the BackendInputCollector object.
  // 'buffer_byte_size' is the byte size of 'buffer' if it is not nullptr.
  // 'allowed_input_types' is the ordered list of the memory type and id pairs
  //   that the returned buffer can be. It must only contain the memory type
  //   and id of 'buffer' if it is not nullptr.
  // 'dst_buffer' returns the contiguous buffer of the input tensor.
  // 'dst_memory_type' returns the memory type of 'dst_buffer'.
  // 'dst_memory_type_id' returns the memory type id of 'dst_buffer'.
  TRITONSERVER_Error* ProcessTensor(
      const char* input_name, char* buffer, const size_t buffer_byte_size,
      const std::vector<std::pair<TRITONSERVER_MemoryType, int64_t>>&
          allowed_input_types,
      const char** dst_buffer, size_t* dst_buffer_byte_size,
      TRITONSERVER_MemoryType* dst_memory_type, int64_t* dst_memory_type_id);

  // Finalize processing of all requests for all input tensors. Return
  // true if cudaMemcpyAsync is called, and the caller should call
  // should call cudaStreamSynchronize (or cudaEventSynchronize on 'event')
  // before using the data.
  bool Finalize();

 private:
  // Return whether the entire input is in a contiguous buffer. If returns true,
  // the properties of the contiguous input buffer will also be returned.
  // Otherwise, only 'buffer_byte_size' will be set and return the total byte
  // size of the input.
  bool GetInputBufferIfContiguous(
      const char* input_name, const char** buffer, size_t* buffer_byte_size,
      TRITONSERVER_MemoryType* memory_type, int64_t* memory_type_id);
  bool FlushPendingPinned(
      char* tensor_buffer, const size_t tensor_buffer_byte_size,
      const TRITONSERVER_MemoryType tensor_memory_type,
      const int64_t tensor_memory_type_id);
  bool SetFixedSizeInputTensor(
      TRITONBACKEND_Input* request_input, const size_t tensor_buffer_offset,
      char* tensor_buffer, const size_t tensor_buffer_byte_size,
      const TRITONSERVER_MemoryType tensor_memory_type,
      const int64_t tensor_memory_type_id,
      const TRITONSERVER_MemoryType use_pinned_memory_type,
      TRITONBACKEND_Response** response);

  bool need_sync_;
  TRITONBACKEND_Request** requests_;
  const uint32_t request_count_;
  std::vector<TRITONBACKEND_Response*>* responses_;
  TRITONBACKEND_MemoryManager* memory_manager_;
  const bool pinned_enabled_;
  cudaStream_t stream_;
  cudaEvent_t event_;

  using RequestsList =
      std::list<std::pair<TRITONBACKEND_Response**, TRITONBACKEND_Input*>>;

  size_t pending_pinned_byte_size_;
  size_t pending_pinned_offset_;
  RequestsList pending_pinned_inputs_;

  // managed memories that need to live over the lifetime of this
  // BackendInputCollector object.
  std::list<std::unique_ptr<BackendMemory>> backend_memories_;

  // Pinned memory buffers and the corresponding request_inputs where
  // the final copy to the tensor is deferred until Finalize() after
  // waiting for all in-flight copies.
  struct DeferredPinned {
    DeferredPinned(
        char* pinned_memory, const size_t pinned_memory_size,
        char* tensor_buffer, const size_t tensor_buffer_offset,
        const TRITONSERVER_MemoryType tensor_memory_type,
        const int64_t tensor_memory_id, RequestsList&& requests)
        : pinned_memory_(pinned_memory),
          pinned_memory_size_(pinned_memory_size),
          tensor_buffer_(tensor_buffer),
          tensor_buffer_offset_(tensor_buffer_offset),
          tensor_memory_type_(tensor_memory_type),
          tensor_memory_id_(tensor_memory_id), requests_(std::move(requests))
    {
    }

    // Holding reference to the pinned memory buffer, which is managed
    // by BackendInputCollector as 'pinned_memory'
    char* pinned_memory_;
    const size_t pinned_memory_size_;
    char* tensor_buffer_;
    const size_t tensor_buffer_offset_;
    const TRITONSERVER_MemoryType tensor_memory_type_;
    const int64_t tensor_memory_id_;
    RequestsList requests_;
  };

  std::list<DeferredPinned> deferred_pinned_;
};

}}  // namespace triton::backend
