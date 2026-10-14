#include "../core/project.hpp"
#include "../media/media_engine.hpp"
#include "app_paths.hpp"
#include "process.hpp"
#include <csignal>
#include <iostream>
#include <locale>
namespace {
std::atomic<bool> cancelled{false};
void interrupt(int) { cancelled = true; }
int cli(const std::vector<std::string> &args) {
  using namespace koralo;
  if (args.empty() || args[0] == "--help") {
    std::cout << "KoraLo offline tools\n  doctor\n  probe MEDIA\n  render "
                 "PROJECT.kora OUTPUT.mp4 [--cpu]\n  collect PROJECT.kora "
                 "NEW_DIRECTORY\n  --version\n";
    return 0;
  }
  if (args[0] == "--version") {
    std::cout << "KoraLo 0.7.0 " << KORALO_SOURCE_REVISION << '\n';
    return 0;
  }
  if (args[0] == "collect" && args.size() == 3) {
    std::string error;
    auto project = Project::load(args[1], &error);
    if (!project || !project->collect(args[2], &error)) {
      std::cerr << error << '\n';
      return 1;
    }
    std::cout << "Collected: " << args[2] << '\n';
    return 0;
  }
  if ((args[0] == "doctor" && args.size() != 1) ||
      (args[0] == "probe" && args.size() != 2) ||
      (args[0] == "render" && (args.size() < 3 || args.size() > 4 ||
                               (args.size() == 4 && args[3] != "--cpu"))) ||
      (args[0] != "doctor" && args[0] != "probe" && args[0] != "render")) {
    std::cerr << "Invalid command; use --help\n";
    return 2;
  }
  MediaEngine engine;
  if (args[0] == "doctor") {
    std::cout << engine.diagnostics()
              << "\nData: " << path_to_utf8(data_directory())
              << "\nCache: " << path_to_utf8(cache_directory()) << '\n';
    bool ok = engine.ready();
    for (const auto &path :
         {engine.ffmpeg_path(), engine.ffprobe_path(), engine.ffplay_path()}) {
      if (path.empty()) {
        ok = false;
        continue;
      }
      auto result = run_process(path, {"-version"});
      std::cout << result.output.substr(0, result.output.find('\n')) << '\n';
      ok = ok && result.started && result.exit_code == 0;
    }
    TemporaryFile probe(temporary_directory(), ".doctor");
    return ok && probe.valid() ? 0 : 1;
  }
  if (!engine.ready()) {
    std::cerr << engine.diagnostics() << '\n';
    return 1;
  }
  std::string error;
  if (args[0] == "probe") {
    auto media = engine.probe(args[1], &error);
    if (!media) {
      std::cerr << error << '\n';
      return 1;
    }
    std::cout << media->name << "\nDuration: " << media->duration
              << " s\nVideo: " << media->width << 'x' << media->height << " @ "
              << media->fps << " fps\nAudio: " << media->sample_rate << " Hz, "
              << media->audio_channels << " channels\n";
    return 0;
  }
  auto project = Project::load(args[1], &error);
  if (!project) {
    std::cerr << error << '\n';
    return 1;
  }
  RenderOptions options;
  options.use_gpu = args.size() != 4;
  std::signal(SIGINT, interrupt);
  std::signal(SIGTERM, interrupt);
  std::atomic<double> progress{0};
  if (!engine.render_project(*project, args[2], &progress, &error, &options,
                             &cancelled)) {
    std::cerr << error << '\n';
    return cancelled ? 130 : 1;
  }
  std::cout << "Rendered: " << args[2] << '\n';
  return 0;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  koralo::initialize_executable_path(
      koralo::path_to_utf8(std::filesystem::path(argv[0])));
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i)
    args.push_back(koralo::path_to_utf8(std::filesystem::path(argv[i])));
#else
int main(int argc, char **argv) {
  koralo::initialize_executable_path(argv[0]);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i)
    args.emplace_back(argv[i]);
#endif
  try {
    std::locale::global(std::locale::classic());
    return cli(args);
  } catch (const std::exception &e) {
    std::cerr << "KoraLo: " << e.what() << '\n';
    return 1;
  }
}
