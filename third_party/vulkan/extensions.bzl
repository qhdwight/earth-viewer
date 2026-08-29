"""Module extension exposing the locally installed Vulkan SDK as @vulkan."""

_BUILD_WINDOWS = """\
load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")

cc_import(
    name = "loader",
    interface_library = "lib/vulkan-1.lib",
    # vulkan-1.dll ships with the driver / Windows itself, so it is not a
    # runtime dependency Bazel needs to stage.
    system_provided = True,
)

cc_library(
    name = "vulkan",
    hdrs = glob([
        "include/vulkan/**",
        "include/vk_video/**",
    ]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [":loader"],
)
"""

_BUILD_UNIX = """\
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "vulkan",
    hdrs = glob([
        "include/vulkan/**",
        "include/vk_video/**",
    ]),
    includes = ["include"],
    linkopts = [
        "-L{lib_dir}",
        "-lvulkan",
    ],
    visibility = ["//visibility:public"],
)
"""

def _vulkan_repo_impl(rctx):
    sdk = rctx.getenv("VULKAN_SDK")
    if not sdk:
        fail(
            "VULKAN_SDK is not set. Install the LunarG Vulkan SDK and source " +
            "its setup script (setup-env.sh / SetEnv.ps1) before building.",
        )

    sdk_path = rctx.path(sdk)
    windows = rctx.os.name.lower().startswith("windows")

    # The SDK capitalises directories differently per platform.
    include_dir = sdk_path.get_child("Include" if windows else "include")
    lib_dir = sdk_path.get_child("Lib" if windows else "lib")

    if not include_dir.exists:
        fail("No Vulkan headers at %s" % include_dir)

    rctx.symlink(include_dir, "include")
    rctx.symlink(lib_dir, "lib")

    if windows:
        rctx.file("BUILD.bazel", _BUILD_WINDOWS)
    else:
        rctx.file("BUILD.bazel", _BUILD_UNIX.format(lib_dir = str(lib_dir)))

_vulkan_repo = repository_rule(
    implementation = _vulkan_repo_impl,
    environ = ["VULKAN_SDK"],
    local = True,
)

def _vulkan_impl(_module_ctx):
    _vulkan_repo(name = "vulkan")

vulkan = module_extension(implementation = _vulkan_impl)
