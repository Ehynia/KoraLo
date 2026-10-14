#include "media_probe.hpp"

#include "../platform/process.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <unordered_map>

namespace koralo {
namespace {

double parse_fraction(const std::string& s) {
    const auto pos = s.find('/');
    try {
        if (pos == std::string::npos) return std::stod(s);
        const double a = std::stod(s.substr(0, pos));
        const double b = std::stod(s.substr(pos + 1));
        return b == 0 ? 0 : a / b;
    } catch (...) {
        return 0;
    }
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

std::string dequote(std::string s) {
    s = trim(std::move(s));
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

} // namespace

std::optional<MediaItem> probe_media_file(const std::string& ffprobe_path,
                                          const std::string& path,
                                          std::string* error) {
    if (ffprobe_path.empty()) {
        if (error) *error = "ffprobe is not available";
        return std::nullopt;
    }

    const auto process = run_process(ffprobe_path,
        {"-v", "error", "-show_entries",
         "format=duration:stream=index,codec_type,codec_name,width,height,avg_frame_rate,channels,sample_rate:stream_disposition=attached_pic",
         "-of", "compact=p=0:nk=0", path});
    const std::string& out = process.output;
    if (!process.started || process.exit_code != 0) {
        if (error) *error = out.empty() ? "ffprobe failed" : out;
        return std::nullopt;
    }

    MediaItem m;
    m.path = path;
    m.name = basename_utf8(path);
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty()) continue;
        std::unordered_map<std::string, std::string> fields;
        std::size_t begin = 0;
        while (begin <= line.size()) {
            const std::size_t end = line.find('|', begin);
            const std::string token = line.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            const std::size_t eq = token.find('=');
            if (eq != std::string::npos) fields[token.substr(0, eq)] = dequote(token.substr(eq + 1));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        if (auto it = fields.find("duration"); it != fields.end()) {
            try { m.duration = std::stod(it->second); } catch (...) {}
            continue;
        }
        const auto type_it = fields.find("codec_type");
        if (type_it == fields.end()) continue;
        const std::string& type = type_it->second;
        const auto value = [&](const char* key) -> std::string {
            auto it = fields.find(key);
            return it == fields.end() ? std::string{} : it->second;
        };
        if (type == "video" && !m.has_video && value("disposition:attached_pic") != "1") {
            m.has_video = true;
            m.video_codec = value("codec_name");
            try { m.width = std::stoi(value("width")); } catch (...) {}
            try { m.height = std::stoi(value("height")); } catch (...) {}
            m.fps = parse_fraction(value("avg_frame_rate"));
        } else if (type == "audio" && !m.has_audio) {
            m.has_audio = true;
            m.audio_codec = value("codec_name");
            try { m.audio_channels = std::stoi(value("channels")); } catch (...) {}
            try { m.sample_rate = std::stoi(value("sample_rate")); } catch (...) {}
        }
    }

    if (m.has_video && m.video_codec.empty()) m.video_codec = "video";
    if (m.has_audio && m.audio_codec.empty()) m.audio_codec = "audio";
    if (m.has_video && is_still_image_path(path)) {
        m.has_audio = false;
        if (m.duration <= 0.001) m.duration = 5.0;
        if (m.fps <= 0.0) m.fps = 1.0;
    }

    if (!m.has_video && !m.has_audio) {
        auto key_values = [](const std::string& text) {
            std::unordered_map<std::string, std::string> values;
            std::istringstream input(text);
            std::string row;
            while (std::getline(input, row)) {
                row = trim(row);
                const auto eq = row.find('=');
                if (eq != std::string::npos) values[row.substr(0, eq)] = dequote(row.substr(eq + 1));
            }
            return values;
        };
        const auto video = run_process(ffprobe_path,
            {"-v", "error", "-select_streams", "V:0", "-show_entries",
             "stream=codec_name,width,height,avg_frame_rate", "-of", "default=noprint_wrappers=1", path});
        if (video.started && video.exit_code == 0) {
            auto v = key_values(video.output);
            if (!v.empty()) {
                m.has_video = true;
                m.video_codec = v["codec_name"];
                try { m.width = std::stoi(v["width"]); } catch (...) {}
                try { m.height = std::stoi(v["height"]); } catch (...) {}
                m.fps = parse_fraction(v["avg_frame_rate"]);
            }
        }
        const auto audio = run_process(ffprobe_path,
            {"-v", "error", "-select_streams", "a:0", "-show_entries",
             "stream=codec_name,channels,sample_rate", "-of", "default=noprint_wrappers=1", path});
        if (audio.started && audio.exit_code == 0) {
            auto a = key_values(audio.output);
            if (!a.empty()) {
                m.has_audio = true;
                m.audio_codec = a["codec_name"];
                try { m.audio_channels = std::stoi(a["channels"]); } catch (...) {}
                try { m.sample_rate = std::stoi(a["sample_rate"]); } catch (...) {}
            }
        }
        if (m.duration <= 0.0) {
            const auto duration = run_process(ffprobe_path,
                {"-v", "error", "-show_entries", "format=duration", "-of",
                 "default=noprint_wrappers=1:nokey=1", path});
            if (duration.started && duration.exit_code == 0) {
                try { m.duration = std::stod(trim(duration.output)); } catch (...) {}
            }
        }
    }

    if (m.has_video && m.video_codec.empty()) m.video_codec = "video";
    if (m.has_audio && m.audio_codec.empty()) m.audio_codec = "audio";
    if (m.has_video && is_still_image_path(path)) {
        m.has_audio = false;
        if (m.duration <= 0.001) m.duration = 5.0;
        if (m.fps <= 0.0) m.fps = 1.0;
    }

    std::string ext = path_to_utf8(path_from_utf8(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::set<std::string> audio_only_ext = {
        ".wav", ".mp3", ".flac", ".aac", ".m4a", ".ogg", ".wma", ".opus"};
    if (m.has_audio && audio_only_ext.count(ext)) m.has_video = false;

    if (!m.has_video && !m.has_audio) {
        if (error) *error = "No supported audio/video stream. ffprobe output: " + out.substr(0, 512);
        return std::nullopt;
    }
    return m;
}

} // namespace koralo
