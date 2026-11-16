# matconv

`matconv` is a C++ command-line tool for experimental, bidirectional conversion
between NVIDIA MDL and MaterialX. Conversion is lossy. MaterialX-to-MDL uses
MaterialX's MDL shader generator, so supported nodes depend on the pinned
MaterialX generator.
Generated MDL imports MaterialX support modules; those modules must be available
on the consuming MDL runtime's module search path. Generated MaterialX uses
standard-library node categories and should be loaded with MaterialX's standard
libraries.

## MDL to MaterialX

The MDL converter compiles each material in class-compilation mode and
translates its compiled expression DAG into a MaterialX node graph. Supported
class parameters of scalar, color, and vector types become graph inputs with
their MDL defaults. The initial expression mapping includes common arithmetic
and math operations, diffuse reflection, and diffuse emission, plus material
opacity, thin-walled state, backface, and displacement slots.

Not every MDL operation has a standard MaterialX equivalent. In particular,
diffuse reflection is approximated with `oren_nayar_diffuse_bsdf` (MDL's
`multiscatter_tint` and BSDF handle are not represented), and diffuse emission
is mapped to `uniform_edf` without guaranteeing equivalent normalization.
Unsupported expressions and material slots are reported as warnings; an
unsupported top-level surface BSDF is explicitly replaced with black diffuse
scattering. Textures, measured distributions, hair, and volume conversion are
not yet implemented. Generated documents are validated before they are
written.

## Build

Build with the MDL SDK and MaterialX to enable conversion:

```sh
./build.sh
```

The script fetches the pinned dependencies as CMake subprojects, builds with
four parallel jobs, and runs the tests. The executable is `build/matconv`;
fetched sources and build files are stored under `build/`.

The equivalent commands are:

```sh
cmake -S . -B build -DMATCONV_FETCH_DEPENDENCIES=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Fetching is opt-in because both dependency source trees and builds are
substantial. CMake 3.26 or newer is required when fetching. A CMake
configuration without `-DMATCONV_FETCH_DEPENDENCIES=ON` does not search for or
link locally installed SDKs, but cannot perform conversions.

## Usage

```sh
matconv mdl-to-mtlx \
  --input /path/to/material.mdl \
  --output /path/to/material.mtlx \
  --material material_name \
  --mdl-path /path/to/mdl/modules

matconv mtlx-to-mdl \
  --input /path/to/material.mtlx \
  --output /path/to/material.mdl \
  --material material_name
```

`--mdl-path` may be repeated. The input MDL file's own directory and the
MaterialX MDL support modules fetched by CMake are also added to the MDL search
path. Other MDL module dependencies and resource paths must be available to the
SDK through these paths.

MDL and vMaterials licenses may restrict conversion, redistribution, or use of
converted assets. Check the applicable licenses before processing or sharing
materials.
