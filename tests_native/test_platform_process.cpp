#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "platform/process.hpp"
#include "platform/utf8_fs.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace koralo;

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static int child_main(const std::vector<std::string> &args) {
  if (args.size() == 2 && args[0] == "--echo-arg") {
    std::cout << args[1] << '\n';
    return 0;
  }
  if (args.size() == 1 && args[0] == "--echo-stdin") {
    std::string input;
    std::getline(std::cin, input);
    std::cout << input << '\n';
    return 0;
  }
  if (args.size() == 1 && args[0] == "--quiet") return 0;
  if (args.size() == 1 && args[0] == "--fail") return 23;
  if (args.size() == 1 && args[0] == "--sleep") {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    return 0;
  }
  return 2;
}

static int parent_main() {
  const std::string self = path_to_utf8(current_executable_path());
  require(!self.empty(), "find test executable");
  const std::string message = "arg with spaces \\\" and UTF-8: \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";
  const auto args = run_process(self, {"--echo-arg", message});
  if (!(args.started && args.exit_code == 0 && args.output == message + "\n")) std::cerr << "process args: started=" << args.started << " exit=" << args.exit_code << " bytes=" << args.output.size() << " got=[" << args.output << "] expected=[" << message << "]\n";
  require(args.started && args.exit_code == 0 && args.output == message + "\n",
          "Unicode/quoted command-line arguments and stdout");

  ProcessOptions input;
  input.input = "stdin UTF-8: \xd0\xbc\xd1\x83\xd0\xb7\xd1\x8b\xd0\xba\xd0\xb0\n";
  auto piped = run_process(self, {"--echo-stdin"}, input);
  require(piped.started && piped.exit_code == 0 && piped.output == input.input,
          "child stdin/stdout and inherited standard handles");

  ReadPipeProcess encoder;
  std::string error;
  require(encoder.start(self, {"--echo-stdin"}, &error, false, true),
          "streaming encoder opens stdin pipe");
  require(encoder.write_stdin_and_close("frame bytes\n", &error),
          "streaming encoder writes stdin");
  char readback[64]{};
  require(encoder.read_some(readback, sizeof(readback), 5000, nullptr, &error) > 0,
          "streaming encoder returns stdout");
  encoder.terminate();

  auto silent = run_process(self, {"--quiet"});
  require(silent.started && silent.exit_code == 0, "quiet process with NUL std handles");
  auto failure = run_process(self, {"--fail"});
  require(failure.started && failure.exit_code == 23, "child exit code");
  ProcessOptions timeout;
  timeout.timeout_ms = 150;
  auto delayed = run_process(self, {"--sleep"}, timeout);
  require(delayed.started && delayed.timed_out, "timed-out child is killed");

  const fs::path directory = fs::temp_directory_path() /
      utf8_fs::from_utf8("koralo-\xd1\x82\xd0\xb5\xd1\x81\xd1\x82-unicode");
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{directory};
  std::error_code ec;
  fs::create_directories(directory, ec);
  require(!ec, "create UTF-8 directory");
  const auto unicode_exe = directory / current_executable_path().filename();
  fs::copy_file(path_from_utf8(self), unicode_exe, fs::copy_options::overwrite_existing, ec);
  require(!ec, "copy executable into Unicode directory");
  const auto unicode = run_process(path_to_utf8(unicode_exe), {"--echo-arg", message});
  require(unicode.started && unicode.exit_code == 0 && unicode.output == message + "\n",
          "CreateProcessW runs executable from Unicode directory");
  std::cout << "PASS platform process (Unicode, pipes, NUL handles, exit, timeout)\n";
  return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    int needed = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    if (needed > 1) {
      std::string s(needed, '\0');
      WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), needed, nullptr, nullptr);
      s.pop_back();
      args.push_back(s);
    } else {
      args.push_back({});
    }
  }
#else
int main(int argc, char **argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
  try { return args.empty() ? parent_main() : child_main(args); }
  catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
