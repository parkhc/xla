/* Copyright 2017 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/gpu_transfer_manager.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "llvm/IR/DataLayout.h"
#include "xla/literal.h"
#include "xla/service/compiler.h"
#include "xla/service/generic_transfer_manager.h"
#include "xla/service/gpu/infeed_manager.h"
#include "xla/service/gpu/outfeed_manager.h"
#include "xla/service/gpu/target_constants.h"
#include "xla/service/shaped_buffer.h"
#include "xla/service/transfer_manager.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/status_macros.h"
#include "xla/stream_executor/cuda/cuda_platform_id.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/rocm/rocm_platform_id.h"
#include "xla/stream_executor/stream_executor.h"
#include "xla/util.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {

// TODO(b/30467474) Once GPU infeed implementation settles, consider
// folding back the cpu and gpu infeed implementations into a generic
// one if possible.
GpuTransferManager::GpuTransferManager(se::Platform::Id id,
                                       unsigned pointer_size)
    : GenericTransferManager(id, pointer_size) {}

absl::Status GpuTransferManager::TransferLiteralToInfeed(
    se::StreamExecutor* executor, const LiteralSlice& literal) {
  return gpu::GetOrCreateInfeedManager(executor)->TransferLiteralToInfeed(
      executor, literal);
}

absl::Status GpuTransferManager::TransferLiteralFromOutfeed(
    se::StreamExecutor* executor, MutableBorrowingLiteral literal) {
  return gpu::GetOrCreateOutfeedManager(executor)->TransferLiteralFromOutfeed(
      executor, literal);
}

void GpuTransferManager::EnsurePinnedBuffersAllocated(
    se::StreamExecutor* executor) {
  if (pinned_chunk_ != nullptr) {
    return;
  }

  pinned_chunk_se_ = executor;
  pinned_chunk_ = executor->HostMemoryAllocate(kPinnedChunkBytes);
  static_assert(kPinnedChunkBytes % kPinnedBufferBytes == 0,
                "assumption of loop below");
  char* base = reinterpret_cast<char*>(pinned_chunk_->opaque());
  for (char* buf = base; buf < base + kPinnedChunkBytes;
       buf += kPinnedBufferBytes) {
    pinned_buffers_.push_back(buf);
  }
}

absl::Status GpuTransferManager::ReadDynamicShapes(
    se::Stream* stream, const ShapedBuffer* device_buffer,
    Shape* device_shape) {
  DCHECK(device_shape->is_dynamic());
  Shape original_device_shape = *device_shape;

  TF_ASSIGN_OR_RETURN(auto compiler,
                      Compiler::GetForPlatform(stream->parent()->platform()));
  auto shape_size_fn = compiler->ShapeSizeBytesFunction();

  // First, figure out which parts of `device_shape` are dynamic and where the
  // dynamic shapes live in GPU memory.  We'll copy the bytes at the
  // DeviceMemoryBase into the Shape*'s dimensions.
  std::vector<std::pair<se::DeviceMemoryBase, Shape*>> copies;

  TF_RETURN_IF_ERROR(device_buffer->buffers().ForEachElementWithStatus(
      [&](const ShapeIndex& index, const se::DeviceMemoryBase& buffer) {
        const Shape& buffer_shape =
            ShapeUtil::GetSubshape(*device_shape, index);
        if (buffer_shape.IsTuple()) {
          return absl::OkStatus();
        }
        Shape& device_sub_shape =
            *ShapeUtil::GetMutableSubshape(device_shape, index);
        if (device_sub_shape.is_static()) {
          return absl::OkStatus();
        }

        // Read the dynamic shape metadata from the device stream.  The dynamic
        // shape itself is stored at the end of the buffer.
        Shape buffer_shape_static = ShapeUtil::MakeStaticShape(buffer_shape);
        const int64_t offset = shape_size_fn(buffer_shape_static);
        int64_t metadata_size = shape_size_fn(buffer_shape) - offset;
        if (metadata_size == 0) {
          return InvalidArgument("Dynamic shape metadata size should not be 0");
        }

        auto buffer_8 = se::DeviceMemory<uint8_t>(buffer);
        auto metadata_buffer = buffer_8.GetSlice(offset, metadata_size);
        copies.push_back(std::make_pair(metadata_buffer, &device_sub_shape));

        return absl::OkStatus();
      }));

  // Check out pinned memory for each buffer we want to copy.  If there aren't
  // enough pinned buffers available, or if one of our buffers is so big it
  // doesn't fit, allocate an entry for it in fallback_buffers.
  std::vector<int32_t*> h2d_memcpy_dsts;
  std::vector<void*> checked_out_buffers;
  std::vector<std::unique_ptr<char[]>> fallback_buffers;

  // Return checked-out buffers at the end of this function.
  absl::Cleanup cleanup = [&] {
    absl::MutexLock lock(&mu_);
    pinned_buffers_.insert(pinned_buffers_.end(), checked_out_buffers.begin(),
                           checked_out_buffers.end());
  };

  {
    absl::MutexLock lock(&mu_);
    EnsurePinnedBuffersAllocated(stream->parent());

    for (const auto& src_dst : copies) {
      se::DeviceMemoryBase src = src_dst.first;
      if (!pinned_buffers_.empty() && src.size() <= kPinnedBufferBytes) {
        void* buf = pinned_buffers_.back();
        pinned_buffers_.pop_back();
        checked_out_buffers.push_back(buf);
        h2d_memcpy_dsts.push_back(reinterpret_cast<int32_t*>(buf));
      } else {
        LOG_FIRST_N(WARNING, 10)
            << "Unable to copy dynamic shape buffer of size " << src.size()
            << " to host using pinned memory.  Falling back to unpinned "
               "memory, which may be slow.";
        fallback_buffers.push_back(std::make_unique<char[]>(src.size()));
        h2d_memcpy_dsts.push_back(
            reinterpret_cast<int32_t*>(fallback_buffers.back().get()));
      }
    }
  }

  // Copy into the h2d_memcpy_dsts.
  for (int i = 0; i < copies.size(); i++) {
    se::DeviceMemoryBase src = copies[i].first;
    void* dst = h2d_memcpy_dsts[i];
    stream->ThenMemcpy(dst, src, src.size());
  }

  // Wait for all the async copies to complete, then write into device_shape.
  TF_RETURN_IF_ERROR(stream->BlockHostUntilDone());
  for (int i = 0; i < copies.size(); i++) {
    Shape* dst_shape = copies[i].second;
    int32_t* dst = h2d_memcpy_dsts[i];
    for (int64_t j = 0; j < dst_shape->rank(); j++) {
      dst_shape->mutable_dimensions()[j] = dst[j];
    }
  }

  device_shape->clear_dynamic_dimensions();
  TF_RET_CHECK(ShapeUtil::DynamicShapeIsCompatible(*device_shape,
                                                   original_device_shape));
  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace xla

static std::unique_ptr<xla::TransferManager> CreateNVPTXTransferManager() {
  return std::make_unique<xla::gpu::GpuTransferManager>(
      /*id=*/stream_executor::cuda::kCudaPlatformId,
      /*pointer_size=*/llvm::DataLayout(xla::gpu::nvptx::DataLayout())
          .getPointerSize(0 /* default address space */));
}

static std::unique_ptr<xla::TransferManager> CreateAMDGPUTransferManager() {
  return std::make_unique<xla::gpu::GpuTransferManager>(
      /*id=*/stream_executor::rocm::kROCmPlatformId,
      /*pointer_size=*/llvm::DataLayout(xla::gpu::amdgpu::DataLayout())
          .getPointerSize(0 /* default address space */));
}

static bool InitModule() {
  xla::TransferManager::RegisterTransferManager(
      stream_executor::cuda::kCudaPlatformId, &CreateNVPTXTransferManager);
  xla::TransferManager::RegisterTransferManager(
      stream_executor::rocm::kROCmPlatformId, &CreateAMDGPUTransferManager);
  return true;
}

static bool module_initialized = InitModule();
