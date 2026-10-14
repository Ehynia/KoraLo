#include "app_paths.hpp"
#include "process.hpp"
#include <atomic>
#include <chrono>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace koralo {
namespace fs = std::filesystem;
TemporaryFile::TemporaryFile(const fs::path &directory,
                             const std::string &suffix) {
  if (directory.empty())
    return;
  static std::atomic<unsigned long long> serial{0};
#ifdef _WIN32
  auto pid = GetCurrentProcessId();
#else
  auto pid = getpid();
#endif
  for (int i = 0; i < 32; ++i) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto candidate = directory / ("koralo_" + std::to_string(pid) + "_" +
                                  std::to_string(stamp) + "_" +
                                  std::to_string(++serial) + suffix);
#ifdef _WIN32
    auto file = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      path_ = candidate;
      return;
    }
#else
    int file =
        open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file >= 0) {
      close(file);
      path_ = candidate;
      return;
    }
#endif
  }
}
TemporaryFile::~TemporaryFile() {
  if (!path_.empty()) {
    std::error_code ec;
    fs::remove(path_, ec);
  }
}
fs::path TemporaryFile::release() { return std::exchange(path_, {}); }
namespace {
bool writable(const fs::path &path) {
  if (path.empty())
    return false;
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec)
    return false;
  TemporaryFile check(path, ".check");
  return check.valid();
}
fs::path state(bool cache) {
  std::vector<fs::path> choices;
  auto explicit_path = path_from_utf8(
      environment_utf8(cache ? "KORALO_CACHE_DIR" : "KORALO_DATA_DIR"));
  if (!explicit_path.empty())
    choices.push_back(explicit_path);
  auto install = app_install_directory();
  std::error_code ec;
  if (!install.empty() && fs::exists(install / "portable.flag", ec))
    choices.push_back(install / (cache ? "cache" : "data"));
#ifdef _WIN32
  auto user =
      path_from_utf8(environment_utf8(cache ? "LOCALAPPDATA" : "APPDATA"));
  if (!user.empty())
    choices.push_back(user / "KoraLo" / (cache ? "Cache" : "Data"));
#else
  auto xdg = path_from_utf8(
      environment_utf8(cache ? "XDG_CACHE_HOME" : "XDG_DATA_HOME"));
  if (!xdg.empty())
    choices.push_back(xdg / "KoraLo");
  auto home = path_from_utf8(environment_utf8("HOME"));
  if (!home.empty())
    choices.push_back(home / (cache ? ".cache" : ".local/share") / "KoraLo");
#endif
  auto temp = fs::temp_directory_path(ec);
  if (!ec)
    choices.push_back(temp / "KoraLo" / (cache ? "cache" : "data"));
  auto cwd = fs::current_path(ec);
  if (!ec)
    choices.push_back(cwd / ".koralo" / (cache ? "cache" : "data"));
  for (const auto &candidate : choices)
    if (writable(candidate))
      return candidate;
  return {};
}
} // namespace
fs::path data_directory() { return state(false); }
fs::path cache_directory() { return state(true); }
fs::path temporary_directory() {
  std::error_code ec;
  auto temp = fs::temp_directory_path(ec);
  if (!ec && writable(temp))
    return temp;
  auto data = data_directory();
  if (!data.empty() && writable(data / "temp"))
    return data / "temp";
  return {};
}
} // namespace koralo
