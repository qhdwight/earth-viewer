"""Module extension exposing the locally installed Vulkan SDK as @vulkan.

The generated repository's BUILD file is BUILD.vulkan in this package, stamped
with the SDK paths that vary per machine. On Linux without VULKAN_SDK set, the
distribution's packages stand in for the SDK.
"""

def _system_vulkan(rctx):
    """Linux distributions package the headers, loader and glslang."""
    include_dir = rctx.path("/usr/include/vulkan")
    glslang = rctx.which("glslangValidator")
    if not include_dir.exists or not glslang:
        fail(
            "VULKAN_SDK is not set and the system Vulkan packages are missing. " +
            "Install them (Debian / Ubuntu: libvulkan-dev glslang-tools; " +
            "Fedora: vulkan-loader-devel glslang; Arch: vulkan-headers " +
            "vulkan-icd-loader glslang) or the LunarG Vulkan SDK.",
        )
    rctx.symlink(include_dir, "include/vulkan")
    video_dir = rctx.path("/usr/include/vk_video")
    if video_dir.exists:
        rctx.symlink(video_dir, "include/vk_video")
    rctx.symlink(glslang, "bin/glslangValidator")
    rctx.template(
        "BUILD.bazel",
        Label("//third_party/vulkan:BUILD.vulkan"),
        substitutions = {
            "%{lib_dir}": "",
            "%{default_linkopts}": '"-lvulkan"',
            "%{glslang}": "bin/glslangValidator",
        },
    )

def _vulkan_repo_impl(rctx):
    sdk = rctx.getenv("VULKAN_SDK")
    if not sdk and rctx.os.name.lower().startswith("linux"):
        _system_vulkan(rctx)
        return
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
    bin_dir = sdk_path.get_child("Bin" if windows else "bin")

    if not include_dir.exists:
        fail("No Vulkan headers at %s" % include_dir)

    # Recent Linux SDKs keep the loader in its own directory. At run time the
    # distribution's libvulkan.so.1 is found instead.
    loader_dir = lib_dir.get_child("VulkanLoader").get_child("lib")
    if not loader_dir.exists:
        loader_dir = lib_dir

    rctx.symlink(include_dir, "include")
    rctx.symlink(lib_dir, "lib")
    rctx.symlink(bin_dir, "bin")

    rctx.template(
        "BUILD.bazel",
        Label("//third_party/vulkan:BUILD.vulkan"),
        substitutions = {
            "%{lib_dir}": str(lib_dir),
            "%{default_linkopts}": '"-L%s", "-lvulkan"' % loader_dir,
            "%{glslang}": "bin/glslangValidator.exe" if windows else "bin/glslangValidator",
        },
    )

_vulkan_repo = repository_rule(
    implementation = _vulkan_repo_impl,
    environ = ["VULKAN_SDK"],
    local = True,
)

def _vulkan_impl(_module_ctx):
    _vulkan_repo(name = "vulkan")

vulkan = module_extension(implementation = _vulkan_impl)
