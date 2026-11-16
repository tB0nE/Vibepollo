#pragma once
#include <gio/gio.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace clipboard_tools {
inline constexpr std::size_t limit=65536;
// Fixed argv, bounded I/O and a timeout. Clipboard contents never enter a shell.
inline std::optional<std::string> run(const std::vector<std::string> &args, const std::string &input, bool reading) {
  if (args.empty() || input.size()>limit) return std::nullopt;
  std::vector<const char *> argv;for(const auto &arg:args) argv.push_back(arg.c_str());argv.push_back(nullptr);
  GError *error=nullptr;
  auto flags=static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE|G_SUBPROCESS_FLAGS_STDERR_SILENCE|
    (reading?G_SUBPROCESS_FLAGS_STDOUT_PIPE:G_SUBPROCESS_FLAGS_STDOUT_SILENCE));
  GSubprocess *process=g_subprocess_newv(argv.data(),flags,&error);
  if (!process) {g_clear_error(&error);return std::nullopt;}
  GCancellable *cancel=g_cancellable_new();
  std::mutex mutex;std::condition_variable condition;bool done=false;
  std::thread watchdog([&]{std::unique_lock<std::mutex> lock(mutex);
    if (!condition.wait_for(lock,std::chrono::seconds(2),[&]{return done;})) {
      g_cancellable_cancel(cancel);g_subprocess_force_exit(process);
    }
  });
  auto in=g_subprocess_get_stdin_pipe(process);gsize written=0;
  bool ok=g_output_stream_write_all(in,input.data(),input.size(),&written,cancel,&error);
  if (ok) ok=g_output_stream_close(in,cancel,&error);
  std::string output;
  if (reading && ok) {
    auto out=g_subprocess_get_stdout_pipe(process);char buffer[4096];
    while(ok) {
      auto n=g_input_stream_read(out,buffer,sizeof(buffer),cancel,&error);
      if(n<0) {ok=false;break;}if(n==0) break;
      if(output.size()+static_cast<std::size_t>(n)>limit) {ok=false;break;}
      output.append(buffer,static_cast<std::size_t>(n));
    }
  }
  if (!ok) g_subprocess_force_exit(process);
  const bool waited=g_subprocess_wait(process,cancel,ok?&error:nullptr);
  ok=ok && waited && g_subprocess_get_successful(process);
  {std::lock_guard<std::mutex> lock(mutex);done=true;}condition.notify_one();watchdog.join();
  g_clear_error(&error);g_object_unref(cancel);g_object_unref(process);
  return ok?std::optional<std::string>(output):std::nullopt;
}
inline std::string executable(const char *name) {
  std::error_code error;
  auto self=std::filesystem::read_symlink("/proc/self/exe",error);
  auto packaged=self.parent_path()/name;
  if (!error && access(packaged.c_str(),X_OK)==0) return packaged.string();
  gchar *found=g_find_program_in_path(name);std::string path=found?found:"";g_free(found);return path;
}
inline bool wayland() {const char *display=std::getenv("WAYLAND_DISPLAY");return display && *display;}
inline std::optional<std::string> get() {
  auto tool=executable(wayland()?"wl-paste":"xclip");if(tool.empty()) return std::nullopt;
  return wayland()?run({tool,"--no-newline","--type","text"},"",true):run({tool,"-selection","clipboard","-out","-target","UTF8_STRING"},"",true);
}
inline bool set(const std::string &text) {
  auto tool=executable(wayland()?"wl-copy":"xclip");if(tool.empty()) return false;
  return (wayland()?run({tool,"--type","text/plain;charset=utf-8"},text,false):run({tool,"-selection","clipboard","-in","-target","UTF8_STRING"},text,false)).has_value();
}
}
