# MDL converter cases

Each `.mdl` file isolates a conversion feature. `run_mdl_cases.cmake` converts
each case and checks that the result contains a renderable MaterialX
`standard_surface` approximation rather than a black BSDF fallback. It also
checks that the reverse MaterialX-to-MDL conversion exports the expected
material. Referenced image assets are copied into a `textures` folder beside
the converted file in either direction, and the generated resource paths are
rebased to that folder. This intentionally trades some MDL-specific lobe
detail for compatibility with MaterialXView and other standard-surface
renderers. Conversion warnings identify approximated features;
`mtlx-to-mdl` continues to use MaterialX's MDL shader generator for the
reverse direction.

| File | Focus |
|---|---|
| `diffuse_matte.mdl` | diffuse color and roughness |
| `ggx_reflect.mdl` | GGX reflection |
| `ggx_transmit.mdl` | Smooth GGX transmission with material IOR 1.6 |
| `ggx_transmit_rough.mdl` | Rough, tinted GGX transmission with material IOR 1.6 |
| `ggx_glass_smooth.mdl` | Smooth combined GGX reflection and transmission |
| `ggx_glass_rough.mdl` | Rough, tinted combined GGX reflection and transmission |
| `ggx_multiscatter.mdl` | GGX reflection with multiscatter tint |
| `curve_reflection.mdl` | Custom-curve reflection over an empty base |
| `curve_film_zero.mdl` | Custom-curve reflection with a zero-thickness thin film |
| `weighted_layer.mdl` | weighted BSDF layering |
| `custom_curve_layer.mdl` | Fresnel-shaped BSDF layering |
| `thin_film.mdl` | thin-film parameters |
| `emission.mdl` | surface emission |
| `color_lerp.mdl` | componentwise color interpolation |
| `textured_diffuse.mdl` | UV0 image lookup using `textures/checker.png` |
| `textured_roughness.mdl` | UV0 image-driven GGX roughness |

Configure with the `mdl-with-image-plugin` preset so the MDL SDK can resolve
the image resource. The conversion tests check document contents and resource
packaging; they do not render or establish visual equivalence.

The MDL SDK's `df_native` example currently returns zero for 3D texture lookups.
GGX multiscatter uses 3D BSDF tables, so the example can render valid materials
white or black, particularly generated materials in class-compilation mode.
Local diagnostic renders with a 3D BSDF-table lookup no longer exhibited these
failures in the reflection, glass, diffuse and texture cases tested. This does
not establish equivalence for all materials or renderer configurations.
