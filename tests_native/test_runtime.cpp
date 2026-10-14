#include "core/project.hpp"
#include "media/media_engine.hpp"
#include "platform/app_paths.hpp"
#include "platform/process.hpp"
#include "ui/editor_app.hpp"
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace koralo;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
void write(const fs::path &path, const std::string &bytes) {
  std::ofstream f(path, std::ios::binary);
  f << bytes;
  require(static_cast<bool>(f), "write fixture");
}
std::string read(const fs::path &path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void env_set(const char *name, const std::string &value) {
#ifdef _WIN32
  auto key = path_from_utf8(name).native(),
       wide = path_from_utf8(value).native();
  SetEnvironmentVariableW(key.c_str(), value.empty() ? nullptr : wide.c_str());
#else
  if (value.empty())
    unsetenv(name);
  else
    setenv(name, value.c_str(), 1);
#endif
}
struct Env {
  const char *key;
  std::string previous;
  Env(const char *name, const std::string &value)
      : key(name), previous(environment_utf8(name)) {
    env_set(key, value);
  }
  ~Env() { env_set(key, previous); }
};
void process_tests() {
  auto exe = path_to_utf8(current_executable_path());
  require(!exe.empty(), "executable path fallback");
  auto missing = run_process(exe + ".missing", {});
  require(!missing.started, "missing executable must fail at start");
  std::vector<std::string> args{"--args",
                                "spaces and quotes \"\\",
                                "literal $() ; | &",
                                "UTF-8 тест",
                                "",
                                "trailing\\"};
  std::string expected;
  for (std::size_t i = 1; i < args.size(); ++i)
    expected += std::to_string(args[i].size()) + ":" + args[i] + "\n";
  auto result = run_process(exe, args);
  if (!(result.started && result.exit_code == 0 && result.output == expected)) std::cerr << "runtime argv: started=" << result.started << " exit=" << result.exit_code << " got=[" << result.output << "] expected=[" << expected << "]\n";
  require(result.started && result.exit_code == 0 && result.output == expected,
          "argv must preserve quotes, Unicode and empty arguments");
  require(run_process(exe, {"--exit"}).exit_code == 7, "child exit code");
  ProcessOptions options;
  options.timeout_ms = 60;
  auto began = Clock::now();
  result = run_process(exe, {"--sleep"}, options);
  require(result.timed_out &&
              std::chrono::duration<double>(Clock::now() - began).count() < 1,
          "silent process timeout");
  std::atomic<bool> cancel{false};
  options.timeout_ms = 0;
  options.cancel = &cancel;
  std::thread stop([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancel = true;
  });
  result = run_process(exe, {"--sleep"}, options);
  stop.join();
  require(result.cancelled, "silent process cancellation");
  options.cancel = nullptr;
  options.max_output_bytes = 512;
  std::size_t received = 0;
  options.on_output = [&](std::string_view chunk) { received += chunk.size(); };
  result = run_process(exe, {"--flood"}, options);
  require(result.exit_code == 0 && received == 20000 &&
              result.output.size() == 512,
          "bounded capture must still stream all bytes");
  require(!run_process(exe, {std::string("bad\0arg", 7)}).started,
          "embedded NUL rejection");
  ReadPipeProcess pipe;
  require(pipe.start(exe, {"--sleep"}), "open binary pipe");
  char byte{};
  require(!pipe.read_exact(&byte, 1, nullptr, nullptr, 60),
          "binary pipe deadline");
  pipe.terminate();
}
Project project_fixture(const fs::path &media) {
  Project p = Project::make_default();
  MediaItem m;
  m.path = path_to_utf8(media);
  m.has_video = true;
  m.duration = 2;
  m.width = 320;
  m.height = 180;
  m.fps = 24;
  int id = p.add_media(m).id;
  p.add_clip(id, p.tracks.front().id, 0, 0, 2);
  return p;
}
void project_tests(const fs::path &root) {
  auto origin = root / path_from_utf8("исходный проект");
  fs::create_directory(origin);
  auto media = origin / path_from_utf8("видео.mp4");
  write(media, "ORIGINAL-MEDIA");
  auto p = project_fixture(media);
  require(p.save(path_to_utf8(origin / "project.kora")), "atomic project save");
  auto bytes = read(origin / "project.kora");
  require(bytes.rfind("KORALO\t",0) == 0 &&
              bytes.find(path_to_utf8(origin)) == std::string::npos,
          "relative media paths");
  auto moved = root / path_from_utf8("перенесённый проект");
  fs::rename(origin, moved);
  auto loaded = Project::load(path_to_utf8(moved / "project.kora"));
  require(loaded && fs::is_regular_file(path_from_utf8(loaded->media[0].path)),
          "moved project must locate media");
  std::string error;
  auto destination = root / "Collected";
  require(loaded->collect(path_to_utf8(destination), &error),
          "collect project");
  auto copied = Project::load(path_to_utf8(destination / "project.kora"));
  require(copied &&
              read(path_from_utf8(copied->media[0].path)) == "ORIGINAL-MEDIA",
          "collection bytes and paths");
  require(!loaded->collect(path_to_utf8(destination), &error),
          "collect cannot overwrite existing folder");
  loaded->media[0].path = path_to_utf8(root / "missing.mp4");
  require(!loaded->collect(path_to_utf8(root / "Failed"), &error) &&
              !fs::exists(root / "Failed"),
          "failed collection cannot publish partial folder");
  auto blocked = root / "blocked.kora";
  fs::create_directory(blocked);
  write(blocked / "keep", "SAFE");
  require(!p.save(path_to_utf8(blocked)) && read(blocked / "keep") == "SAFE",
          "failed save preserves destination");
  auto malformed = root / "bad.kora";
  for (const auto &text :
       {"", "KORALO\t999\n", "KORALO\t14\n",
        "KORALO\t14\nSETTINGS\t320\t180\tnan\t48000\t1\t1\t0\n",
        "KORALO\t14\nSETTINGS\t320\t180\t24oops\t48000\t1\t1\t0\n",
        "KORALO\t14\nSETTINGS\t320\t180\t24\t48000\t1\t1\t0\nMEDIA\t1\n"}) {
    write(malformed, text);
    error.clear();
    require(!Project::load(path_to_utf8(malformed), &error) && !error.empty(),
            "reject malformed projects");
  }
  p.clips.clear();
  p.tracks.clear();
  p.media[0].id = 1;
  require(p.save(path_to_utf8(root / "trackless.kora")),
          "save trackless project");
  loaded = Project::load(path_to_utf8(root / "trackless.kora"));
  require(loaded && loaded->media.size() == 1,
          "trackless load preserves media");
  FrameCache cache(10, 100);
  Frame frame;
  frame.rgb.resize(48);
  cache.put("one", frame);
  cache.put("two", frame);
  cache.put("three", frame);
  require(!cache.get("one") && cache.get("two") && cache.get("three"),
          "cache byte limit");
  Env directory("KORALO_FFMPEG_DIR", path_to_utf8(root / "missing-runtime"));
  require(MediaEngine::locate_binary("ffmpeg").empty(),
          "explicit runtime is authoritative");
}
struct Services : PlatformServices {
  std::string title, save_path, open_path;
  int prompts = 0;
  SaveChangesChoice choice = SaveChangesChoice::Cancel;
  std::vector<std::string> open_media_files() override { return {}; }
  std::string open_project_file() override { return open_path; }
  std::string save_project_file(const std::string &) override {
    return save_path;
  }
  std::string save_render_file(const std::string &) override { return {}; }
  void show_message(const std::string &, const std::string &,
                    bool = false) override {}
  SaveChangesChoice ask_save_changes(const std::string &) override {
    ++prompts;
    return choice;
  }
  void set_title(const std::string &value) override { title = value; }
  void request_redraw() override {}
  std::string executable_dir() const override {
    return path_to_utf8(app_install_directory());
  }
};
struct MenuCanvas : Canvas {
  struct Label {
    int x, y;
    std::string text;
  };
  std::vector<Label> labels;
  int viewport_width=1280;
  int width() const override { return viewport_width; }
  int height() const override { return 800; }
  void fill_rect(Rect, Color) override {}
  void stroke_rect(Rect, Color, int) override {}
  void line(int, int, int, int, Color, int) override {}
  void text(int x, int y, const std::string &value, Color, int, bool) override {
    labels.push_back({x, y, value});
  }
  int text_width(const std::string &value, int, bool) override {
    return static_cast<int>(value.size()) * 6;
  }
  void image(Rect, const Frame &) override {}
  void push_clip(Rect) override {}
  void pop_clip() override {}
};
void click(EditorApp &editor, MenuCanvas &c, const std::string &label) {
  c.labels.clear();
  editor.draw(c);
  for (const auto &hit : c.labels)
    if (hit.text == label) {
      editor.mouse_down(hit.x + 2, hit.y - 5, 1, false, false, false);
      editor.mouse_up(hit.x + 2, hit.y - 5, 1, false, false, false);
      return;
    }
  throw std::runtime_error("Missing menu item: " + label);
}
void editor_tests(const fs::path &root) {
  Env data("KORALO_DATA_DIR", path_to_utf8(root / "editor-data")),
      lang("KORALO_LANG", "en-US"),
      ffmpeg("KORALO_FFMPEG", path_to_utf8(root / "no-ffmpeg"));
  Services services;
  EditorApp editor(services);
  editor.set_ui_scale(1);
  editor.key_down(Key::M, false, false, false);
  require(services.title.front() == '*', "mark dirty");
  editor.key_down(Key::N, true, false, false);
  require(services.prompts == 1 && services.title.front() == '*',
          "cancel New preserves edits");
  services.choice = SaveChangesChoice::Save;
  editor.key_down(Key::N, true, false, false);
  require(services.title.front() == '*', "cancelled Save As prevents New");
  services.choice = SaveChangesChoice::Cancel;
  services.open_path = path_to_utf8(root / "open.kora");
  require(Project::make_default().save(services.open_path), "open fixture");
  editor.key_down(Key::O, true, false, false);
  require(services.prompts == 3 && services.title.front() == '*',
          "cancel Open preserves edits");
  services.choice = SaveChangesChoice::Save;
  services.save_path = path_to_utf8(root / "saved.kora");
  editor.key_down(Key::N, true, false, false);
  auto saved = Project::load(services.save_path);
  require(saved && !saved->markers.empty() && services.title.front() != '*',
          "Save before New persists content");
  auto recovery = data_directory() / "recovery";
  fs::create_directories(recovery);
  auto fixture = Project::make_default();
  fixture.markers.push_back({1, "Recovered marker"});
  auto valid = recovery / "old.kora";
  require(fixture.save(path_to_utf8(valid)), "recovery fixture");
  fs::last_write_time(valid, fs::file_time_type::clock::now() -
                                 std::chrono::seconds(2));
  write(recovery / "newest.kora", "KORALO\t14\nBROKEN\n");
  MenuCanvas canvas;
  click(editor, canvas, "File");
  click(editor, canvas, "Recover Latest Autosave");
  require(services.title.find("Recovered project") != std::string::npos,
          "valid recovery copy selected");
  services.save_path = path_to_utf8(root / "recovered.kora");
  editor.key_down(Key::S, true, true, false);
  saved = Project::load(services.save_path);
  require(saved && saved->markers[0].label == "Recovered marker" &&
              fs::exists(valid),
          "skip broken newest recovery and preserve original");
  editor.key_down(Key::M, false, false, false);
  services.choice = SaveChangesChoice::Cancel;
  editor.request_close();
  require(!editor.wants_close(), "cancel Close");
  services.choice = SaveChangesChoice::Discard;
  editor.request_close();
  require(editor.wants_close(), "discard Close");
}
void inspector_tests(const fs::path &root) {
  Env data("KORALO_DATA_DIR",path_to_utf8(root/"inspector-data")),lang("KORALO_LANG","en-US"),ffmpeg("KORALO_FFMPEG",path_to_utf8(root/"no-ffmpeg"));
  auto fixture=project_fixture(root/"offline.mp4");fixture.media[0].name="Inspector clip";
  fixture.clips[0].video_effects.push_back(make_effect_instance("koralo.anime_restore_v45",501));
  const auto path=root/"inspector.kora";require(fixture.save(path_to_utf8(path)),"inspector fixture");
  Services services;services.open_path=path_to_utf8(path);EditorApp editor(services);editor.set_ui_scale(1);
  editor.key_down(Key::O,true,false,false);MenuCanvas c;editor.draw(c);
  bool selected=false;
  for(const auto& label:c.labels)if(label.text=="Inspector clip"&&label.y>400&&label.x>260){
    editor.mouse_down(label.x+4,label.y+15,1,false,false,false);editor.mouse_up(label.x+4,label.y+15,1,false,false,false);selected=true;break;
  }
  require(selected,"select real timeline clip in graphite layout");
  c.labels.clear();editor.draw(c);int scale_y=-1;
  for(const auto& label:c.labels)if(label.text=="Scale")scale_y=label.y;
  require(scale_y>0,"selection shows transform inspector");
  bool changed=false;
  for(const auto& label:c.labels)if(label.text=="+"&&std::abs(label.y-scale_y)<3){
    editor.mouse_down(label.x+2,label.y-5,1,false,false,false);editor.mouse_up(label.x+2,label.y-5,1,false,false,false);changed=true;break;
  }
  require(changed,"inspector scale control");editor.key_down(Key::S,true,false,false);
  auto saved=Project::load(path_to_utf8(path));require(saved&&std::abs(sample_motion(saved->clips[0].video_motion,0).zoom-1.05)<1e-6,"inspector keyframe persists");
  editor.key_down(Key::Z,true,false,false);editor.key_down(Key::S,true,false,false);
  saved=Project::load(path_to_utf8(path));require(saved&&std::abs(sample_motion(saved->clips[0].video_motion,0).zoom-1)<1e-6,"inspector Undo restores transform");
  click(editor,c,"Effects");click(editor,c,"ON");editor.key_down(Key::S,true,false,false);
  saved=Project::load(path_to_utf8(path));require(saved&&!saved->clips[0].video_effects[0].enabled,"inspector effect toggle persists");
  editor.key_down(Key::Z,true,false,false);editor.key_down(Key::S,true,false,false);
  saved=Project::load(path_to_utf8(path));require(saved&&saved->clips[0].video_effects[0].enabled,"effect toggle Undo");
  c.viewport_width=960;c.labels.clear();editor.draw(c);
  for(const auto& label:c.labels)require(label.text!="Properties","narrow window collapses inspector");
  click(editor,c,"File");click(editor,c,"Save");
  require(Project::load(path_to_utf8(path)).has_value(),"narrow layout keeps project commands available");
  c.viewport_width=1280;click(editor,c,"Properties");click(editor,c,"Edit effects...");
  for(std::size_t i=1;i<effect_definition("koralo.anime_restore_v45")->parameters.size();++i)click(editor,c,">");
  c.labels.clear();editor.draw(c);bool post_sharpen=false;for(auto& label:c.labels)if(label.text=="Post Sharpen")post_sharpen=true;
  require(post_sharpen,"all Anime parameters are reachable");
  editor.key_down(Key::Escape,false,false,false);
}

void render_tests(const fs::path &root) {
  MediaEngine engine;
  if (!engine.ready()) {
    std::cout << "SKIP runtime render checks: no codecs\n";
    return;
  }
  auto source = root / "source.mp4";
  auto made = run_process(engine.ffmpeg_path(),
                          {"-v", "error", "-f", "lavfi", "-i",
                           "testsrc2=s=320x180:r=24:d=2", "-c:v", "libx264",
                           "-pix_fmt", "yuv420p", path_to_utf8(source)});
  require(made.started && made.exit_code == 0, "generate real media");
  auto p = project_fixture(source);
  p.settings.width = 320;
  p.settings.height = 180;
  p.settings.fps = 24;
  RenderOptions options;
  options.use_gpu = false;
  std::string error;
  auto output = root / "export.mp4";
  write(output, "OLD-EXPORT");
  std::atomic<bool> cancel{true};
  require(!engine.render_project(p, path_to_utf8(output), nullptr, &error,
                                 &options, &cancel) &&
              read(output) == "OLD-EXPORT",
          "pre-cancel preserves old export");
  require(!engine.render_project(p, path_to_utf8(source), nullptr, &error,
                                 &options),
          "export cannot overwrite source");
  auto original = read(source);
  require(original.size() > 1024, "source preserved");
  p.media[0].path = path_to_utf8(root / "missing.mp4");
  require(!engine.render_project(p, path_to_utf8(output), nullptr, &error,
                                 &options) &&
              read(output) == "OLD-EXPORT",
          "missing media fails and preserves old export");
  p.media[0].path = path_to_utf8(source);
  cancel = false;
  p.clips[0].duration = 120;
  p.clips[0].loop = true;
  options.width = 1280;
  options.height = 720;
  auto began = Clock::now();
  auto work = std::async(std::launch::async, [&] {
    return engine.render_project(p, path_to_utf8(output), nullptr, &error,
                                 &options, &cancel);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  cancel = true;
  require(!work.get() &&
              std::chrono::duration<double>(Clock::now() - began).count() < 3 &&
              read(output) == "OLD-EXPORT",
          "actual encoding cancellation must be bounded and atomic");
  p.clips[0].duration = 8;
  options.width = 640;
  options.height = 360;
  cancel = false;
  std::atomic<double> progress{0};
  bool intermediate = false;
  work = std::async(std::launch::async, [&] {
    return engine.render_project(p, path_to_utf8(output), &progress, &error,
                                 &options, &cancel);
  });
  while (work.wait_for(std::chrono::milliseconds(10)) !=
         std::future_status::ready) {
    auto value = progress.load();
    intermediate |= value > 0.06 && value < 0.99;
  }
  require(work.get() && progress == 1.0 && intermediate,
          "real export progress and completion");
  auto decoded = run_process(engine.ffmpeg_path(),
                             {"-v", "error", "-xerror", "-i",
                              path_to_utf8(output), "-f", "null", "-"});
  require(decoded.exit_code == 0, "fully decode exported file");
}
int runtime_main(int argc, char **argv) {
  initialize_executable_path(argv[0]);
  if (argc > 1) {
    auto command = std::string(argv[1]);
    if (command == "--args") {
      for (int i = 2; i < argc; ++i)
        std::cout << std::string(argv[i]).size() << ":" << argv[i] << '\n';
      return 0;
    }
    if (command == "--exit")
      return 7;
    if (command == "--sleep") {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      return 0;
    }
    if (command == "--flood") {
      std::cout << std::string(20000, 'x');
      return 0;
    }
  }
  TemporaryFile reserve(temporary_directory(), ".tests");
  require(reserve.valid(), "reserve test directory");
  auto root = reserve.release();
  fs::remove(root);
  fs::create_directory(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  try {
    process_tests();
    project_tests(root);
    editor_tests(root);
    inspector_tests(root);
    render_tests(root);
    std::cout << "KoraLo runtime regression tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Runtime regression failure: " << e.what() << '\n';
    return 1;
  }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> strings;
  for (int i = 0; i < argc; ++i) {
    int needed = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    if (needed > 1) {
      std::string s(needed, '\0');
      WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), needed, nullptr, nullptr);
      s.pop_back();
      strings.push_back(s);
    } else {
      strings.push_back({});
    }
  }
  std::vector<char *> args;
  for (auto &value : strings)
    args.push_back(value.data());
  return runtime_main(argc, args.data());
}
#else
int main(int argc, char **argv) { return runtime_main(argc, argv); }
#endif
