// Check the published contract independently of the current enum/spec tables.
#include "clap_harness.hpp"
#include <fstream>
#include <sstream>

using namespace harness;

int main(int argc, char** argv) {
  assert(argc == 3);
  const Library library(argv[1]);
  const auto* plugin = library.create();
  std::ifstream input(argv[2]);
  assert(input);
  std::string line;
  size_t checked = 0;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    std::string kind, number, expected;
    assert(std::getline(row, kind, '\t'));
    assert(std::getline(row, number, '\t'));
    assert(std::getline(row, expected));
    const auto id = static_cast<clap_id>(std::stoul(number));
    std::string actual;
    if (kind == "parameter") {
      const auto info = param_info(plugin, id);
      assert(info.id == id);
      actual = info.name;
    } else {
      assert(kind == "source" || kind == "preset");
      const auto param_id = find_param(plugin, kind == "source" ? "Waveform" : "Preset");
      assert(param_info(plugin, param_id).max_value >= id);
      char text[256]{};
      const bool ok = params_of(plugin)->value_to_text(plugin, param_id, id, text, sizeof(text));
      assert(ok);
      actual = text;
    }
    if (actual != expected) {
      std::fprintf(stderr, "%s ID %u changed: expected '%s', got '%s'\n",
          kind.c_str(), id, expected.c_str(), actual.c_str());
      assert(false && "published automation and catalogue IDs must remain compatible");
    }
    ++checked;
  }
  assert(checked == 277);
  plugin->destroy(plugin);
  std::puts("compatibility_tests: 136 parameter, 59 source, and 82 preset IDs preserved");
}
