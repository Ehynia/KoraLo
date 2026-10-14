#include "media/media_engine.hpp"
#include "media/native_effects.hpp"
#include "platform/app_paths.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace koralo;
static void check(bool b, const std::string &message) {
  if (!b)
    throw std::runtime_error(message);
}
static Frame fixture() {
  Frame f;
  f.width = 160;
  f.height = 96;
  f.rgb.resize(160 * 96 * 3);
  for (int y = 0; y < 96; ++y)
    for (int x = 0; x < 160; ++x) {
      bool line = (x > 28 && x < 33) || (y > 45 && y < 49);
      auto i = (y * 160 + x) * 3;
      f.rgb[i] = line ? 45 : 190 + x / 4;
      f.rgb[i + 1] = line ? 55 : 160 + y / 2;
      f.rgb[i + 2] = line ? 65 : 130 + x / 3;
    }
  return f;
}
static Frame preview(MediaEngine &e, PreviewRequest r) {
  static std::uint64_t token = 900;
  r.token = ++token;
  r.allow_prepared_cache = false;
  e.request_preview(r);
  Frame f;
  std::string error;
  std::uint64_t actual = 0;
  for (int i = 0; i < 1000; ++i) {
    if (e.poll_preview(actual, f, error) && actual == token) {
      check(!f.rgb.empty(), "preview: " + error);
      return f;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("preview timeout");
}
static double difference(const Frame &a, const Frame &b) {
  check(a.rgb.size() == b.rgb.size(), "matching dimensions");
  double sum = 0;
  for (std::size_t i = 0; i < a.rgb.size(); ++i)
    sum += std::abs(int(a.rgb[i]) - int(b.rgb[i]));
  return sum / a.rgb.size();
}
int main() {
  try {
    auto input = fixture();
    auto curves = make_effect_instance("koralo.curves", 1);
    Frame f = input;
    apply_native_effect(f, curves, 0);
    check(difference(input, f) < .1, "identity curve");
    set_effect_numeric(curves, "channel", 1);
    set_effect_numeric(curves, "highlights", .95);
    f = input;
    apply_native_effect(f, curves, 0);
    check(difference(f, input) > 1, "curve changes red");
    for (std::size_t i = 0; i < f.rgb.size(); i += 3)
      check(f.rgb[i + 1] == input.rgb[i + 1] &&
                f.rgb[i + 2] == input.rgb[i + 2],
            "curve isolates channel");
    auto mask = make_effect_instance("koralo.layer_mask", 2);
    set_effect_numeric(mask, "feather", 0);
    f = input;
    apply_native_effect(f, mask, 0);
    check(f.alpha.front() == 0 && f.alpha[48 * 160 + 80] == 255,
          "ellipse mask");
    set_effect_numeric(mask, "invert", 1);
    f = input;
    apply_native_effect(f, mask, 0);
    check(f.alpha.front() == 255 && f.alpha[48 * 160 + 80] == 0,
          "inverted mask");
    auto paint = make_effect_instance("koralo.paint_mask", 5);
    effect_parameter(paint, "strokes")->value =
        std::string(".3 .5 .7 .5 .1 1 1\n.5 .5 .5 .5 .04 1 0\n");
    f = input;
    apply_native_effect(f, paint, 0);
    check(f.alpha[48 * 160 + 60] == 0 && f.alpha[48 * 160 + 80] == 255 &&
              f.alpha[10] == 255,
          "continuous stroke and restore");
    auto lines = make_effect_instance("koralo.anime_line_restore", 3);
    set_effect_numeric(lines, "strength", 0);
    f = input;
    apply_native_effect(f, lines, 0);
    check(difference(input, f) < .1, "zero line strength");
    set_effect_numeric(lines, "strength", 1);
    Frame flat = input;
    std::fill(flat.rgb.begin(), flat.rgb.end(), 180);
    f = flat;
    apply_native_effect(f, lines, 0);
    check(difference(f, flat) < .1, "no outlines on flat field");
    auto anime = make_effect_instance("koralo.anime_restore_v45", 4);
    f = input;
    apply_native_effect(f, anime, 0);
    Frame again = input;
    apply_native_effect(again, anime, 10);
    check(f.rgb == again.rgb, "stable texture");
    check(difference(f, input) > 2, "V45 changes fixture");
    Frame tiny;
    tiny.width = tiny.height = 1;
    tiny.rgb = {10, 20, 30};
    check(apply_native_effect(tiny, anime, 0), "one pixel safe padding");
    MediaEngine e;
    if (!e.ready())
      return 77;
    TemporaryFile source(temporary_directory(), ".ppm"),
        output(temporary_directory(), ".mp4"),
        audio(temporary_directory(), ".wav");
    check(source.valid() && output.valid() && audio.valid(),
          "reserve fixtures");
    {
      std::ofstream os(source.path(), std::ios::binary);
      os << "P6\n160 96\n255\n";
      os.write(reinterpret_cast<const char *>(input.rgb.data()),
               input.rgb.size());
    }
    auto generated = run_process(e.ffmpeg_path(),
                                 {"-v", "error", "-y", "-f", "lavfi", "-i",
                                  "sine=frequency=440:sample_rate=48000", "-t",
                                  "1.125", path_to_utf8(audio.path())});
    check(generated.exit_code == 0, "audio fixture");
    Project p;
    p.settings.width = 160;
    p.settings.height = 96;
    p.settings.fps = 24;
    int track = p.add_track(TrackKind::Video, "Video").id;
    MediaItem m;
    m.path = path_to_utf8(source.path());
    m.width = 160;
    m.height = 96;
    m.has_video = true;
    m.duration = 2;
    int mid = p.add_media(m).id;
    p.add_clip(mid, track, 0, 0, 1.125).video_effects = {anime};
    std::string error;
    std::atomic<double> progress{0};
    bool ok =
        e.render_project(p, path_to_utf8(output.path()), &progress, &error);
    check(ok, "native clip export: " + error);
    check(progress == 1, "completed progress");
    PreviewRequest request;
    request.path = m.path;
    request.width = 160;
    request.height = 96;
    request.effects = {anime};
    Frame expected = preview(e, request);
    request.path = path_to_utf8(output.path());
    request.effects.clear();
    request.time = .5;
    Frame actual = preview(e, request);
    double mae = difference(expected, actual);
    std::cout << "clip preview/export MAE=" << mae << "\n";
    check(mae < 4, "Anime preview/export parity");
    auto probe = e.probe(path_to_utf8(output.path()), &error);
    check(probe && std::abs(probe->duration - 1.125) < .06,
          "batch join duration");
    auto am = e.probe(path_to_utf8(audio.path()), &error);
    check(bool(am), "audio probe");
    int aid = p.add_media(*am).id,
        at = p.add_track(TrackKind::Audio, "Audio").id;
    p.add_clip(aid, at, 0, 0, 1.125);
    p.clips.front().video_effects.clear();
    p.media_by_id(mid)->video_effects = {anime};
    p.track_by_id(track)->video_effects = {curves};
    p.video_output_effects = {mask};
    ok = e.render_project(p, path_to_utf8(output.path()), nullptr, &error);
    check(ok, "master export: " + error);
    request.path = m.path;
    request.time = 0;
    request.effects = {anime, curves};
    request.output_effects = {mask};
    expected = preview(e, request);
    request.path = path_to_utf8(output.path());
    request.effects.clear();
    request.output_effects.clear();
    request.time = .5;
    actual = preview(e, request);
    mae = difference(expected, actual);
    std::cout << "master preview/export MAE=" << mae << "\n";
    check(mae < 5, "media track master parity and cache invalidation");
    probe = e.probe(path_to_utf8(output.path()), &error);
    check(probe && probe->has_audio, "master retains audio");
    p.video_output_effects.clear();
    p.clips.front().video_effects = {paint};
    p.media_by_id(mid)->video_effects.clear();
    p.track_by_id(track)->video_effects.clear();
    ok = e.render_project(p, path_to_utf8(output.path()), nullptr, &error);
    check(ok, "paint export: " + error);
    request.path = m.path;
    request.effects = {paint};
    request.time = 0;
    expected = preview(e, request);
    request.path = path_to_utf8(output.path());
    request.effects.clear();
    request.time = .5;
    actual = preview(e, request);
    check(difference(expected, actual) < 5, "paint alpha export parity");
    auto size = std::filesystem::file_size(output.path());
    std::atomic<bool> cancel{true};
    check(!e.render_project(p, path_to_utf8(output.path()), nullptr, &error,
                            nullptr, &cancel),
          "cancel export");
    check(std::filesystem::file_size(output.path()) == size,
          "cancel preserves destination");
    std::cout << "native effects tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
