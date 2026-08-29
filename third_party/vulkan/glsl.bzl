"""Compiles GLSL to a SPIR-V C header using the Vulkan SDK's glslangValidator."""

def _glsl_header_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".h")
    ctx.actions.run(
        executable = ctx.file._glslang,
        arguments = [
            "-V",
            "--target-env",
            "vulkan1.4",
            "--vn",
            ctx.attr.variable,
            ctx.file.src.path,
            "-o",
            out.path,
        ],
        inputs = [ctx.file.src],
        tools = [ctx.file._glslang],
        outputs = [out],
        mnemonic = "Glslang",
        progress_message = "Compiling shader %{input}",
    )
    return [DefaultInfo(files = depset([out]))]

glsl_header = rule(
    implementation = _glsl_header_impl,
    doc = "Compiles a GLSL shader into a header declaring `const uint32_t <variable>[]`.",
    attrs = {
        "src": attr.label(allow_single_file = True, mandatory = True),
        "variable": attr.string(mandatory = True),
        "_glslang": attr.label(
            default = "@vulkan//:glslang",
            allow_single_file = True,
            cfg = "exec",
        ),
    },
)
