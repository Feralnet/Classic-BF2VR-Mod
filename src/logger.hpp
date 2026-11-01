#pragma once

#include "shader_patch_version.hpp"

#include <cstdio>
#include <ctime>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#pragma warning(push)
#pragma warning(disable : 4996)

namespace sp {

using namespace std::literals;

// Each run logs to a file of its own in this folder (next to BattlefrontII.exe), named
// by the time the game started; a crash dump of that run gets the same name with .dmp.
// Players attach these to bug reports, so the folder carries a README on how to.
const auto log_folder = "bf2vr_logs"s;
constexpr std::size_t kept_logs = 10;
constexpr std::size_t kept_dumps = 3;

enum class Log_level { info, warning, error };

inline std::ostream& operator<<(std::ostream& stream, const Log_level& level) noexcept
{
   switch (level) {
   case Log_level::info:
      return stream << "[INFO]   "sv;
   case Log_level::warning:
      return stream << "[WARNING]"sv;
   case Log_level::error:
      return stream << "[ERROR]  "sv;
   }

   return stream;
}

class Logger {
public:
   Logger(const Logger&) = delete;
   Logger& operator=(const Logger&) = delete;

   Logger(Logger&&) = delete;
   Logger& operator=(Logger&&) = delete;

   friend auto get_logger() noexcept -> Logger&;
   friend auto get_log_stream() noexcept -> std::ostream&;
   friend auto log_run_path(std::string_view extension) noexcept -> std::string;

private:
   Logger() noexcept
   {
      namespace fs = std::filesystem;

      std::error_code error;
      fs::create_directory(log_folder, error);

      const auto readme = fs::path{log_folder} / "README.txt";

      if (!fs::exists(readme, error)) {
         std::ofstream{readme} << readme_text;
      }

      remove_oldest(".log"sv, kept_logs - 1);
      remove_oldest(".dmp"sv, kept_dumps);

      const auto time = std::time(nullptr);
      std::ostringstream stem;
      stem << log_folder << '/' << std::put_time(std::localtime(&time), "%Y-%m-%d %H-%M-%S");
      _run_stem = stem.str();

      _log_file.open(_run_stem + ".log");
      _log_file << "Shader Patch log started. Shader Patch version is "sv
                << current_shader_patch_version_string << std::endl;
   }

   // Leaves the newest `keep` files with the extension (names sort by time).
   static void remove_oldest(const std::string_view extension, const std::size_t keep) noexcept
   {
      namespace fs = std::filesystem;

      std::vector<fs::path> files;
      std::error_code error;

      for (const auto& entry : fs::directory_iterator{log_folder, error}) {
         if (entry.path().extension() == extension) files.push_back(entry.path());
      }

      if (files.size() <= keep) return;

      std::sort(files.begin(), files.end());

      for (std::size_t i = 0; i < files.size() - keep; ++i) fs::remove(files[i], error);
   }

   static constexpr auto readme_text =
      "Star Wars Battlefront II VR mod: logs\n"
      "=====================================\n"
      "\n"
      "Each start of the game writes one .log file here, named by the date and time\n"
      "the game started (the newest name = the last run). The last 10 are kept.\n"
      "If the game crashed, a .dmp file with the same name is written next to that\n"
      "run's log (the last 3 are kept).\n"
      "\n"
      "Reporting a problem (GitHub issue):\n"
      "1. After the problem, close the game (or let it crash) and attach that run's\n"
      "   .log file before starting the game many more times: only 10 are kept.\n"
      "2. Attach its .dmp file too if there is one (zip it if GitHub refuses it).\n"
      "3. Say the map, what you were (soldier class, hero, vehicle, turret), what\n"
      "   happened and roughly when (every log line has the time).\n"
      "\n"
      "What they contain: the game's and the mod's messages, your VR settings, your\n"
      "Windows version, graphics card, headset runtime and the folder names in\n"
      "GameData\\addon. A .dmp is a snapshot of part of the game's memory at the\n"
      "crash; it can contain file paths (and so your Windows user name).\n"
      "The mod never sends anything anywhere: you choose what to attach.\n";

   std::ofstream _log_file;
   std::string _run_stem;
};

inline auto get_logger() noexcept -> Logger&
{
   static Logger logger;

   return logger;
}

inline auto get_log_stream() noexcept -> std::ostream&
{
   return get_logger()._log_file;
}

// This run's file with another extension (e.g. ".dmp" for its crash dump), relative to
// the game's folder.
inline auto log_run_path(const std::string_view extension) noexcept -> std::string
{
   return get_logger()._run_stem + std::string{extension};
}

template<typename... Args>
inline void log(const Log_level level, Args&&... args) noexcept
{
   auto& stream = get_log_stream();

   const auto time = std::time(nullptr);
   const auto local_time = std::localtime(&time);

   stream << level << ' ' << std::put_time(local_time, "%T") << ' ';
   (stream << ... << args);
   stream << std::endl;
}

template<typename... Args>
inline void log_debug([[maybe_unused]] fmt::format_string<const Args&...> format_str,
                      [[maybe_unused]] const Args&... args) noexcept
{
#ifndef NDEBUG
   auto& stream = get_log_stream();

   const auto time = std::time(nullptr);
   const auto local_time = std::localtime(&time);

   stream << Log_level::info << ' ' << std::put_time(local_time, "%T") << ' ';
   stream << fmt::format(format_str, args...);
   stream << '\n';
#endif
}

template<typename... Args>
inline void log_fmt(const Log_level level, fmt::format_string<const Args&...> format_str,
                    const Args&... args) noexcept
{
   auto& stream = get_log_stream();

   const auto time = std::time(nullptr);
   const auto local_time = std::localtime(&time);

   stream << level << ' ' << std::put_time(local_time, "%T") << ' ';
   stream << fmt::format(format_str, args...);
   stream << std::endl;
}

template<typename... Args>
[[noreturn]] inline void log_and_terminate(Args&&... args)
{
   log(Log_level::error, std::forward<Args>(args)...);

   std::terminate();
}

template<typename... Args>
[[noreturn]] inline void log_and_terminate_fmt(fmt::format_string<const Args&...> format_str,
                                               const Args&... args)
{
   log_fmt(Log_level::error, format_str, args...);

   std::terminate();
}

}

#pragma warning(pop)
