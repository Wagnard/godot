#!/usr/bin/env python3

if __name__ != "__main__":
    raise SystemExit(f'Utility script "{__file__}" should not be used as a module!')

# Builds NVIDIA's Real-time Denoisers (NRD), the path tracer's denoiser on GPUs without DLSS Ray Reconstruction
# (PT_DENOISER_NRD_REBLUR, servers/rendering/renderer_rd/effects/nrd_denoiser.cpp), next to the other build
# dependencies, where SCons finds it (`nrd_path`). Optional: without it the engine builds without that denoiser.
#
# NRD is under the NVIDIA RTX SDKs licence (LICENSE.txt in its repository): it may be used and shipped in object form
# inside an application, unmodified, and must never become part of an open source distribution. So it is not in this
# repository: this script downloads the pinned release from NVIDIA's own repository, builds it as it is, and compiles
# this engine's front and back end shaders (shaders/effects/nrd_*.hlsl) with NRD's header and DXC into SPIR-V. A game
# that ships it credits NVIDIA's SDK (licence supplement, section 6.1 c).
#
# Needs CMake 3.30+ (in PATH, or Visual Studio's on Windows) and a C++ compiler; the NRD build downloads its own
# dependencies (ShaderMake, MathLib, DXC).

import argparse
import glob
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../"))

from misc.utility.color import Ansi, color_print

# Check for latest version: https://github.com/NVIDIA-RTX/NRD/releases
nrd_version = "4.18.0"

parser = argparse.ArgumentParser(description="Build NVIDIA NRD for the path tracer's PT_DENOISER_NRD_REBLUR.")
parser.add_argument("--deps_folder", default="", help="Where to install (default: as SCons' `nrd_path` default).")
parser.add_argument("--cmake", default="", help="CMake executable (default: from PATH, or Visual Studio's).")
args = parser.parse_args()

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../"))
deps_folder = args.deps_folder
if not deps_folder:
    # As misc/scripts/install_d3d12_sdk_windows.py, and SConstruct's `nrd_path` default.
    deps_folder = os.getenv("LOCALAPPDATA")
    if sys.platform == "win32" and deps_folder and not os.getenv("MSYSTEM"):
        deps_folder = os.path.join(deps_folder, "Godot", "build_deps")
    else:
        deps_folder = os.path.join(root, "bin", "build_deps")
nrd_folder = os.path.join(deps_folder, "nrd")
source_folder = os.path.join(deps_folder, f"nrd_src_{nrd_version}")
build_folder = os.path.join(source_folder, "_Build")


def find_cmake():
    if args.cmake:
        return args.cmake
    cmake = shutil.which("cmake")
    if cmake:
        return cmake
    if sys.platform == "win32":
        program_files = os.getenv("ProgramFiles(x86)", "")
        vswhere = os.path.join(program_files, "Microsoft Visual Studio", "Installer", "vswhere.exe")
        if os.path.exists(vswhere):
            vs = subprocess.run(
                [vswhere, "-latest", "-property", "installationPath"], capture_output=True, text=True
            ).stdout.strip()
            candidate = os.path.join(
                vs, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe"
            )
            if os.path.exists(candidate):
                return candidate
    raise SystemExit("CMake 3.30+ not found: put it in PATH or pass --cmake.")


def run(command, cwd=None):
    print(" ".join(command))
    subprocess.run(command, cwd=cwd, check=True)


os.makedirs(deps_folder, exist_ok=True)

# 1. NRD's sources, as NVIDIA publishes them.
color_print(f"{Ansi.BOLD}[1/4] NRD {nrd_version} sources")
if not os.path.exists(os.path.join(source_folder, "Include", "NRD.h")):
    archive = os.path.join(deps_folder, f"NRD-{nrd_version}.zip")
    if not os.path.isfile(archive):
        url = f"https://github.com/NVIDIA-RTX/NRD/archive/refs/tags/v{nrd_version}.zip"
        print(f"Downloading {url}")
        urllib.request.urlretrieve(url, archive)
    with zipfile.ZipFile(archive) as zf:
        top = zf.namelist()[0].split("/")[0]
        zf.extractall(deps_folder)
    if os.path.exists(source_folder):
        shutil.rmtree(source_folder)
    os.rename(os.path.join(deps_folder, top), source_folder)

# 2. The static library, unmodified. Only SPIR-V embedded: on D3D12 the engine translates it as every other shader. Quad
# intrinsics off: on Vulkan they need VK_KHR_compute_shader_derivatives, which the engine does not enable. The C runtime
# as the engine links it (MSVC: /MT).
color_print(f"{Ansi.BOLD}[2/4] Building NRD")
cmake = find_cmake()
configure = [
    cmake,
    "-S",
    source_folder,
    "-B",
    build_folder,
    "-DNRD_STATIC_LIBRARY=ON",
    "-DNRD_NRI=OFF",
    "-DNRD_EMBEDS_SPIRV_SHADERS=ON",
    "-DNRD_EMBEDS_DXIL_SHADERS=OFF",
    "-DNRD_EMBEDS_DXBC_SHADERS=OFF",
    "-DNRD_SUPPORTS_QUAD_INTRINSICS=OFF",
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
]
if sys.platform == "win32":
    configure += ["-A", "x64", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"]
else:
    configure += ["-DCMAKE_BUILD_TYPE=Release"]
run(configure)
run([cmake, "--build", build_folder, "--config", "Release", "--target", "NRD", "--parallel"])

library = glob.glob(os.path.join(source_folder, "_Bin", "**", "NRD.lib"), recursive=True) + glob.glob(
    os.path.join(source_folder, "_Bin", "**", "libNRD.a"), recursive=True
)
if not library:
    raise SystemExit("The NRD library was not built.")
# NRD finds its shaders in their blobs with NVIDIA's ShaderMake (MIT), a static library of its own.
shadermake_build = os.path.join(build_folder, "_deps", "shadermake-build")
shadermake_blob = glob.glob(os.path.join(shadermake_build, "**", "ShaderMakeBlob.lib"), recursive=True) + glob.glob(
    os.path.join(shadermake_build, "**", "libShaderMakeBlob.a"), recursive=True
)
if not shadermake_blob:
    raise SystemExit("ShaderMake's blob library was not built.")

# 3. The engine's front and back end shaders, with NRD's header and the DXC NRD compiled its own SPIR-V with.
color_print(f"{Ansi.BOLD}[3/4] Compiling the engine's NRD front and back end shaders")
dxc = ""
with open(os.path.join(build_folder, "CMakeCache.txt"), encoding="utf-8") as cache:
    for line in cache:
        if line.startswith("SHADERMAKE_DXC_VK_PATH") or (not dxc and line.startswith("SHADERMAKE_DXC_PATH")):
            dxc = line.split("=", 1)[1].strip()
if not dxc or not os.path.exists(dxc):
    raise SystemExit("NRD's DXC was not found in its CMake cache.")
mathlib = os.path.join(build_folder, "_deps", "mathlib-src")
shaders = {}
for name in ("nrd_frontend", "nrd_backend"):
    source = os.path.join(root, "servers", "rendering", "renderer_rd", "shaders", "effects", f"{name}.hlsl")
    output = os.path.join(build_folder, f"{name}.spv")
    run(
        [
            dxc,
            "-T",
            "cs_6_2",
            "-E",
            "main",
            "-spirv",
            "-fspv-target-env=vulkan1.2",
            "-HV",
            "2021",
            "-WX",
            "-O3",
            "-I",
            os.path.join(source_folder, "Shaders"),
            "-I",
            mathlib,
            "-Fo",
            output,
            source,
        ]
    )
    with open(output, "rb") as f:
        shaders[name] = f.read()

# 4. Installed: headers, library, the shaders as a header, NVIDIA's licence.
color_print(f"{Ansi.BOLD}[4/4] Installing in {nrd_folder}")
if os.path.exists(nrd_folder):
    shutil.rmtree(nrd_folder)
os.makedirs(os.path.join(nrd_folder, "include"))
os.makedirs(os.path.join(nrd_folder, "lib"))
for header in ("NRD.h", "NRDDescs.h", "NRDSettings.h"):
    shutil.copy(os.path.join(source_folder, "Include", header), os.path.join(nrd_folder, "include"))
shutil.copy(library[0], os.path.join(nrd_folder, "lib"))
shutil.copy(shadermake_blob[0], os.path.join(nrd_folder, "lib"))
shutil.copy(os.path.join(source_folder, "LICENSE.txt"), nrd_folder)
with open(os.path.join(nrd_folder, "include", "nrd_godot_shaders.gen.h"), "w", encoding="utf-8", newline="\n") as f:
    f.write(f"// Generated by misc/scripts/install_nrd.py from NRD {nrd_version}: do not edit.\n#pragma once\n\n")
    for name, data in shaders.items():
        f.write(f"inline constexpr unsigned char {name}_spirv[] = {{\n")
        for i in range(0, len(data), 24):
            f.write("\t" + ", ".join(str(b) for b in data[i : i + 24]) + ",\n")
        f.write("};\n\n")
with open(os.path.join(nrd_folder, "VERSION"), "w", encoding="utf-8") as f:
    f.write(nrd_version + "\n")

color_print(f"{Ansi.GREEN}NRD {nrd_version} installed in {nrd_folder}.")
