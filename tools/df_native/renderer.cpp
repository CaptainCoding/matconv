#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#include "diagnostic_example_df_native.cpp"

static void lookup_bsdf_table(
    float result[3], const mi::mdl::Texture_handler_base *base,
    mi::Uint32 index, const float coord[3], Tex_wrap_mode wrap_u,
    Tex_wrap_mode wrap_v, Tex_wrap_mode wrap_w, const float crop_u[2],
    const float crop_v[2], const float crop_w[2], float) {
  const auto *handler = static_cast<const Texture_handler *>(base);
  if (index == 0 || index > handler->num_textures) {
    store_result3(result, 0.0f);
    return;
  }
  const Texture &texture = handler->textures[index - 1];
  if (texture.ncomp != 1 || wrap_u != Tex_wrap_mode::wrap_clamp ||
      wrap_v != Tex_wrap_mode::wrap_clamp ||
      wrap_w != Tex_wrap_mode::wrap_clamp || crop_u[0] != 0.0f ||
      crop_u[1] != 1.0f || crop_v[0] != 0.0f || crop_v[1] != 1.0f ||
      crop_w[0] != 0.0f || crop_w[1] != 1.0f || texture.size.x == 0 ||
      texture.size.y == 0 || texture.size.z == 0) {
    std::cerr << "Unsupported 3D texture lookup in diagnostic renderer\n";
    std::abort();
  }

  const unsigned sizes[] = {texture.size.x, texture.size.y, texture.size.z};
  unsigned lo[3], hi[3];
  float fraction[3];
  for (unsigned axis = 0; axis < 3; ++axis) {
    const float position =
        std::clamp(coord[axis], 0.0f, 1.0f) * sizes[axis] - 0.5f;
    const int base_index = static_cast<int>(std::floor(position));
    fraction[axis] = position - std::floor(position);
    lo[axis] = static_cast<unsigned>(
        std::clamp(base_index, 0, static_cast<int>(sizes[axis]) - 1));
    hi[axis] = static_cast<unsigned>(
        std::clamp(base_index + 1, 0, static_cast<int>(sizes[axis]) - 1));
  }

  float value = 0.0f;
  for (unsigned z = 0; z < 2; ++z)
    for (unsigned y = 0; y < 2; ++y)
      for (unsigned x = 0; x < 2; ++x) {
        const unsigned ix = x ? hi[0] : lo[0];
        const unsigned iy = y ? hi[1] : lo[1];
        const unsigned iz = z ? hi[2] : lo[2];
        const float weight = (x ? fraction[0] : 1.0f - fraction[0]) *
                             (y ? fraction[1] : 1.0f - fraction[1]) *
                             (z ? fraction[2] : 1.0f - fraction[2]);
        value += weight * texture.data[(iz * sizes[1] + iy) * sizes[0] + ix];
      }
  store_result3(result, value);
}

int MAIN_UTF8(int argc, char **argv) {
  tex_vtable.m_tex_lookup_float3_3d = lookup_bsdf_table;
  try {
    return sdk_df_native_main(argc, argv);
  } catch (const std::runtime_error &error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

COMMANDLINE_TO_UTF8
