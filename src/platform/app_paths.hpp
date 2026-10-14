#pragma once
#include <filesystem>
#include <string>
namespace koralo {
std::filesystem::path data_directory();
std::filesystem::path cache_directory();
std::filesystem::path temporary_directory();
class TemporaryFile {
public:
  TemporaryFile(const std::filesystem::path &directory,
                const std::string &suffix);
  ~TemporaryFile();
  TemporaryFile(const TemporaryFile &) = delete;
  TemporaryFile &operator=(const TemporaryFile &) = delete;
  bool valid() const { return !path_.empty(); }
  const std::filesystem::path &path() const { return path_; }
  std::filesystem::path release();

private:
  std::filesystem::path path_;
};
} // namespace koralo
