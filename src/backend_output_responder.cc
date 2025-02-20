// Copyright (c) 2019-2020, NVIDIA CORPORATION. All rights reserved.
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

#include "triton/backend/backend_output_responder.h"

#include "triton/backend/backend_common.h"
#include "triton/backend/backend_model.h"
#include "triton/backend/backend_model_instance.h"

namespace triton { namespace backend {

//
// BackendOutputResponder
//
BackendOutputResponder::~BackendOutputResponder()
{
  for (auto& pinned_memory : pinned_memories_) {
    LOG_IF_ERROR(
        TRITONBACKEND_MemoryManagerFree(
            memory_manager_, reinterpret_cast<void*>(pinned_memory),
            TRITONSERVER_MEMORY_CPU_PINNED, 0),
        "failed to free pinned memory");
  }
}

void
BackendOutputResponder::ProcessTensor(
    const std::string& output_name, const TRITONSERVER_DataType datatype,
    std::vector<int64_t>& batchn_shape, const char* buffer,
    const TRITONSERVER_MemoryType memory_type, const int64_t memory_type_id)
{
  // A value of CPU_PINNED indicates that pinned memory buffer is not
  // needed for this tensor. Any other value indicates that a pinned
  // memory buffer is needed when the target memory type matches
  // 'use_pinned_memory_type'.
  TRITONSERVER_MemoryType use_pinned_memory_type =
      TRITONSERVER_MEMORY_CPU_PINNED;
  if (pinned_enabled_ && (memory_type != TRITONSERVER_MEMORY_CPU_PINNED)) {
    use_pinned_memory_type = (memory_type == TRITONSERVER_MEMORY_CPU)
                                 ? TRITONSERVER_MEMORY_GPU
                                 : TRITONSERVER_MEMORY_CPU;
  }

  size_t tensor_offset = 0;
  
  /* 为每个request准备response */
  for (size_t idx = 0; idx < responses_->size(); idx++) {
    auto& request = requests_[idx];
    auto& response = (*responses_)[idx];

    // If then pending copies are from tensor buffer that is not
    // contiguous with 'response's part of that buffer, then need to
    // go ahead and perform the pending copies so that can start a
    // new contiguous region if necessary.
    if ((pending_pinned_byte_size_ > 0) &&
        (tensor_offset !=
         (pending_pinned_byte_size_ + pending_pinned_offset_))) {
      need_sync_ |= FlushPendingPinned(buffer, memory_type, memory_type_id);
    }

    // Override shape to be correct for this response.
    /* 获取当前request的真实batch_size，用于形成该request对应output的shape，从而计算该request对应output的数据大小 */
    if (max_batch_size_ != 0) {
      const char* name;
      TRITONBACKEND_RequestInputName(request, 0, &name);
      TRITONBACKEND_Input* input;
      TRITONBACKEND_RequestInput(request, name, &input);
      const int64_t* shape;
      TRITONBACKEND_InputProperties(
          input, nullptr, nullptr, &shape, nullptr, nullptr, nullptr);
      batchn_shape[0] = shape[0];
    }

    /* 当前request对应输出的数据大小 */
    const size_t tensor_byte_size = GetByteSize(datatype, batchn_shape);

    TRITONBACKEND_Output* response_output;
    if (response != nullptr) {
      uint32_t output_count;
      RESPOND_AND_SET_NULL_IF_ERROR(
          /* 获取当前response所包含的output的数量 */
          &response, TRITONBACKEND_RequestOutputCount(request, &output_count));
      if (response != nullptr) {
        /* 遍历该response所需的每个output */
        for (uint32_t output_idx = 0; output_idx < output_count; output_idx++) {
          const char* name;
          /* 取得当前output的名称 */
          RESPOND_AND_SET_NULL_IF_ERROR(
              &response,
              TRITONBACKEND_RequestOutputName(request, output_idx, &name));
          /* 根据目标output的名称找到当前正要处理的output */
          if ((response != nullptr) && (output_name == name)) {
            /* 为当前目标output在response中创建output tensor对象 */
            RESPOND_AND_SET_NULL_IF_ERROR(
                &response, TRITONBACKEND_ResponseOutput(
                               response, &response_output, name, datatype,
                               batchn_shape.data(), batchn_shape.size()));
            /* 把输出buffer的内容拷贝到response中的buffer里 */
            if (response != nullptr) {
              need_sync_ |= SetFixedSizeOutputBuffer(
                  &response, response_output, output_name, tensor_byte_size,
                  tensor_offset, buffer, memory_type, memory_type_id,
                  use_pinned_memory_type);
            }

            break;
          }
        }
      }
    }

    tensor_offset += tensor_byte_size;
  }

  // Done with the tensor, flush any pending pinned copies.
  need_sync_ |= FlushPendingPinned(buffer, memory_type, memory_type_id);
#ifdef TRITON_ENABLE_GPU
  if (need_sync_ && (event_ != nullptr)) {
    cudaEventRecord(event_, stream_);
  }
#endif  // TRITON_ENABLE_GPU
}

bool
BackendOutputResponder::Finalize()
{
#ifdef TRITON_ENABLE_GPU
  if ((!deferred_pinned_.empty()) && need_sync_) {
    if (event_ != nullptr) {
      cudaEventSynchronize(event_);
    } else {
      cudaStreamSynchronize(stream_);
    }
    need_sync_ = false;
  }
#endif  // TRITON_ENABLE_GPU

  // After the above sync all the GPU->pinned copies are complete. Any
  // deferred copies of pinned->CPU can now be done.
  for (auto& def : deferred_pinned_) {
    auto pinned_memory_type = TRITONSERVER_MEMORY_CPU_PINNED;
    int64_t pinned_memory_id = 0;
    char* pinned_buffer = def.pinned_memory_;

    size_t offset = 0;
    for (auto& pr : def.responses_) {
      auto& response = pr.first;
      auto& response_output = pr.second;

      bool cuda_used = false;
      RESPOND_AND_SET_NULL_IF_ERROR(
          response,
          CopyBuffer(
              response_output.name_, pinned_memory_type, pinned_memory_id,
              response_output.memory_type_, response_output.memory_type_id_,
              response_output.buffer_byte_size_, pinned_buffer + offset,
              const_cast<void*>(response_output.buffer_), stream_, &cuda_used));
      need_sync_ |= cuda_used;

      offset += response_output.buffer_byte_size_;
    }
  }

#ifdef TRITON_ENABLE_GPU
  // Record the new event location if deferred copies occur
  if ((!deferred_pinned_.empty()) && need_sync_ && (event_ != nullptr)) {
    cudaEventRecord(event_, stream_);
  }
#endif  // TRITON_ENABLE_GPU
  deferred_pinned_.clear();

  return need_sync_;
}

bool
BackendOutputResponder::SetFixedSizeOutputBuffer(
    TRITONBACKEND_Response** response, TRITONBACKEND_Output* response_output,
    const std::string& output_name, const size_t tensor_byte_size,
    const size_t tensor_offset, const char* tensor_buffer,
    const TRITONSERVER_MemoryType tensor_memory_type,
    const int64_t tensor_memory_type_id,
    const TRITONSERVER_MemoryType use_pinned_memory_type)
{
  void* buffer = nullptr;
  bool cuda_copy = false;

  TRITONSERVER_MemoryType actual_memory_type = tensor_memory_type;
  int64_t actual_memory_type_id = tensor_memory_type_id;

  auto err = TRITONBACKEND_OutputBuffer(
      response_output, &buffer, tensor_byte_size, &actual_memory_type,
      &actual_memory_type_id);
  if (err != nullptr) {
    RESPOND_AND_SET_NULL_IF_ERROR(response, err);
    return cuda_copy;
  }

  // If the response buffer matches the memory type that should use an
  // intermediate pinned memory buffer for the transfer, then just
  // record the response as pending and increase the size required for
  // the intermediate pinned buffer.
  if ((use_pinned_memory_type != TRITONSERVER_MEMORY_CPU_PINNED) &&
      (actual_memory_type == use_pinned_memory_type)) {
    if (pending_pinned_byte_size_ == 0) {
      pending_pinned_offset_ = tensor_offset;
    }

    pending_pinned_byte_size_ += tensor_byte_size;
    pending_pinned_outputs_.push_back(std::make_pair(
        response, OutputData(
                      output_name, buffer, tensor_byte_size, actual_memory_type,
                      actual_memory_type_id)));
  } else {
    // Direct copy without intermediate pinned memory.
    bool cuda_used = false;
    err = CopyBuffer(
        output_name, tensor_memory_type, tensor_memory_type_id,
        actual_memory_type, actual_memory_type_id, tensor_byte_size,
        tensor_buffer + tensor_offset, buffer, stream_, &cuda_used);
    cuda_copy |= cuda_used;

    if (err != nullptr) {
      RESPOND_AND_SET_NULL_IF_ERROR(response, err);
      return cuda_copy;
    }
  }

  return cuda_copy;
}

bool
BackendOutputResponder::FlushPendingPinned(
    const char* tensor_buffer, const TRITONSERVER_MemoryType tensor_memory_type,
    const int64_t tensor_memory_type_id)
{
  bool cuda_copy = false;

  // Will be copying from CPU->pinned->GPU or GPU->pinned->CPU

  // Attempt to allocate a pinned buffer to use for staging the
  // copy... if we fail to allocated the pinned buffer then we just
  // directly go CPU->GPU or GPU->CPU.
  char* pinned_memory = nullptr;
  if (pending_pinned_byte_size_ > 0) {
    TRITONSERVER_Error* err = TRITONBACKEND_MemoryManagerAllocate(
        memory_manager_, reinterpret_cast<void**>(&pinned_memory),
        TRITONSERVER_MEMORY_CPU_PINNED, 0 /* memory_type_id */,
        pending_pinned_byte_size_);
    if (err != nullptr) {
      pinned_memory = nullptr;
      TRITONSERVER_ErrorDelete(err);
    }
  }

  // If the pinned buffer wasn't actually allocated then just perform
  // a direct copy.
  if (pinned_memory == nullptr) {
    size_t offset = 0;
    for (auto& pr : pending_pinned_outputs_) {
      auto& response = pr.first;
      auto& response_output = pr.second;

      bool cuda_used = false;
      RESPOND_AND_SET_NULL_IF_ERROR(
          response,
          CopyBuffer(
              response_output.name_, tensor_memory_type, tensor_memory_type_id,
              response_output.memory_type_, response_output.memory_type_id_,
              response_output.buffer_byte_size_,
              tensor_buffer + pending_pinned_offset_ + offset,
              const_cast<void*>(response_output.buffer_), stream_, &cuda_used));
      cuda_copy |= cuda_used;

      offset += response_output.buffer_byte_size_;
    }
  }
  // We have a pinned buffer so do a single copy of a block of tensor
  // data to the pinned buffer.
  else {
    bool cuda_used = false;
    auto err = CopyBuffer(
        "pinned buffer", tensor_memory_type, tensor_memory_type_id,
        TRITONSERVER_MEMORY_CPU_PINNED, 0 /* memory_type_id */,
        pending_pinned_byte_size_, tensor_buffer + pending_pinned_offset_,
        pinned_memory, stream_, &cuda_used);
    cuda_copy |= cuda_used;

    // If something goes wrong with the copy all the pending
    // responses fail...
    if (err != nullptr) {
      for (auto& pr : pending_pinned_outputs_) {
        auto& response = pr.first;
        if (*response != nullptr) {
          LOG_IF_ERROR(
              TRITONBACKEND_ResponseSend(
                  *response, TRITONSERVER_RESPONSE_COMPLETE_FINAL, err),
              "failed to send TensorFlow error response");
          *response = nullptr;
        }
      }
      TRITONSERVER_ErrorDelete(err);
    }

    // If the copy was not async (i.e. if tensor was in CPU so a
    // CPU->CPU-PINNED copy was performed above), then the pinned
    // buffer now holds the tensor contents and we can immediately
    // issue the copies from the pinned buffer to the
    // responses.
    //
    // Otherwise the GPU->CPU-PINNED async copies are in flight and we
    // simply remember the pinned buffer and the corresponding
    // response outputs so that we can do the pinned->CPU copies in
    // finalize after we have waited for all async copies to complete.
    if (!cuda_used) {
      size_t offset = 0;
      for (auto& pr : pending_pinned_outputs_) {
        auto& response = pr.first;
        auto& response_output = pr.second;

        bool cuda_used = false;
        RESPOND_AND_SET_NULL_IF_ERROR(
            response,
            CopyBuffer(
                response_output.name_, TRITONSERVER_MEMORY_CPU_PINNED,
                0 /* memory_type_id */, response_output.memory_type_,
                response_output.memory_type_id_,
                response_output.buffer_byte_size_, pinned_memory + offset,
                const_cast<void*>(response_output.buffer_), stream_,
                &cuda_used));
        cuda_copy |= cuda_used;

        offset += response_output.buffer_byte_size_;
      }
    } else {
      deferred_pinned_.emplace_back(
          pinned_memory, pending_pinned_byte_size_,
          std::move(pending_pinned_outputs_));
    }
  }

  // Pending pinned copies are handled...
  pending_pinned_byte_size_ = 0;
  pending_pinned_offset_ = 0;
  pending_pinned_outputs_.clear();

  // Need to hold on to the allocated pinned buffer as there are still
  // copies in flight. Will delete it in finalize.
  if (pinned_memory != nullptr) {
    pinned_memories_.push_back(pinned_memory);
  }

  return cuda_copy;
}

}}  // namespace triton::backend
