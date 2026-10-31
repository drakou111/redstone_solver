#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <GLFW/glfw3.h>

#include "gui/app.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace {

constexpr int kInitialWidth = 1500;
constexpr int kInitialHeight = 950;
constexpr float kBaseFontSize = 16.0f;
constexpr double kIdleWaitSeconds = 1.0 / 30.0;
constexpr const char *kGlslVersion = "#version 330";
constexpr const char *kUiFont = "C:/Windows/Fonts/segoeui.ttf";

constexpr int kScreenshotWarmupFrames = 5;

void glfw_error(int code, const char *description) {
  std::fprintf(stderr, "GLFW error %d: %s\n", code, description);
}

// Writes the current back buffer as a 24-bit BMP.
void save_screenshot(const std::filesystem::path &path, int width, int height) {
  constexpr int kBmpHeaderSize = 54;
  constexpr int kDibHeaderSize = 40;
  int row = (width * 3 + 3) & ~3;
  std::vector<unsigned char> pixels(static_cast<std::size_t>(row) * height);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadBuffer(GL_BACK);
  glReadPixels(0, 0, width, height, GL_BGR_EXT, GL_UNSIGNED_BYTE,
               pixels.data());

  unsigned char header[kBmpHeaderSize] = {'B', 'M'};
  auto put32 = [&](int at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k)
      header[at + k] = static_cast<unsigned char>(v >> (8 * k));
  };
  put32(2, kBmpHeaderSize + static_cast<std::uint32_t>(pixels.size()));
  put32(10, kBmpHeaderSize);
  put32(14, kDibHeaderSize);
  put32(18, static_cast<std::uint32_t>(width));
  put32(22, static_cast<std::uint32_t>(height));
  header[26] = 1;  // planes
  header[28] = 24; // bits per pixel
  std::ofstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot write " + path.string());
  file.write(reinterpret_cast<const char *>(header), kBmpHeaderSize);
  file.write(reinterpret_cast<const char *>(pixels.data()),
             static_cast<std::streamsize>(pixels.size()));
}

} // namespace

// Usage: redstone_solver [spec-file] [--solve] [--tab <name>]
//                        [--screenshot <file.bmp>]
// --screenshot renders off screen once solving is done, saves the image and
// exits (used to check the UI without touching the desktop).
int main(int argc, char **argv) {
  std::string spec_path;
  std::string tab;
  std::filesystem::path screenshot;
  bool solve_now = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--solve")
      solve_now = true;
    else if (arg == "--tab" && i + 1 < argc)
      tab = argv[++i];
    else if (arg == "--screenshot" && i + 1 < argc)
      screenshot = std::filesystem::absolute(argv[++i]);
    else
      spec_path = std::filesystem::absolute(arg).string();
  }

  // Relative paths (examples/) resolve next to the program. If this fails the
  // app falls back to a built-in spec and says so.
  std::error_code ignored;
  std::filesystem::current_path(
      std::filesystem::absolute(argv[0]).parent_path(), ignored);

  glfwSetErrorCallback(glfw_error);
  if (!glfwInit())
    return 1;
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  if (!screenshot.empty())
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  GLFWwindow *window = glfwCreateWindow(kInitialWidth, kInitialHeight,
                                        "Redstone Solver", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();

  float scale = 1.0f;
  glfwGetWindowContentScale(window, &scale, nullptr);
  ImGui::GetStyle().ScaleAllSizes(scale);
  if (std::filesystem::exists(kUiFont))
    io.Fonts->AddFontFromFileTTF(kUiFont, kBaseFontSize * scale);
  else
    io.FontGlobalScale = scale;

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(kGlslVersion);

  {
    rs::gui::App app(spec_path, solve_now);
    if (!tab.empty())
      app.select_tab(tab);
    int idle_frames = 0;
    while (!glfwWindowShouldClose(window)) {
      glfwWaitEventsTimeout(kIdleWaitSeconds);
      if (glfwGetWindowAttrib(window, GLFW_ICONIFIED))
        continue;
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      app.draw();
      ImGui::Render();

      int width = 0;
      int height = 0;
      glfwGetFramebufferSize(window, &width, &height);
      glViewport(0, 0, width, height);
      glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      if (!screenshot.empty()) {
        idle_frames = app.is_solving() ? 0 : idle_frames + 1;
        if (idle_frames >= kScreenshotWarmupFrames) {
          save_screenshot(screenshot, width, height);
          break;
        }
      }
      glfwSwapBuffers(window);
    }
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
