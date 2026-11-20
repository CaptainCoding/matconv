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

MDL surface BSDFs are approximated with a renderable `standard_surface`.
GGX roughness is converted to the standard-surface convention; independent
roughness in layered reflection and transmission cannot always be preserved.
Standalone reflection-only GGX uses a metallic approximation because its MDL
reflection mode does not apply dielectric Fresnel attenuation. The resulting
grazing-angle reflectance can differ.
Not every MDL operation has a standard MaterialX equivalent. In particular,
multiscatter tint and BSDF handles are not fully represented, and emission
normalization may differ. Unsupported expressions and material slots are
reported as warnings; an unsupported top-level surface BSDF is approximated
with neutral gray diffuse. Texture conversion requires the MDL OpenImageIO plugin; other
unsupported features include measured distributions, hair, and volume
conversion. Generated documents are validated before they are written.

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

Fetching is opt-in because dependency sources and builds are substantial. CMake
3.26 or newer is required when fetching. To include MDL image-texture support,
install or bootstrap vcpkg and configure with `VCPKG_ROOT` pointing to its
checkout:

```sh
cmake --preset mdl-with-image-plugin
cmake --build build/mdl-with-image-plugin --parallel 4
ctest --test-dir build/mdl-with-image-plugin --output-on-failure
```

The preset opts into the `mdl-image-plugin` vcpkg feature, which fetches
OpenImageIO and its dependencies. A configuration without
`-DMATCONV_FETCH_DEPENDENCIES=ON` does not search for or link locally installed
SDKs, but cannot perform conversions.

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
SDK through these paths. Referenced local image files are copied to a
`textures` directory beside the output file, and resource paths are rebased
accordingly. Different files with the same name receive a suffix rather than
overwriting one another.

MDL and vMaterials licenses may restrict conversion, redistribution, or use of
converted assets. Check the applicable licenses before processing or sharing
materials.
