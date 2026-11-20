#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>

#include <SDL3/SDL.h>

#include <rex/input/sdl/joystick_probe.h>

using namespace rex::input::sdl;

namespace {

std::FILE* OpenTempFile() {
#ifdef _WIN32
  std::FILE* file = nullptr;
  return tmpfile_s(&file) == 0 ? file : nullptr;
#else
  return std::tmpfile();
#endif
}

}  // namespace

TEST_CASE("Probe device lines are JSON with escaped names", "[input][probe]") {
  ProbeDevice d;
  d.id = 3;
  d.name = R"(MOZA "R5" \ Base)";
  d.path = R"(\\?\HID#VID_346E)";
  d.vendor = 0x346e;
  d.product = 0x0004;
  d.wheel = true;
  d.rest = {0, -32768};
  d.buttons = 64;
  d.hats = 1;
  CHECK(ProbeDeviceLine(d) ==
        R"({"device":{"id":3,"name":"MOZA \"R5\" \\ Base","path":"\\\\?\\HID#VID_346E",)"
        R"("vendor":"346e","product":"0004","wheel":true,"rest":[0,-32768],"buttons":64,)"
        R"("hats":1}})");
}

TEST_CASE("Probe state lines give buttons as a 0/1 string", "[input][probe]") {
  ProbeState s;
  s.id = 3;
  s.axes = {12, -32768};
  s.buttons = {false, true, false, false};
  s.hats = {0};
  CHECK(ProbeStateLine(s) ==
        "{\"state\":{\"id\":3,\"axes\":[12,-32768],\"buttons\":\"0100\",\"hats\":[0]}}");
  CHECK(ProbeRemovedLine(3) == "{\"removed\":3}");
}

TEST_CASE("The probe streams a virtual wheel", "[input][probe]") {
  REQUIRE(SDL_InitSubSystem(SDL_INIT_JOYSTICK));
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_WHEEL;
  desc.naxes = 2;
  desc.nbuttons = 4;
  desc.nhats = 1;
  desc.name = "Probe Test Wheel";
  const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  REQUIRE(id != 0);

  std::FILE* out = OpenTempFile();
  REQUIRE(out);
  int loops = 0;
  CHECK(RunJoystickProbe(out, [&] { return loops++ < 1; }) == 0);
  std::string text;
  std::rewind(out);
  for (int c = std::fgetc(out); c != EOF; c = std::fgetc(out)) {
    text.push_back(static_cast<char>(c));
  }
  std::fclose(out);
  SDL_DetachVirtualJoystick(id);
  SDL_QuitSubSystem(SDL_INIT_JOYSTICK);

  INFO(text);
  CHECK(text.find("\"name\":\"Probe Test Wheel\"") != std::string::npos);
  CHECK(text.find("\"wheel\":true") != std::string::npos);
  CHECK(text.find("{\"ready\":true}") != std::string::npos);
  CHECK(text.find("{\"state\":{\"id\":" + std::to_string(id) + ",") != std::string::npos);
}
