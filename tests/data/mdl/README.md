# MDL converter cases

Each `.mdl` file isolates a conversion feature. `run_mdl_cases.cmake` converts
each case and checks that its MaterialX graph contains the expected node rather
than silently accepting a black fallback.

| File | Focus |
|---|---|
| `diffuse_matte.mdl` | diffuse color and roughness |
| `ggx_reflect.mdl` | GGX reflection |
| `ggx_transmit.mdl` | GGX transmission |
| `weighted_layer.mdl` | weighted BSDF layering |
| `custom_curve_layer.mdl` | Fresnel-shaped BSDF layering |
| `thin_film.mdl` | thin-film parameters |
| `emission.mdl` | surface emission |
| `color_lerp.mdl` | componentwise color interpolation |
| `textured_diffuse.mdl` | UV0 image lookup using `textures/checker.png` |

Render the source texture case using the local MDL Core Viewer, the project HDR,
and 512×512 at 32 spp:

```sh
build/mdl-core-viewer-vcpkg/examples/mdl_core/df_native/Debug/df_native \
  --no_window --hdr approaching_storm.hdr --spp 32 --res 512 512 \
  -o build/mdl-cases/source_textured_diffuse.png \
  -p demo -p tests/data/mdl ::textured_diffuse::textured_diffuse
```

The source render shows the checkerboard. Texture lookup and image-resource preservation are included as required
conversion cases. Configure with the `mdl-with-image-plugin` preset so the MDL
SDK can resolve the image resource; a successful command exit alone is not
considered a passing conversion.
