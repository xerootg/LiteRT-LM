# JNI for Kokoro Reader (see kokoro_tts_jni.cc).
#
#   bazel build -c opt --config=android_arm64 //omni/text2speech/jni:libkokoro_tts_jni.so
#   bazel build -c opt //omni/text2speech/jni:libkokoro_tts_jni.so        # host, for a compile check

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "libkokoro_tts_jni.so",
    srcs = ["kokoro_tts_jni.cc"],
    linkshared = True,
    linkstatic = True,
    linkopts = select({
        # 16 KB page size: Android 15+ warns (and 16 KB-page devices refuse) unless LOAD segments are 16 KB aligned.
        "@platforms//os:android": ["-llog", "-landroid", "-Wl,-z,max-page-size=16384"],
        "//conditions:default": [],
    }),
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "//omni:omni_engine",
        "//omni:omni_registry",
        "//omni:omni_session",
        "//omni/base:io_types",
        "//omni/text2speech:kokoro",
        "//runtime/executor:default_gpu_accelerator",
        "//runtime/proto:text2speech_metadata_cc_proto",
        "//runtime/proto:text2speech_session_config_cc_proto",
    ] + select({
        "@platforms//os:android": [],
        "//conditions:default": ["@bazel_tools//tools/jdk:jni"],
    }),
)
