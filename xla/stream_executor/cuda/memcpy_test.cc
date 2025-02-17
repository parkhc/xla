/* Copyright 2019 The OpenXLA Authors.

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

#if GOOGLE_CUDA
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/multi_platform_manager.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/stream.h"
#include "xla/stream_executor/stream_executor.h"
#include "tsl/platform/test.h"

namespace stream_executor {

TEST(MemcpyTest, PinnedHostMemory) {
  Platform* platform = MultiPlatformManager::PlatformWithName("CUDA").value();
  StreamExecutor* executor = platform->ExecutorForDevice(0).value();
  Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  auto d_ptr = executor->HostMemoryAllocate(sizeof(int));
  DeviceMemoryBase d_mem(d_ptr->opaque(), sizeof(int));

  int h_ptr;
  stream.ThenMemcpy(&h_ptr, d_mem, d_mem.size());
  EXPECT_TRUE(stream.BlockHostUntilDone().ok());
}

}  // namespace stream_executor

#endif  // GOOGLE_CUDA
