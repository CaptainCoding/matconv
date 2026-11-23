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
GGX roughness is converted to the standard-surface convention. The two
anisotropic roughness axes and tangent are mapped to its roughness, anisotropy,
and rotation controls; extreme anisotropy and roughness outside its supported
range are clamped. Reflection and transmission roughness are kept separate for supported
reflective layers over transmissive bases, although anisotropy and other
layered combinations still share standard-surface controls.
MaterialX also provides composable BSDF nodes, including conductor, dielectric,
generalized Schlick, mix, and layer nodes. These can retain more separate lobe
parameters than a single `standard_surface`, but the converter does not yet
emit them for surface scattering; their semantics and renderer support must
be considered when mapping MDL mixtures and layers.
MDL `simple_glossy_bsdf` uses a Phong distribution with V-cavities masking;
it is approximated with GGX, so highlights and grazing angles can differ.
Standalone reflection-only glossy BSDFs use a metallic approximation because
their MDL reflection mode does not apply dielectric Fresnel attenuation. The resulting
grazing-angle reflectance can differ. A weighted reflective GGX layer over a
metallic GGX base is approximated using blended roughness, anisotropy and color
plus a coat. The coat's Fresnel response differs from an MDL weighted layer,
and both lobes share one tangent. This approximation cannot reproduce two
independently weighted lobes exactly.
Supported conditional BSDFs select between approximated surface inputs using
their original conditions. Fresnel layers and specular coatings are
approximations and can still differ visibly, especially for thin-walled glass.
Weighted nested layers, conductors and thin-film subtrees blend their
standard-surface inputs; this preserves the outer weight but is not a physical
equivalent of general MDL layering. Enum parameters used for conversion are
fixed to their MDL defaults and reported as warnings. Unsupported directional
and measured-curve factors are omitted with a warning while their underlying
BSDF is retained when it can be translated.
Supported BSDF tint modifiers retain separate reflection and transmission colors.
MDL sheen is approximated using the standard-surface sheen and its translated
multiscatter base; the multiscatter tint and layer order can differ.
The scalar IOR used for MDL Abbe dispersion does not reproduce dispersion.
For MDL generation, the loaded standard-surface implementation is adjusted to
forward its uniform `thin_walled` input to the surface constructor.
Not every MDL operation has a standard MaterialX equivalent. In particular,
multiscatter tint and BSDF handles are not fully represented, and emission
normalization may differ. Unsupported expressions and material slots are
reported as warnings; an unsupported top-level surface BSDF is approximated
with neutral gray diffuse. Texture conversion requires the MDL OpenImageIO plugin; other
unsupported features include measured distributions, hair, and volume
conversion. Generated documents are validated before they are written.
`base::file_texture` color and average-mono outputs retain their image,
color scale, and offset; other mono modes are not yet translated. UV-space
`base::coordinate_source` and supported `base::transform_coordinate` matrices
preserve their UV coordinates, including parameterized matrix products.
Unsupported coordinate expressions fall back to UV0 with a warning.
Tangent-space normal textures and transformed tangent frames are translated
where supported. A BSDF layer normal is normally approximated using the shared
standard-surface normal; a GGX layer mapped to a coat uses `coat_normal`.
Other independently layered normals may differ.
Object-space projections, other matrix expressions, mirrored normal-map
wrapping, cropping, clipping, and animation can still differ.
MDL gamma 2.2 images are tagged `g22_rec709`; MDL generation uses MaterialX
color management to convert tagged colors into linear Rec.709. Non-color image
lookups may not receive the same gamma correction.
Constant-component vector indexing, numeric and boolean conditionals, and
scalar `math::fmod` are translated. `base::blend_color_layers` supports
constructor arrays with blend, add, multiply, and overlay modes; other modes are reported as
unsupported. When only one conditional branch can be translated, it is used
for both branches with a warning.
Supported `base::perlin_noise_texture` color outputs use summed MaterialX 3D
noise; MDL uses 4D noise, so the pattern differs. Nonzero phase, distortion,
ridged/marble/dent modes, banding, dynamic octave counts or sizes, and the
mono output are not supported. Object and world position coordinates are
preserved when their transforms can be translated.

## Converter structure

The MDL-to-MaterialX entry point and material assembly live in
[`mdl_to_materialx.cpp`](src/mdl_to_materialx.cpp). The expression translator
separates [general expression translation](src/mdl_expression_translator.cpp),
[BSDF approximations](src/mdl_bsdf_translation.cpp),
[texture translation](src/mdl_texture_translation.cpp), and
[shared graph and MDL helpers](src/mdl_translation_utils.cpp). The separate
[native BSDF translation](src/mdl_native_bsdf_translation.cpp) is not the
active surface-output strategy. These files share the translator state; this
organization does not change the conversion strategy or its documented
limitations.

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

For `mdl-to-mtlx`, `--strict` rejects any conversion warning and does not
write the MaterialX document. Texture resources may already have been copied
while evaluating the material. This is a conservative warning policy, not a
proof that a warning-free conversion is physically equivalent.
The MaterialX MDL generator may expose multiple generated MDL parameters for
one shared MaterialX graph input; its default value is preserved, but editing
one generated parameter does not necessarily update the others.

MDL and vMaterials licenses may restrict conversion, redistribution, or use of
converted assets. Check the applicable licenses before processing or sharing
materials.
