#include "host/catalog_file.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "host/json_io.hpp"

namespace rs::host {

namespace {

Json cell_json(Vec3 p) { return Json::array({p.x, p.y, p.z}); }

Vec3 cell_from(const Json &json) {
  return {json.at(0).get<int>(), json.at(1).get<int>(), json.at(2).get<int>()};
}

} // namespace

void load_catalog(const std::filesystem::path &file, GateCatalog &catalog) {
  std::ifstream in(file);
  if (!in)
    return;
  std::stringstream text;
  text << in.rdbuf();
  Json json = Json::parse(text.str(), nullptr, false);
  if (json.is_discarded() || !json.is_array())
    throw std::runtime_error("the gate catalog " + file.string() +
                             " is damaged");
  for (const Json &entry : json) {
    GateModule module;
    for (const Json &f : entry.at("functions"))
      module.functions.push_back(f.get<std::uint8_t>());
    module.layout = world_from_json(entry.at("layout"));
    if (entry.contains("ins")) {
      for (const Json &port : entry.at("ins"))
        module.ins.push_back(cell_from(port));
    } else {
      // Written before modules had more than two inputs.
      module.ins = {cell_from(entry.at("in1")), cell_from(entry.at("in2"))};
    }
    for (const Json &out : entry.at("outs"))
      module.outs.push_back(cell_from(out));
    catalog.add(entry.at("key").get<std::string>(), module);
  }
}

void save_catalog(const std::filesystem::path &file,
                  const GateCatalog &catalog) {
  Json json = Json::array();
  for (const auto &[key, module] : catalog.all()) {
    Json ins = Json::array();
    for (Vec3 in : module.ins)
      ins.push_back(cell_json(in));
    Json outs = Json::array();
    for (Vec3 out : module.outs)
      outs.push_back(cell_json(out));
    json.push_back({{"key", key},
                    {"functions", module.functions},
                    {"layout", to_json(module.layout)},
                    {"ins", ins},
                    {"outs", outs}});
  }
  std::filesystem::path temporary = file;
  temporary += ".tmp";
  {
    std::ofstream out(temporary);
    if (!out)
      throw std::runtime_error("cannot write " + temporary.string());
    out << json.dump();
  }
  std::filesystem::rename(temporary, file);
}

} // namespace rs::host
