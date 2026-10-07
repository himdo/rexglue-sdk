"""Compile the scene-effect shaders for both GPU backends.

The scene effects (ambient occlusion, indirect light, reflections, fog,
volumetric light, the image pass - see src/graphics/pipeline/scene_effects.h)
run as extra passes the backends add to the game's frame. Their shaders are
written once in HLSL; this compiles each into the C headers the backends
#include:

  src/graphics/shaders/scene_fx/<source>.hlsl               the sources
  src/graphics/shaders/bytecode/d3d12_5_1/<name>.h           DXBC (Direct3D 12)
  src/graphics/shaders/vulkan_spirv/<name>.h                 SPIR-V (Vulkan)

Some sources build several variants with different defines (VARIANTS below).
Rerun after editing a source, then rebuild the SDK; commit the headers with it.

Usage:

    python scripts/build_scene_fx_shaders.py [--fxc fxc] [--glslang glslangValidator]

Needs, on PATH (or passed as options):
  * fxc - the Windows SDK's HLSL compiler. A Visual Studio developer shell
    ("Developer PowerShell for VS", or vcvars64.bat) puts it on PATH.
  * glslangValidator - compiles the same HLSL to SPIR-V. The Vulkan SDK
    installs it on PATH; or build the target glslangValidator of
    thirdparty/glslang.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "src" / "graphics" / "shaders"
SOURCES = SHADERS / "scene_fx"
DXBC_OUT = SHADERS / "bytecode" / "d3d12_5_1"
SPIRV_OUT = SHADERS / "vulkan_spirv"

# (output name, source, stage, extra defines)
VARIANTS = (
    ("scene_fx_depth_copy_cs", "scene_fx_depth_copy_cs.hlsl", "cs", ()),
    ("scene_fx_depth_copy_msaa_cs", "scene_fx_depth_copy_cs.hlsl", "cs", ("FX_MSAA",)),
    ("scene_fx_shadow_copy_cs", "scene_fx_depth_copy_cs.hlsl", "cs", ("FX_RAW",)),
    ("scene_fx_ao_prefilter_cs", "scene_fx_ao_prefilter_cs.hlsl", "cs", ()),
    ("scene_fx_ao_cs", "scene_fx_ao_cs.hlsl", "cs", ()),
    ("scene_fx_ao_gi_cs", "scene_fx_ao_cs.hlsl", "cs", ("FX_GI",)),
    ("scene_fx_color_capture_cs", "scene_fx_color_capture_cs.hlsl", "cs", ()),
    ("scene_fx_color_capture_msaa_cs", "scene_fx_color_capture_cs.hlsl", "cs", ("FX_MSAA",)),
    ("scene_fx_color_copy_cs", "scene_fx_color_capture_cs.hlsl", "cs", ("FX_FULL",)),
    ("scene_fx_color_copy_msaa_cs", "scene_fx_color_capture_cs.hlsl", "cs",
     ("FX_FULL", "FX_MSAA")),
    ("scene_fx_contact_shadows_cs", "scene_fx_contact_shadows_cs.hlsl", "cs", ()),
    ("scene_fx_reflections_cs", "scene_fx_reflections_cs.hlsl", "cs", ()),
    ("scene_fx_ao_blur_cs", "scene_fx_ao_blur_cs.hlsl", "cs", ()),
    ("scene_fx_blur_rgba_cs", "scene_fx_ao_blur_cs.hlsl", "cs", ("FX_RGBA",)),
    ("scene_fx_sky_color_cs", "scene_fx_sky_color_cs.hlsl", "cs", ()),
    ("scene_fx_sky_color_msaa_cs", "scene_fx_sky_color_cs.hlsl", "cs", ("FX_MSAA",)),
    ("scene_fx_volumetric_cs", "scene_fx_volumetric_cs.hlsl", "cs", ()),
    ("scene_fx_temporal_cs", "scene_fx_temporal_cs.hlsl", "cs", ()),
    ("scene_fx_fullscreen_vs", "scene_fx_fullscreen_vs.hlsl", "vs", ()),
    ("scene_fx_composite_ps", "scene_fx_composite_ps.hlsl", "ps", ()),
    ("scene_fx_image_ps", "scene_fx_image_ps.hlsl", "ps", ()),
)
# HLSL stage -> glslangValidator stage name.
GLSLANG_STAGE = {"cs": "comp", "vs": "vert", "ps": "frag"}


def find_tool(name, option):
    path = shutil.which(name)
    if not path:
        raise SystemExit(f"{name} not found on PATH; pass {option} (see --help)")
    return path


def run(command):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
        raise SystemExit(f"failed: {' '.join(str(part) for part in command)}")


def normalize_line_endings(path):
    # FXC writes CRLF; the repository keeps LF.
    data = path.read_bytes()
    path.write_bytes(data.replace(b"\r\n", b"\n"))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fxc", default="fxc",
                        help="FXC: a path, or a name looked up on PATH (default: fxc)")
    parser.add_argument("--glslang", default="glslangValidator",
                        help="glslangValidator: a path, or a name looked up on PATH "
                             "(default: glslangValidator)")
    args = parser.parse_args()
    fxc = find_tool(args.fxc, "--fxc")
    glslang = find_tool(args.glslang, "--glslang")

    for name, source, stage, defines in VARIANTS:
        source_path = SOURCES / source
        # Direct3D 12: shader model 5.1, the header holds a byte array `name`.
        run([fxc, "/nologo", "/O3", "/T", f"{stage}_5_1", "/E", "main",
             *[f"/D{define}=1" for define in defines],
             "/Vn", name, "/Fh", DXBC_OUT / f"{name}.h", source_path])
        # Vulkan: the same HLSL as SPIR-V (-D: HLSL input). SCENE_FX_VULKAN
        # selects the Vulkan bindings in scene_fx_common.hlsli.
        run([glslang, "-D", "-V", "--target-env", "vulkan1.0",
             "-S", GLSLANG_STAGE[stage], "-e", "main", "-DSCENE_FX_VULKAN=1",
             *[f"-D{define}=1" for define in defines],
             "--vn", name, "-o", SPIRV_OUT / f"{name}.h", source_path])
        normalize_line_endings(DXBC_OUT / f"{name}.h")
        normalize_line_endings(SPIRV_OUT / f"{name}.h")
        print(f"built {name}")


if __name__ == "__main__":
    main()
