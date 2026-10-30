#!/bin/bash
set -euo pipefail

usage() {
    printf 'Usage: %s <absolute-install-prefix>\n' "${0##*/}" >&2
}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

if [[ $# -ne 1 || "$1" != /* ]]; then
    usage
    exit 2
fi

for tool in git cmake ninja xcrun xcodebuild; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done

[[ "$(uname -s)" == Darwin ]] || fail 'run this builder on macOS'

prefix=$(mkdir -p "$1" && cd "$1" && pwd -P)
jobs=4
deployment_target=17.0
platforms=(iphonesimulator iphoneos)
simulator_prefix="$prefix/${platforms[0]}"
device_prefix="$prefix/${platforms[1]}"

source_root="$prefix/src"
build_root="$prefix/build"
mkdir -p "$source_root" "$build_root"

fetch_pinned_source() {
    local name=$1 url=$2 ref=$3 commit=$4 directory=$5 origin actual
    if [[ ! -d "$directory/.git" ]]; then
        mkdir -p "$(dirname "$directory")"
        git clone --filter=blob:none --no-checkout "$url" "$directory"
    fi
    origin=$(git -C "$directory" remote get-url origin)
    [[ "$origin" == "$url" ]] || fail "$name source has unexpected origin: $origin"
    git -C "$directory" fetch --quiet --filter=blob:none --depth=1 origin "$ref"
    git -C "$directory" checkout --quiet --detach "$commit"
    actual=$(git -C "$directory" rev-parse HEAD)
    [[ "$actual" == "$commit" ]] || fail "$name commit mismatch: $actual"
    [[ -z "$(git -C "$directory" status --porcelain --untracked-files=all)" ]] ||
        fail "$name source checkout is dirty: $directory"
    printf '%s %s %s\n' "$name" "$ref" "$actual"
}

fetch_pinned_source glslang \
    https://github.com/KhronosGroup/glslang.git \
    refs/tags/16.6.0 e1b562a8bed273a02f30b59b66a5d499793cede5 \
    "$source_root/glslang"
fetch_pinned_source SPIRV-Cross \
    https://github.com/KhronosGroup/SPIRV-Cross.git \
    refs/tags/vulkan-sdk-1.4.363.0 f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7 \
    "$source_root/spirv-cross"
fetch_pinned_source SPIRV-Tools \
    https://github.com/KhronosGroup/SPIRV-Tools.git \
    ef96ed763b43b59b33b31b362f09a02b729fa1c9 ef96ed763b43b59b33b31b362f09a02b729fa1c9 \
    "$source_root/spirv-tools"
fetch_pinned_source SPIRV-Headers \
    https://github.com/KhronosGroup/SPIRV-Headers.git \
    496543121ce6419f23d6fa5d7194ba66c36212d2 496543121ce6419f23d6fa5d7194ba66c36212d2 \
    "$source_root/spirv-headers"

libraries=(
    glslang SPIRV MachineIndependent GenericCodeGen OSDependent
    glslang-default-resource-limits SPIRV-Tools-opt SPIRV-Tools
    spirv-cross-c spirv-cross-msl spirv-cross-glsl spirv-cross-core
)

for platform in "${platforms[@]}"; do
    sdk=$(xcrun --sdk "$platform" --show-sdk-path)
    [[ -d "$sdk" ]] || fail "SDK not found: $sdk"
    install="$prefix/$platform"
    build="$build_root/$platform"
    common_ios_args=(
        -DCMAKE_SYSTEM_NAME=iOS
        -DCMAKE_OSX_SYSROOT="$sdk"
        -DCMAKE_OSX_ARCHITECTURES=arm64
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target"
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX="$install"
        -DBUILD_SHARED_LIBS=OFF
    )
    cmake -S "$source_root/spirv-tools" -B "$build/spirv-tools" -G Ninja \
        "${common_ios_args[@]}" \
        -DSPIRV-Headers_SOURCE_DIR="$source_root/spirv-headers" \
        -DSPIRV_TOOLS_BUILD_STATIC=ON \
        -DSPIRV_TOOLS_LIBRARY_TYPE=STATIC \
        -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON
    # Upstream always defines a shared target. Build only the static closure
    # and consume build-tree package exports so installation cannot build it.
    cmake --build "$build/spirv-tools" --parallel "$jobs" \
        --target SPIRV-Tools-static SPIRV-Tools-opt
    # Upstream emits configs at the build root but exports in source/ and opt/.
    cp "$build/spirv-tools/source/SPIRV-ToolsTarget.cmake" \
        "$build/spirv-tools/source/opt/SPIRV-Tools-optTargets.cmake" \
        "$build/spirv-tools/"
    cmake -S "$source_root/glslang" -B "$build/glslang" -G Ninja \
        "${common_ios_args[@]}" \
        -DSPIRV-Tools-opt_DIR="$build/spirv-tools" \
        -DSPIRV-Tools_DIR="$build/spirv-tools" \
        -DALLOW_EXTERNAL_SPIRV_TOOLS=ON -DENABLE_OPT=ON \
        -DENABLE_HLSL=ON -DENABLE_GLSLANG_BINARIES=OFF \
        -DENABLE_SPVREMAPPER=OFF -DGLSLANG_TESTS=OFF \
        -DGLSLANG_ENABLE_INSTALL=ON
    cmake --build "$build/glslang" --parallel "$jobs"
    cmake --install "$build/glslang"
    mkdir -p "$install/lib" "$install/include/spirv-tools"
    cp -p "$build/spirv-tools/source/libSPIRV-Tools.a" \
        "$build/spirv-tools/source/opt/libSPIRV-Tools-opt.a" "$install/lib/"
    cp -pR "$source_root/spirv-tools/include/spirv-tools/." \
        "$install/include/spirv-tools/"
    cmake -S "$source_root/spirv-cross" -B "$build/spirv-cross" -G Ninja \
        "${common_ios_args[@]}" \
        -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_SHARED=OFF \
        -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF \
        -DSPIRV_CROSS_ENABLE_C_API=ON -DSPIRV_CROSS_ENABLE_MSL=ON \
        -DSPIRV_CROSS_ENABLE_GLSL=ON -DSPIRV_CROSS_ENABLE_HLSL=OFF \
        -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF \
        -DSPIRV_CROSS_ENABLE_UTIL=OFF
    cmake --build "$build/spirv-cross" --parallel "$jobs"
    cmake --install "$build/spirv-cross"
done

mkdir -p "$prefix/xcframeworks"
staging_root=$(mktemp -d /tmp/ps3recomp-shader-deps.XXXXXX)
# Only this script's disposable staging tree is deleted, never the prefix.
trap 'rm -rf -- "$staging_root"' EXIT
for library in "${libraries[@]}"; do
    output="$prefix/xcframeworks/$library.xcframework"
    # Repackage only after an archive or public header changes.
    if [[ -f "$output/.built" ]] &&
        [[ -z "$(find "$simulator_prefix/include" "$device_prefix/include" \
            "$simulator_prefix/lib/lib$library.a" \
            "$device_prefix/lib/lib$library.a" -newer "$output/.built" -print)" ]]; then
        printf 'Unchanged %s\n' "$output"
        continue
    fi
    xcodebuild -create-xcframework \
        -library "$simulator_prefix/lib/lib$library.a" \
        -headers "$simulator_prefix/include" \
        -library "$device_prefix/lib/lib$library.a" \
        -headers "$device_prefix/include" \
        -output "$staging_root/$library.xcframework"
    touch "$staging_root/$library.xcframework/.built"
    if [[ -e "$output" ]]; then
        mv "$output" "$staging_root/$library.previous"
    fi
    if ! mv "$staging_root/$library.xcframework" "$output"; then
        if [[ -e "$staging_root/$library.previous" ]]; then
            mv "$staging_root/$library.previous" "$output"
        fi
        fail "could not install $output"
    fi
    printf 'Built %s (arm64 iOS + arm64 Simulator, static)\n' "$output"
done
printf 'Shader XCFramework prefix: %s\n' "$prefix"
