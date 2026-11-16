# GPUOpen MaterialX roundtrip probes

These are real-world probes of the current MaterialX → MDL → MaterialX path.
The source packages were downloaded from the GPUOpen MaterialX Library. Its
API lists AMD as author and reports `MIT Public Domain` for each material; the
library marks each selected package as verified MaterialX. The corresponding
MIT license text is in [LICENSE.txt](LICENSE.txt).

By default, `python3 demo/gpuopen/build_demo.py` does not access the network. It
reconverts the MaterialX files already present in `demo/gpuopen/catalog/` and
regenerates `demo/gpuopen/catalog_manifest.json`, `demo/gpuopen/all_materials.usda`,
and the per-material USD files in `demo/gpuopen/submodels/`. To fetch the latest
catalog and download its packages, explicitly pass `--download`:

```sh
python3 demo/gpuopen/build_demo.py --download
```

The download mode selects currently published materials whose API license is
`MIT Public Domain` or `CC0 Public Domain`, choosing each 1k/8-bit package (or
the smallest available package when that label is absent). The current catalog
reports 454 materials, all `MIT Public Domain`; the selected downloads total
about 1.85 GB before extraction. Both modes require the built `build/matconv`
executable and the Python `pxr` package to write the USD index. If no local
source materials exist, the default mode exits with instructions to use
`--download`.

The full catalog run processed all 454 materials: 451 generated roundtrip
MaterialX files with a black surface fallback, and 3 failed MDL loading. The
failed packages are Bardiglio Gray Marble Tiles, Blue Mosaic Tiles with Gold,
and Blue Portuguese Ceramic Tile. Their source graphs reference
`Tile_pattern_*.resources/*.png` files that are absent from the downloaded
packages; the generated MDL therefore contains an unresolved texture resource.
For these three materials the MDL and conversion log are retained, but no
roundtrip MaterialX file was produced. Their USD submodels still contain both
spheres; the converted sphere uses the documented magenta placeholder and
retains the failed conversion status.

The original GPUOpen packages are not generally texture-free: the catalog
contains 1,323 image files across 437 materials. The source-texture audit
repairs 10 path-separator or filename-case mismatches into sibling
`*_portable.mtlx` documents while preserving the downloaded MTLX files. Three
source graphs refer to image files that are not present in their packages.

The converted side is a different story: none of the 451 generated roundtrip
MaterialX documents contains an image node. The current MDL-to-MaterialX
translator loses the texture graph, so the converted spheres cannot show the
input textures. This is a known conversion limitation, not a download-size or
USD-link issue. The conversion gap is tracked in the list below.

Each file in `submodels/` contains two UV-mapped sphere meshes: original on the
left and converted on the right. Each sphere is bound to the corresponding
Material prim imported from its `.mtlx` file: the file is referenced as a USD
layer under a `Looks` scope, and the binding targets its
`/MaterialX/Materials/<material-name>` prim. This follows the MaterialX
file-format workflow used by `scenes/materialx_scene.usda`. The main
`all_materials.usda` stage references these submodels and lays them out in a
grid. Opening the MaterialX references requires the USD MaterialX file-format
plugin. When a conversion did not produce an `.mtlx` file, the converted
sphere uses a magenta `UsdPreviewSurface` placeholder and its conversion
status is stored on the material.

## Results

| Material | GPUOpen package | Source textures | Both commands write files? | Roundtrip result |
|---|---|---:|---|---|
| [Indigo Palm Wallpaper](indigo-palm/) | 1k/8b, 6.2 MB | 3 | Yes | Black surface fallback; texture nodes lost |
| [Copper](copper/) | 1k/8b, 0.87 KB | 0 | Yes | Black surface fallback |
| [White Plastic](white-plastic/) | 1k/8b, 0.78 KB | 0 | Yes | Black surface fallback |
| [Frosted Glass](frosted-glass/) | 1k/8b, 0.82 KB | 0 | Yes | Black surface fallback |
| [Old Painted Wood Planks](old-painted-wood-planks/) | 1k/8b, 3.58 MB | 3 | Yes | Black surface fallback; texture nodes lost |
| [TH Rusty Metallic Profile](th-rusty-metal-profile/) | 1k/8b, 4.19 MB | 3 | Yes | Black surface fallback; texture nodes lost |

Both conversion processes exited successfully for all six materials, and all
six generated MaterialX files parse as XML. That is only syntactic success:
none of these roundtrips preserves the source appearance. The source graphs
contain one `standard_surface` each; the generated MDL expands that node into
an MDL implementation that the current reverse translator cannot represent.
All six outputs contain an explicit black diffuse fallback. Textured samples
also have zero image nodes in the output.

The package IDs are Indigo Palm `cda6c347-8e03-4658-8382-dda9522dbf47`,
Copper `8a5a0476-3403-4862-95b5-63c970da30e1`, White Plastic
`9ad2257c-419c-43fe-833f-c538a6f0bcce`, Frosted Glass
`3b69679a-c252-4c79-9531-e42d859b89c6`, Old Painted Wood Planks
`bf363754-b246-4b11-ace3-31bd44714a7b`, and TH Rusty Metallic Profile
`102bf758-5a73-4216-9e52-516dc8421389`. The IDs link to the corresponding
public API records and packages, for example:
<https://api.matlib.gpuopen.com/api/packages/102bf758-5a73-4216-9e52-516dc8421389/>.

## Observed gaps for follow-up

1. **`df::thin_film` is unsupported.** The MaterialX MDL generator emits
   `::df::thin_film(float,color,bsdf)` for all six `standard_surface` materials.
   The MDL-to-MaterialX translator drops it, cannot translate
   `surface.scattering`, and substitutes black. This is the primary blocker
   even for untextured Copper, White Plastic, and Frosted Glass.
2. **EDF component arrays are unsupported.** `operator[](%3C0%3E[],int)` and
   `::df::unbounded_mix(::df::edf_component[N])` cannot be translated; the
   emission expression is omitted.
3. **Texture resources are not carried through.** All 451 generated roundtrip
   MaterialX files have zero image nodes, so input textures do not reach the
   output graphs. The source packages do contain textures for 437 materials;
   path case and separator issues are repaired in `*_portable.mtlx` files.
   Three source MTLX references point to images absent from their archives.
4. **Unsupported class-compilation parameter types are omitted.** The current
   interface conversion supports bool, int, float, color, and vectors; other
   parameter types produce repeated warnings. The MDL generated from these
   standard surfaces has 36–48 class parameters, and not all can become
   MaterialX graph inputs.
5. **Material slots remain incomplete.** IOR and volume scattering,
   absorption, and scattering-coefficient slots are reported as untranslated.
6. **Generated MDL depends on MaterialX support modules.** The converter now
   adds its fetched `MaterialXGenMdl/mdl` directory to the MDL search path
   automatically; the generated MDL remains dependent on those modules when
   consumed outside this project.
7. **Resource filename case can differ.** In the Indigo Palm package, the graph
   references `Normal.png` and `baseColor.png`, while the archive contains
   `normal.png` and `basecolor.png`. This is a portability risk on
   case-sensitive filesystems, separate from the missing image plugin.
8. **Some packages reference absent texture resources.** Three full-catalog
   materials refer to `Tile_pattern_*.resources` images that are not in their
   archives, preventing the MDL SDK from loading the generated MDL module.

These findings describe the tested examples, not an exhaustive list of every
unsupported MDL or MaterialX feature. The generated `.mdl` and roundtrip
`.mtlx` files are retained beside each source package so future fixes can be
compared against the same inputs.

## Re-run a probe

From the repository root, after building with the MDL SDK and MaterialX:

```sh
build/matconv mtlx-to-mdl \
  --input demo/gpuopen/copper/source/Copper.mtlx \
  --output demo/gpuopen/copper/roundtrip/Copper.mdl \
  --material Copper

build/matconv mdl-to-mtlx \
  --input demo/gpuopen/copper/roundtrip/Copper.mdl \
  --output demo/gpuopen/copper/roundtrip/Copper_roundtrip.mtlx \
  --material Copper \
  --mdl-path demo/gpuopen/copper/source
```
