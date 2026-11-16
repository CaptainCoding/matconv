# MDL to MaterialX demo

This demo converts `basic.mdl` to `basic.mtlx` with the MDL SDK. The generated
graph preserves the `base_color` and `roughness` parameters and approximates
MDL diffuse reflection with MaterialX Oren–Nayar diffuse scattering.

Run from the repository root after building with SDK dependencies:

```sh
build/matconv mdl-to-mtlx \
  --input demo/mdl-to-mtlx/basic.mdl \
  --output demo/mdl-to-mtlx/basic.mtlx \
  --material basic \
  --mdl-path demo/mdl-to-mtlx
```

The converter reports that MDL `multiscatter_tint`, the BSDF handle, and the
material IOR are not represented. This is a working example of the currently
supported subset, not a lossless conversion.
