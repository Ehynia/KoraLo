#include "core/project.hpp"
#include "core/undo.hpp"
#include "media/media_engine.hpp"
#include "media/paint_mask.hpp"
#include "media/vector_renderer.hpp"
#include "platform/app_paths.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace koralo;
namespace {
void require(bool v, const char *text) {
  if (!v)
    throw std::runtime_error(text);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-5; }
Frame wait_preview(MediaEngine &e, PreviewRequest req) {
  const auto wanted = req.token;
  e.request_preview(std::move(req));
  for (int i = 0; i < 400; ++i) {
    std::uint64_t token = 0;
    Frame f;
    std::string error;
    if (e.poll_preview(token, f, error) && token == wanted) {
      require(error.empty(), error.c_str());
      require(!f.rgb.empty(), "preview frame exists");
      return f;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("preview timeout");
}
double mae(const Frame &a, const Frame &b) {
  require(a.rgb.size() == b.rgb.size(), "frame dimensions");
  double sum = 0;
  for (std::size_t i = 0; i < a.rgb.size(); ++i)
    sum += std::abs(int(a.rgb[i]) - int(b.rgb[i]));
  return sum / a.rgb.size();
}
} // namespace
int main() {
  try {
    AnimatedDouble channel{0, {}};
    channel.set_key(0, 0, KeyframeInterpolation::Smooth);
    channel.set_key(1, 1);
    require(channel.value_at(.2) < .2, "smooth ease-in");
    require(channel.value_at(.8) > .8, "smooth ease-out");
    channel.keys[0].x1 = .12;
    channel.keys[0].y1 = .8;
    channel.keys[0].x2 = .55;
    channel.keys[0].y2 = .9;
    require(channel.value_at(.2) > .4, "Bezier handles affect evaluation");
    channel.keys[0].interpolation = KeyframeInterpolation::Hold;
    require(near(channel.value_at(.99), 0) && near(channel.value_at(1), 1),
            "hold switches at exact key");
    VectorScene scene;
    scene.enabled = true;
    scene.next_shape_id = 2;
    auto rect = make_vector_rectangle(1);
    rect.x.set_key(0, .25, KeyframeInterpolation::Smooth);
    rect.x.set_key(1, .75);
    rect.fill = {1, .2, .3, .7};
    rect.stroke_width = .004;
    rect.name = "Контур \"А\"";
    rect.rotation.default_value = 37;
    const VectorPoint anchor{-.18, -.14};
    const auto rotated = transform_vector_point(rect, anchor, .4, 16.0 / 9);
    const auto unrotated = inverse_vector_point(rect, rotated, .4, 16.0 / 9);
    require(near(anchor.x, unrotated.x) && near(anchor.y, unrotated.y),
            "rotated node picking uses pixel aspect consistently");
    rect.rotation.default_value = 0;
    scene.shapes.push_back(rect);
    scene.crop_enabled = true;
    scene.crop_x.default_value = .1;
    scene.crop_w.default_value = .8;
    scene.mask_strokes =
        encode_brush_stroke({.48, .48, .52, .52, .04, .75, true});
    scene.mask_amount.set_key(0, .2);
    scene.mask_amount.set_key(1, .8);
    VectorScene decoded;
    require(decode_vector_scene(encode_vector_scene(scene), decoded),
            "scene decode");
    require(encode_vector_scene(scene) == encode_vector_scene(decoded),
            "scene round trip");
    const auto before = encode_vector_scene(decoded);
    require(!decode_vector_scene("V1 nonsense", decoded) &&
                encode_vector_scene(decoded) == before,
            "invalid record is transactional");
    auto f0 = render_vector_scene(scene, 320, 180, 0),
         f1 = render_vector_scene(scene, 320, 180, 1);
    require(mae(f0, f1) > 8, "animated vector pixels move");
    require(f0.alpha[90 * 320 + 80] > 0 && f0.alpha[90 * 320 + 240] == 0,
            "transparent shape positioning");
    require(f0.alpha[90 * 320 + 5] == 0, "nondestructive crop");
    PaintMaskRaster incremental;
    std::string strokes;
    for (int i = 0; i < 60; ++i) {
      const double x = .1 + (i % 20) * .04, y = .2 + (i / 20) * .2;
      strokes +=
          encode_brush_stroke({x, y, x + .035, y + .02, .035, .65, i % 7 != 0});
      incremental.update(strokes, 320, 180);
      PaintMaskRaster reference;
      reference.update(strokes, 320, 180);
      require(incremental.coverage() == reference.coverage(),
              "incremental mask equals fresh replay including restore");
    }
    require(incremental.segments_rasterized() == 60 &&
                incremental.rebuilds() == 1,
            "one raster pass per appended segment");
    incremental.update(strokes, 320, 180);
    require(incremental.segments_rasterized() == 60,
            "unchanged mask has no stroke work");
    strokes.erase(strokes.rfind('\n', strokes.size() - 2) + 1);
    incremental.update(strokes, 320, 180);
    PaintMaskRaster undo_reference;
    undo_reference.update(strokes, 320, 180);
    require(incremental.coverage() == undo_reference.coverage() &&
                incremental.rebuilds() == 2,
            "undo rebuilds safely");
    incremental.update(strokes, 640, 360);
    PaintMaskRaster resized;
    resized.update(strokes, 640, 360);
    require(incremental.coverage() == resized.coverage(),
            "resolution change rebuilds safely");
    Project p = Project::make_default();
    p.settings.width = 320;
    p.settings.height = 180;
    p.settings.fps = 20;
    MediaItem m;
    m.name = "Vectors";
    m.path = "generated:vector";
    m.has_video = true;
    m.duration = 1;
    m.width = 320;
    m.height = 180;
    m.fps = 20;
    m.generated_vector = scene;
    const int mid = p.add_media(m).id;
    int track = 0;
    for (const auto &t : p.tracks)
      if (t.kind == TrackKind::Video) {
        track = t.id;
        break;
      }
    p.add_clip(mid, track, 0, 0, 1);
    UndoStack undo;
    undo.begin(p);
    p.media_by_id(mid)->generated_vector.shapes[0].x.keys[0].value = .35;
    require(undo.commit(p) && undo.undo(p), "vector edit undo");
    require(
        near(p.media_by_id(mid)->generated_vector.shapes[0].x.value_at(0), .25),
        "undo restores scene");
    require(undo.redo(p), "vector redo");
    undo.undo(p);
    TemporaryFile file(temporary_directory(), ".kora");
    require(file.valid() && p.save(path_to_utf8(file.path())),
            "save vector project");
    std::string error;
    auto loaded = Project::load(path_to_utf8(file.path()), &error);
    require(loaded && encode_vector_scene(
                          loaded->media_by_id(mid)->generated_vector) ==
                          encode_vector_scene(scene),
            "project persists vectors, easing, mask and crop");
    require(loaded->media_by_id(mid)->path == "generated:vector",
            "generated path stays symbolic");
    {
      std::ifstream input(file.path());
      std::string bytes((std::istreambuf_iterator<char>(input)), {});
      std::ofstream damaged(file.path(), std::ios::app);
      damaged << "VANIM\t999999\t" << encode_vector_scene(scene) << '\n';
      damaged.close();
      require(!Project::load(path_to_utf8(file.path()), &error),
              "orphan vector records fail explicitly");
      std::ofstream restore(file.path(), std::ios::trunc);
      restore << bytes;
    }
    MediaEngine engine;
    PreviewRequest req;
    req.width = 320;
    req.height = 180;
    req.vector_source = scene;
    req.time = .4;
    req.token = 100;
    auto preview = wait_preview(engine, req);
    require(!preview.rgb.empty(), "vector preview without source file");
    if (engine.ready()) {
      TemporaryFile output(temporary_directory(), ".mp4");
      RenderOptions options;
      options.use_gpu = false;
      options.video_bitrate_kbps = 12000;
      require(engine.render_project(p, path_to_utf8(output.path()), nullptr,
                                    &error, &options),
              error.c_str());
      auto probe = engine.probe(path_to_utf8(output.path()), &error);
      require(probe && probe->width == 320 && probe->height == 180 &&
                  std::abs(probe->duration - 1) < .1,
              "vector export metadata");
      PreviewRequest decoded_req;
      decoded_req.path = path_to_utf8(output.path());
      decoded_req.time = .4;
      decoded_req.width = 320;
      decoded_req.height = 180;
      decoded_req.token = 101;
      decoded_req.allow_prepared_cache = false;
      auto exported = wait_preview(engine, decoded_req);
      require(mae(preview, exported) < 4, "preview/export vector parity");
      // The vector foreground and video background may have different rates.
      PreviewRequest composite;
      composite.vector_source.enabled = true;
      composite.width = 320;
      composite.height = 180;
      composite.has_background = true;
      composite.background_path = path_to_utf8(output.path());
      composite.background_allow_prepared_cache = false;
      composite.background_source_rate = 2;
      composite.requested_fps = 20;
      composite.continuous = true;
      for (int i = 0; i < 5; ++i) {
        composite.time = .1 + i * .05;
        composite.background_time = .2 + i * .1;
        composite.token = 200 + i * 2;
        auto streamed = wait_preview(engine, composite);
        decoded_req.time = composite.background_time;
        decoded_req.token = composite.token + 1;
        require(mae(streamed, wait_preview(engine, decoded_req)) < 4,
                "streamed vector/video composite respects background rate");
      }
      // Split/trim sampling uses source-media time, so split right events
      // keep the same animation rather than restarting at key zero.
      const int cid = p.clips[0].id;
      int right = 0;
      require(p.split_clip(cid, .5, &right), "split generated animation");
      require(near(p.clip_by_id(right)->source_offset(), .5),
              "split advances animation source time");
    }
    std::cout << "PASS: vector model, Bezier easing, alpha/crop, incremental "
                 "mask, undo, save/load, preview/export\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
