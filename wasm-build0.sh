#!/usr/bin/env bash
# Build native lfortran and install runtime .mod files to the WASM host env.
# Run in the 'lf' env before wasm-build1.sh.
#
# Prerequisites:
#   - xeus-lfortran-wasm-host conda env already created

set -ex

export PREFIX=${PREFIX:-$MAMBA_ROOT_PREFIX/envs/xeus-lfortran-wasm-host}

if [ "$(uname)" = "Darwin" ]; then
    CORES=$(sysctl -n hw.ncpu)
else
    CORES=$(nproc)
fi

# Generate parser/ASR headers if not already done.
./build0.sh

rm -rf asset_dir

# Build lfortran binary only; skip the runtime cmake sub-project.
cmake -S . -B asset_dir \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_LLVM=no \
    -DLFORTRAN_BUILD_ALL=yes \
    -DWITH_RUNTIME_LIBRARY=no \
    -DWITH_STACKTRACE=no \
    -DWITH_RUNTIME_STACKTRACE=no \
    -DWITH_XEUS=no \
    -DWITH_LSP=no \
    -DWITH_KOKKOS=no \
    -DCMAKE_PREFIX_PATH="$CONDA_PREFIX"

cmake --build asset_dir -j"$CORES" --target lfortran

# Compile runtime modules using the freshly built lfortran.
LFORTRAN="$(pwd)/asset_dir/src/bin/lfortran"
MODDIR="$(pwd)/asset_dir/mods"
RTDIR="$(pwd)/src/runtime"
mkdir -p "$MODDIR"

$LFORTRAN --backend=cpp -c -J "$MODDIR" "$RTDIR/pure/lfortran_intrinsic_iso_fortran_env.f90"
$LFORTRAN --backend=cpp -c -J "$MODDIR" "$RTDIR/custom/lfortran_intrinsic_custom.f90"
$LFORTRAN --backend=cpp -c -J "$MODDIR" -I "$MODDIR" "$RTDIR/pure/lfortran_intrinsic_ieee_arithmetic.f90"
$LFORTRAN --backend=cpp -c -J "$MODDIR" "$RTDIR/pure/lfortran_intrinsic_iso_c_binding.f90"
$LFORTRAN --backend=cpp -c -J "$MODDIR" -I "$MODDIR" "$RTDIR/openmp/omp_lib.f90"

cp "$MODDIR"/*.mod "$PREFIX/lib/"
rm -rf asset_dir
