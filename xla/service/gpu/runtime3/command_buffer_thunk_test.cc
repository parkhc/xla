/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/service/gpu/runtime3/command_buffer_thunk.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "xla/service/buffer_assignment.h"
#include "xla/service/gpu/buffer_allocations.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/gpu/matmul_utils.h"
#include "xla/service/gpu/runtime3/command_buffer_cmd.h"
#include "xla/service/gpu/thunk.h"
#include "xla/service/service_executable_run_options.h"
#include "xla/shape_util.h"
#include "xla/stream_executor/blas.h"
#include "xla/stream_executor/cuda/cuda_test_kernels.h"
#include "xla/stream_executor/multi_platform_manager.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/stream_executor.h"
#include "xla/stream_executor/stream_executor_pimpl.h"
#include "xla/types.h"  // IWYU pragma: keep
#include "tsl/lib/core/status_test_util.h"
#include "tsl/platform/test.h"

namespace xla::gpu {

static se::StreamExecutor* CudaExecutor() {
  auto* platform = se::MultiPlatformManager::PlatformWithName("CUDA").value();
  return platform->ExecutorForDevice(0).value();
}

TEST(CommandBufferThunkTest, MemcpyCmd) {
  se::StreamExecutor* executor = CudaExecutor();

  se::Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  int64_t length = 4;
  int64_t byte_length = sizeof(int32_t) * length;

  // Prepare arguments: a=42, b=0
  se::DeviceMemory<int32_t> a = executor->AllocateArray<int32_t>(length, 0);
  se::DeviceMemory<int32_t> b = executor->AllocateArray<int32_t>(length, 0);

  stream.ThenMemset32(&a, 42, byte_length);
  stream.ThenMemZero(&b, byte_length);

  // Prepare buffer allocations for recording command buffer.
  BufferAllocation alloc_a(/*index=*/0, byte_length, /*color=*/0);
  BufferAllocation alloc_b(/*index=*/1, byte_length, /*color=*/0);

  BufferAllocation::Slice slice_a(&alloc_a, 0, byte_length);
  BufferAllocation::Slice slice_b(&alloc_b, 0, byte_length);

  // Prepare commands sequence for constructing command buffer.
  CommandBufferCmdSequence commands;
  commands.Emplace<MemcpyDeviceToDeviceCmd>(slice_b, slice_a, byte_length);

  // Construct a thunk with command sequence.
  CommandBufferThunk thunk(std::move(commands), Thunk::ThunkInfo(nullptr));

  ServiceExecutableRunOptions run_options;
  BufferAllocations allocations({a, b}, 0, executor->GetAllocator());
  Thunk::ExecuteParams params(run_options, allocations, &stream, {});

  // Execute command buffer thunk and verify that it copied the memory.
  TF_ASSERT_OK(thunk.ExecuteOnStream(params));

  // Copy `b` data back to host.
  std::vector<int32_t> dst(4, 0);
  stream.ThenMemcpy(dst.data(), b, byte_length);

  ASSERT_EQ(dst, std::vector<int32_t>(4, 42));
}

TEST(CommandBufferThunkTest, LaunchCmd) {
  se::StreamExecutor* executor = CudaExecutor();

  se::Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  int64_t length = 4;
  int64_t byte_length = sizeof(int32_t) * length;

  // Prepare arguments: a=42, b=0
  se::DeviceMemory<int32_t> a = executor->AllocateArray<int32_t>(length, 0);
  se::DeviceMemory<int32_t> b = executor->AllocateArray<int32_t>(length, 0);

  stream.ThenMemset32(&a, 42, byte_length);
  stream.ThenMemZero(&b, byte_length);

  // Prepare buffer allocations for recording command buffer.
  BufferAllocation alloc_a(/*index=*/0, byte_length, /*color=*/0);
  BufferAllocation alloc_b(/*index=*/1, byte_length, /*color=*/0);

  BufferAllocation::Slice slice_a(&alloc_a, 0, byte_length);
  BufferAllocation::Slice slice_b(&alloc_b, 0, byte_length);

  auto args = {slice_a, slice_a, slice_b};  // b = a + a

  // Prepare commands sequence for constructing command buffer.
  CommandBufferCmdSequence commands;
  commands.Emplace<LaunchCmd>("add", args, LaunchDimensions(1, 4),
                              /*shmem_bytes=*/0);

  // Construct a thunk with command sequence.
  CommandBufferThunk thunk(std::move(commands), Thunk::ThunkInfo(nullptr));

  ServiceExecutableRunOptions run_options;
  BufferAllocations allocations({a, b}, 0, executor->GetAllocator());
  Thunk::ExecuteParams params(run_options, allocations, &stream, {});

  CommandBufferCmd::ExecutableSource source = {
      /*text=*/se::cuda::internal::kAddI32Kernel, /*binary=*/{}};
  TF_ASSERT_OK(thunk.Initialize(executor, source));

  // Execute command buffer thunk and verify that it added the value.
  TF_ASSERT_OK(thunk.ExecuteOnStream(params));

  // Copy `b` data back to host.
  std::vector<int32_t> dst(4, 0);
  stream.ThenMemcpy(dst.data(), b, byte_length);

  ASSERT_EQ(dst, std::vector<int32_t>(4, 42 + 42));

  // Prepare buffer allocation for updating command buffer: c=0
  se::DeviceMemory<int32_t> c = executor->AllocateArray<int32_t>(length, 0);
  stream.ThenMemZero(&c, byte_length);

  // Update buffer allocation #1 to buffer `c`.
  allocations = BufferAllocations({a, c}, 0, executor->GetAllocator());

  // Thunk execution should automatically update underlying command buffer.
  TF_ASSERT_OK(thunk.ExecuteOnStream(params));

  // Copy `c` data back to host.
  std::fill(dst.begin(), dst.end(), 0);
  stream.ThenMemcpy(dst.data(), c, byte_length);

  ASSERT_EQ(dst, std::vector<int32_t>(4, 42 + 42));
}

TEST(CommandBufferThunkTest, GemmCmd) {
  se::StreamExecutor* executor = CudaExecutor();

  se::Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  int64_t lhs_length = sizeof(float) * 2 * 4;
  int64_t rhs_length = sizeof(float) * 4 * 3;
  int64_t out_length = sizeof(float) * 2 * 3;

  // Prepare arguments:
  // lhs = [1.0, 2.0, 3.0, 4.0
  //        5.0, 6.0, 7.0, 8.0]
  // rhs = [1.0, 1.0, 1.0
  //        1.0, 1.0, 1.0
  //        1.0, 1.0, 1.0
  //        1.0, 1.0, 1.0]
  se::DeviceMemory<float> lhs = executor->AllocateArray<float>(2 * 4);
  std::vector<float> lhs_arr{1, 2, 3, 4, 5, 6, 7, 8};
  stream.ThenMemcpy(&lhs, lhs_arr.data(), lhs_length);

  se::DeviceMemory<float> rhs = executor->AllocateArray<float>(4 * 3);
  std::vector<float> rhs_arr(12, 1);
  stream.ThenMemcpy(&rhs, rhs_arr.data(), rhs_length);

  se::DeviceMemory<float> out = executor->AllocateArray<float>(2 * 3);
  stream.ThenMemZero(&out, out_length);

  // Prepare buffer allocations for recording command buffer.
  BufferAllocation alloc_lhs(/*index=*/0, lhs_length, /*color=*/0);
  BufferAllocation alloc_rhs(/*index=*/1, rhs_length, /*color=*/0);
  BufferAllocation alloc_out(/*index=*/2, out_length, /*color=*/0);

  BufferAllocation::Slice slice_lhs(&alloc_lhs, 0, lhs_length);
  BufferAllocation::Slice slice_rhs(&alloc_rhs, 0, rhs_length);
  BufferAllocation::Slice slice_out(&alloc_out, 0, out_length);

  auto config = GemmConfig::For(
      ShapeUtil::MakeShape(PrimitiveType::F32, {2, 4}), {}, {1},
      ShapeUtil::MakeShape(PrimitiveType::F32, {4, 3}), {}, {0},
      ShapeUtil::MakeShape(PrimitiveType::F32, {2, 3}), 1.0, 0.0, 0.0,
      std::nullopt, se::blas::kDefaultComputePrecision, false, false);
  ASSERT_TRUE(config.ok());

  // Prepare commands sequence for constructing command buffer.
  CommandBufferCmdSequence commands;
  commands.Emplace<GemmCmd>(config.value(), slice_lhs, slice_rhs, slice_out,
                            /*deterministic*/ true);

  // Construct a thunk with command sequence.
  CommandBufferThunk thunk(std::move(commands), Thunk::ThunkInfo(nullptr));

  ServiceExecutableRunOptions run_options;
  BufferAllocations allocations({lhs, rhs, out}, 0, executor->GetAllocator());
  Thunk::ExecuteParams params(run_options, allocations, &stream, {});

  CommandBufferCmd::ExecutableSource source = {/*text=*/"", /*binary=*/{}};
  TF_ASSERT_OK(thunk.Initialize(executor, source));

  // Execute command buffer thunk and verify that it executed a GEMM.
  TF_ASSERT_OK(thunk.ExecuteOnStream(params));

  // Copy `out` data back to host.
  std::vector<float> dst(6, 0);
  stream.ThenMemcpy(dst.data(), out, out_length);

  ASSERT_EQ(dst, std::vector<float>({10, 10, 10, 26, 26, 26}));
}

}  // namespace xla::gpu
