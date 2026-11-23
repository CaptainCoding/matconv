# matconv

Experimental C++ command-line converter between NVIDIA MDL and MaterialX.
Conversion is lossy; unsupported features may produce warnings or fallbacks.

## Build

Requires CMake 3.26+, a C++17 compiler, and vcpkg with `VCPKG_ROOT` set.
The preset opts into fetching the MDL SDK, MaterialX, and image dependencies:

```sh
cmake --preset mdl-with-image-plugin
cmake --build build/mdl-with-image-plugin --parallel 4
ctest --test-dir build/mdl-with-image-plugin --output-on-failure
```

## Usage

```sh
build/mdl-with-image-plugin/matconv mdl-to-mtlx \
  --input material.mdl --output material.mtlx \
  --material material_name --mdl-path /path/to/mdl/modules

build/mdl-with-image-plugin/matconv mtlx-to-mdl \
  --input material.mtlx --output material.mdl \
  --material material_name
```

`--material` is optional; `--mdl-path` can be repeated. Use `--strict` with
`mdl-to-mtlx` to reject warnings. Referenced textures are copied beside the
output. Generated MDL requires MaterialX MDL support modules on the consuming
runtime's search path.

See the [demos](demo/) for examples. Check asset licenses before converting or
redistributing materials.
