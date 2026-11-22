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
| `conditional_bsdf.mdl` | Parameter-driven BSDF branches and scalar Abbe IOR |
| `specular_coat.mdl` | Specular reflection layer over transmissive GGX |
| `fresnel_glass.mdl` | Thin-walled Fresnel reflection and transmission |
| `curve_reflection.mdl` | Custom-curve reflection over an empty base |
| `curve_film_zero.mdl` | Custom-curve reflection with a zero-thickness thin film |
| `weighted_layer.mdl` | weighted BSDF layering |
| `custom_curve_layer.mdl` | Fresnel-shaped BSDF layering |
| `thin_film.mdl` | thin-film parameters |
| `emission.mdl` | surface emission |
| `color_lerp.mdl` | componentwise color interpolation |
| `textured_diffuse.mdl` | UV0 image lookup using `textures/checker.png` |
| `textured_roughness.mdl` | UV0 image-driven GGX roughness |
| `base_file_texture.mdl` | `base::file_texture` color and average mono with scale and offset |
| `base_file_texture_alpha.mdl` | Explicit warning for unsupported alpha mono output |
| `base_file_texture_transformed.mdl` | Identity texture-coordinate matrix |
| `texture_channels.mdl` | Texture channel indexing, numeric selection and negation |
| `metallic_diffuse_layer.mdl` | Color-weighted metallic and diffuse layering approximation |
| `color_layers.mdl` | Layer blend/add, boolean expressions and signed scalar remainder |
| `unsupported_color_layer.mdl` | Unsupported mode warning and propagation of failed expressions |
| `uv_transform.mdl` | Parameterized scale, translation and matrix-product rotation |
| `unsupported_uv.mdl` | Explicit UV0 fallback for object-space coordinates |
| `normal_layer.mdl` | Tangent-space normal texture on a BSDF layer |
| `normal_layer_uv.mdl` | Rotated and scaled UV coordinates with a normal texture |
| `normal_geometry.mdl` | Rotated normal texture on geometry, including tangent flip |

Configure with the `mdl-with-image-plugin` preset so the MDL SDK can resolve
the image resource. The conversion tests check document contents and resource
packaging; they do not render or establish visual equivalence.

The MDL SDK's `df_native` example currently returns zero for 3D texture lookups.
GGX multiscatter uses 3D BSDF tables, so the example can render valid materials
white or black, particularly generated materials in class-compilation mode.
Local diagnostic renders with a 3D BSDF-table lookup no longer exhibited these
failures in the reflection, glass, diffuse and texture cases tested. This does
not establish equivalence for all materials or renderer configurations.
