#pragma once

#include <OpenImageIO/imageio.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace matconv::diagnostic {

inline std::vector<float> read_texture(OIIO::ImageInput &image, bool linear) {
  const OIIO::ImageSpec &spec = image.spec();
  if (spec.width <= 0 || spec.height <= 0 || spec.depth != 1 ||
      spec.nchannels < 1 || spec.nchannels > 4)
    throw std::runtime_error("Diagnostic renderer: unsupported image dimensions or channel count.");

  const int alpha = spec.alpha_channel;
  if (alpha < -1 || alpha >= spec.nchannels)
    throw std::runtime_error("Diagnostic renderer: invalid alpha channel.");
  const int colors = spec.nchannels - (alpha >= 0 ? 1 : 0);
  if (colors != 1 && colors != 3)
    throw std::runtime_error("Diagnostic renderer: expected grayscale or RGB image channels.");

  const std::size_t width = static_cast<std::size_t>(spec.width);
  const std::size_t height = static_cast<std::size_t>(spec.height);
  if (width > std::numeric_limits<std::size_t>::max() / 4 / height)
    throw std::runtime_error("Diagnostic renderer: image dimensions are too large.");
  std::vector<float> pixels(width * height * 4);
  if (!image.read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT,
                        pixels.data() + (height - 1) * width * 4,
                        4 * sizeof(float), -static_cast<OIIO::stride_t>(width * 4 * sizeof(float)),
                        OIIO::AutoStride))
    throw std::runtime_error("Diagnostic renderer: could not read image: " +
                             image.geterror());

  int color_indices[3] = {};
  int index = 0;
  for (int channel = 0; channel < spec.nchannels; ++channel)
    if (channel != alpha)
      color_indices[index++] = channel;

  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    const float r = pixels[i + color_indices[0]];
    const float g = colors == 1 ? r : pixels[i + color_indices[1]];
    const float b = colors == 1 ? r : pixels[i + color_indices[2]];
    const float a = alpha < 0 ? 1.0f : pixels[i + alpha];
    pixels[i] = linear ? r : std::pow(r, 2.2f);
    pixels[i + 1] = linear ? g : std::pow(g, 2.2f);
    pixels[i + 2] = linear ? b : std::pow(b, 2.2f);
    pixels[i + 3] = a;
  }
  return pixels;
}

} // namespace matconv::diagnostic
