load("@tsl//tsl/platform:rules_cc.bzl", "cc_library")

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = [":friends"],
    licenses = ["notice"],
)

package_group(
    name = "friends",
    includes = [
        "//xla:friends",
    ],
)

cc_library(
    name = "retain",
    srcs = ["retain.cc"],
    visibility = ["//visibility:public"],
    alwayslink = 1,
)

cc_library(
    name = "collectives",
    srcs = ["collectives.cc"],
    hdrs = ["collectives.h"],
    deps = [
        "//xla:executable_run_options",
        "//xla:shape_util",
        "//xla:xla_data_proto_cc",
        "//xla/runtime:custom_call",
        "//xla/runtime:custom_call_registry",
        "//xla/runtime:executable",
        "//xla/service/cpu:cpu_runtime",
        "@llvm-project//mlir:Support",
    ],
)

cc_library(
    name = "convolution",
    srcs = ["convolution.cc"],
    hdrs = ["convolution.h"],
    deps = [
        "//xla:executable_run_options",
        "//xla/runtime:memref_view",
        "//xla/service/cpu:runtime_conv2d",
        "//xla/service/cpu:runtime_conv3d",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/types:span",
    ],
)

cc_library(
    name = "convolution_ffi",
    srcs = ["convolution_ffi.cc"],
    hdrs = ["convolution_ffi.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":convolution",
        "//xla/runtime:aot_ffi",
        "//xla/runtime:aot_ffi_execution_context",
        "//xla/runtime/ffi:ffi_api",
    ],
)

cc_library(
    name = "convolution_call",
    srcs = ["convolution_call.cc"],
    hdrs = ["convolution_call.h"],
    deps = [
        ":convolution",
        "//xla:executable_run_options",
        "//xla/runtime:custom_call",
        "//xla/runtime:custom_call_registry",
        "//xla/runtime:executable",
    ],
)

cc_library(
    name = "custom_call",
    srcs = ["custom_call.cc"],
    hdrs = ["custom_call.h"],
    deps = [
        "//xla:shape_util",
        "//xla:xla_proto_cc",
        "//xla/runtime:custom_call",
        "//xla/runtime:custom_call_registry",
        "//xla/runtime:executable",
        "//xla/service:custom_call_status_internal",
        "//xla/service:custom_call_target_registry",
        "//xla/service:hlo_proto_cc",
        "@llvm-project//mlir:Support",
    ],
)

cc_library(
    name = "fft_call",
    srcs = ["fft_call.cc"],
    hdrs = ["fft_call.h"],
    deps = [
        "//xla:executable_run_options",
        "//xla:xla_proto_cc",
        "//xla/runtime:custom_call",
        "//xla/runtime:custom_call_registry",
        "//xla/runtime:executable",
        "//xla/runtime:memref_view",
        "//xla/service:hlo_proto_cc",
        "//xla/service/cpu:runtime_fft",
        "@com_google_absl//absl/container:inlined_vector",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "xfeed",
    srcs = ["xfeed.cc"],
    hdrs = ["xfeed.h"],
    deps = [
        "//xla:executable_run_options",
        "//xla:shape_util",
        "//xla/runtime:custom_call",
        "//xla/runtime:custom_call_registry",
        "//xla/runtime:executable",
        "//xla/service/cpu:cpu_runtime",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Support",
    ],
)

cc_library(
    name = "rng",
    srcs = ["rng.cc"],
    hdrs = ["rng.h"],
    deps = [
        "//xla:executable_run_options",
        "//xla/runtime:memref_view",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "rng_call",
    srcs = ["rng_call.cc"],
    hdrs = ["rng_call.h"],
    deps = [
        ":rng",
        "//xla:executable_run_options",
        "//xla/runtime:custom_call",
        "//xla/runtime:custom_call_registry",
        "//xla/runtime:executable",
    ],
)

cc_library(
    name = "rng_ffi",
    srcs = ["rng_ffi.cc"],
    hdrs = ["rng_ffi.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":rng",
        "//xla/runtime:aot_ffi",
        "//xla/runtime:aot_ffi_execution_context",
        "//xla/runtime/ffi:ffi_api",
    ],
)
