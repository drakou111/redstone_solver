#include <cstdio>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#include <httplib.h>
#include <webview/webview.h>

#include "host/api.hpp"
#include "host/embedded_files.hpp"

namespace {

constexpr int kWindowWidth = 1600;
constexpr int kWindowHeight = 1000;
constexpr int kTokenBytes = 16;
constexpr const char *kHost = "127.0.0.1";
constexpr const char *kUsage =
    "Usage: redstone_solver [--serve] [--port <n>] [--token <t>] [--help]\n"
    "\n"
    "Opens the Redstone Solver window.\n"
    "  --serve     no window: serve the UI and print its URL (for testing)\n"
    "  --port <n>  port to serve on (default: any free port)\n"
    "  --token <t> session token instead of a random one (for testing)\n";

std::string random_token() {
  std::random_device device;
  std::string token;
  for (int i = 0; i < kTokenBytes; ++i)
    token += std::format("{:02x}", device() & 0xFF);
  return token;
}

// The UI and its API on 127.0.0.1. API calls must carry the session token so
// other local pages cannot drive the solver or write files.
void configure(httplib::Server &server, rs::host::Api &api,
               const std::string &token) {
  server.Post(R"(/api/(.+))", [&api, token](const httplib::Request &request,
                                            httplib::Response &response) {
    if (request.get_header_value("X-Token") != token) {
      response.status = 403;
      response.set_content(R"({"error":"bad session token"})",
                           "application/json");
      return;
    }
    try {
      rs::host::Json body = request.body.empty()
                                ? rs::host::Json::object()
                                : rs::host::Json::parse(request.body);
      rs::host::Json reply = api.handle(request.matches[1].str(), body);
      response.set_content(reply.dump(), "application/json");
    } catch (const std::exception &error) {
      // Reported to the UI, which shows it to the user.
      response.status = 400;
      response.set_content(rs::host::Json{{"error", error.what()}}.dump(),
                           "application/json");
    }
  });

  server.Get(R"(/.*)", [](const httplib::Request &request,
                          httplib::Response &response) {
    std::string path = request.path == "/" ? "/index.html" : request.path;
    for (const rs::host::EmbeddedFile &file : rs::host::embedded_files()) {
      if (file.path != path)
        continue;
      response.set_content(reinterpret_cast<const char *>(file.data), file.size,
                           std::string(file.mime));
      return;
    }
    response.status = 404;
  });
}

} // namespace

int main(int argc, char **argv) {
  bool serve_only = false;
  int port = 0;
  std::string token = random_token();
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::fputs(kUsage, stdout);
      return 0;
    }
    if (arg == "--serve") {
      serve_only = true;
    } else if (arg == "--port" && i + 1 < argc) {
      port = std::stoi(argv[++i]);
    } else if (arg == "--token" && i + 1 < argc) {
      token = argv[++i];
    } else {
      std::fprintf(stderr, "unknown argument: %s\n\n%s", argv[i], kUsage);
      return 2;
    }
  }

  std::filesystem::path program_dir =
      std::filesystem::absolute(argv[0]).parent_path();
  rs::host::Api api(program_dir / "examples");

  httplib::Server server;
  configure(server, api, token);
  if (port == 0)
    port = server.bind_to_any_port(kHost);
  else if (!server.bind_to_port(kHost, port))
    port = -1;
  if (port < 0) {
    std::fprintf(stderr, "cannot listen on %s\n", kHost);
    return 1;
  }
  std::thread listener([&server] { server.listen_after_bind(); });
  std::string url = std::format("http://{}:{}/?token={}", kHost, port, token);

  if (serve_only) {
    std::printf("%s\n", url.c_str());
    std::fflush(stdout);
    listener.join();
    return 0;
  }

  {
    webview::webview window(false, nullptr);
    window.set_title("Redstone Solver");
    window.set_size(kWindowWidth, kWindowHeight, WEBVIEW_HINT_NONE);
    window.navigate(url);
    window.run();
  }
  server.stop();
  listener.join();
  return 0;
}
