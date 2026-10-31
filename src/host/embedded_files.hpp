#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace rs::host {

// The built web UI, compiled into the program (generated at build time).
struct EmbeddedFile {
  std::string_view path; // "/index.html", "/assets/..."
  std::string_view mime;
  const unsigned char *data;
  std::size_t size;
};

std::span<const EmbeddedFile> embedded_files();

} // namespace rs::host
