#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#include <httplib.h>

#include "host/api.hpp"

namespace {

constexpr int kTokenBytes = 16;
constexpr const char *kHost = "127.0.0.1";
constexpr const char *kUsage =
    "Usage: redstone_solver [--serve] [--port <n>] [--token <t>] "
    "[--library <dir>] [--help]\n"
    "\n"
    "The solver behind the Minecraft mod: serves its JSON API on 127.0.0.1\n"
    "and prints the URL (with the session token) on the first line.\n"
    "  --serve     accepted for compatibility; serving is all it does\n"
    "  --port <n>  port to serve on (default: any free port)\n"
    "  --token <t> session token instead of a random one\n"
    "  --library <dir> keep verified circuits there; requests seen before\n"
    "              are answered from it at once\n";

std::string random_token() {
  std::random_device device;
  std::string token;
  for (int i = 0; i < kTokenBytes; ++i)
    token += std::format("{:02x}", device() & 0xFF);
  return token;
}

// API calls must carry the session token so other local programs and web
// pages cannot drive the solver or write files.
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
      // Reported to the mod, which shows it to the player.
      response.status = 400;
      response.set_content(rs::host::Json{{"error", error.what()}}.dump(),
                           "application/json");
    }
  });
}

} // namespace

int main(int argc, char **argv) {
  int port = 0;
  std::string token = random_token();
  std::optional<std::filesystem::path> library;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::fputs(kUsage, stdout);
      return 0;
    }
    if (arg == "--serve") {
      continue;
    } else if (arg == "--port" && i + 1 < argc) {
      port = std::stoi(argv[++i]);
    } else if (arg == "--token" && i + 1 < argc) {
      token = argv[++i];
    } else if (arg == "--library" && i + 1 < argc) {
      library = std::filesystem::path(argv[++i]);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n\n%s", argv[i], kUsage);
      return 2;
    }
  }

  rs::host::Api api(library);
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
  std::printf("http://%s:%d/?token=%s\n", kHost, port, token.c_str());
  std::fflush(stdout);
  server.listen_after_bind();
  return 0;
}
