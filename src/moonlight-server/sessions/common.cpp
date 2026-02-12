#include <immer/array_transient.hpp>
#include <immer/map_transient.hpp>
#include <platforms/hw.hpp>
#include <sessions/common.hpp>
#include <sessions/handlers.hpp>

namespace wolf::core::sessions {

void start_runner(std::shared_ptr<events::Runner> runner,
                  std::shared_ptr<events::devices_atom_queue> plugged_devices_queue,
                  immer::box<RunnerArgs> args) {
  /* Setup devices paths */
  auto all_devices = immer::array_transient<std::string>();

  /* Setup mounted paths */
  immer::array_transient<std::pair<std::string, std::string>> mounted_paths;

  /* Setup environment paths */
  immer::map_transient<std::string, std::string> full_env;
  full_env.set("XDG_RUNTIME_DIR", args->xdg_runtime_dir);
  full_env.set("WOLF_SESSION_ID", args->session_id);

  auto pulse_sink_name = fmt::format("{}{}", VIRTUAL_SINK_PREFIX, args->session_id);
  auto audio_server_name = args->audio_server ? audio::get_server_name(args->audio_server->server) : "";
  // TODO: properly separate XDG_RUNTIME_DIR from <pulse socket path>
  // for example on my dev machine it's ${XDG_RUNTIME_DIR}/pulse/native
  // but we know that on our images it's ${XDG_RUNTIME_DIR}/pulse-socket so this should be fine..
  auto audio_server_on_host = std::filesystem::path(args->host->host_xdg_runtime_dir) /
                              std::filesystem::path(audio_server_name).filename();
  full_env.set("PULSE_SINK", pulse_sink_name);
  full_env.set("PULSE_SOURCE", pulse_sink_name + ".monitor");
  full_env.set("PULSE_SERVER", audio_server_name);
  mounted_paths.push_back({audio_server_on_host, audio_server_name});

  full_env.set("GAMESCOPE_WIDTH", std::to_string(args->video_settings.width));
  full_env.set("GAMESCOPE_HEIGHT", std::to_string(args->video_settings.height));
  full_env.set("GAMESCOPE_REFRESH", std::to_string(args->video_settings.refresh_rate));
  full_env.set("WOLF_VIDEO_BUFFER_CAPS", args->video_settings.video_producer_buffer_caps);

  if (auto w_display = args->wayland_display.get()) {
    auto socket_name = virtual_display::get_wayland_socket_name(*w_display);
    auto local_wayland_socket = std::filesystem::path(args->xdg_runtime_dir) / socket_name;
    auto host_wayland_socket = std::filesystem::path(args->host->host_xdg_runtime_dir) / socket_name;
    mounted_paths.push_back({host_wayland_socket, local_wayland_socket});
    full_env.set("WAYLAND_DISPLAY", socket_name);
  }

  /* Adding custom state folder */
  mounted_paths.push_back({args->app_host_state_folder, "/home/retro"});

  /* GPU specific adjustments */
  auto render_node = args->video_settings.runner_render_node;
  auto additional_devices = linked_devices(render_node);
  std::copy(additional_devices.begin(), additional_devices.end(), std::back_inserter(all_devices));

  auto gpu_vendor = get_vendor(render_node);
  if (gpu_vendor == NVIDIA) {
    if (auto driver_volume = utils::get_env("NVIDIA_DRIVER_VOLUME_NAME")) {
      logs::log(logs::info, "Mounting nvidia driver {}:/usr/nvidia", driver_volume);
      mounted_paths.push_back({driver_volume, "/usr/nvidia"});
    }
  } else if (gpu_vendor == INTEL) {
    full_env.set("INTEL_DEBUG", "norbc"); // see: https://github.com/games-on-whales/wolf/issues/50
  }

  full_env.set("PUID", std::to_string(args->client_settings->run_uid));
  full_env.set("PGID", std::to_string(args->client_settings->run_gid));

  // Mount /dev/uinput so Steam Input can create virtual controllers inside the container
  if (std::filesystem::exists("/dev/uinput")) {
    all_devices.push_back("/dev/uinput");
  }

  // Add fake-udev and udev mounts
  mounted_paths.push_back(
      {std::filesystem::path(args->host->host_base_state_folder) / "fake-udev", "/usr/bin/fake-udev"});
  mounted_paths.push_back({std::filesystem::path(args->app_host_state_folder) / "udev", "/run/udev/"});

  // Mount fake-uinput broker (listens on socket, creates /dev/input/ nodes for Steam Input)
  mounted_paths.push_back(
      {std::filesystem::path(args->host->host_base_state_folder) / "fake-uinput-broker",
       "/usr/bin/fake-uinput-broker"});

  // Mount LD_PRELOAD interceptor libraries at paths that survive Steam's pressure-vessel
  // bubblewrap sandbox (which overlays /usr/lib/ but preserves /home/).
  // Both 64-bit and 32-bit versions are needed because Steam's main process is 32-bit.
  mounted_paths.push_back(
      {std::filesystem::path(args->host->host_base_state_folder) / "libfake-uinput.so",
       "/home/retro/.wolf/libfake-uinput.so"});
  mounted_paths.push_back(
      {std::filesystem::path(args->host->host_base_state_folder) / "libfake-uinput32.so",
       "/home/retro/.wolf/libfake-uinput32.so"});
  full_env.set("LD_PRELOAD", "/home/retro/.wolf/libfake-uinput.so");
  full_env.set("WOLF_BROKER_SOCK", "/home/retro/.wolf/broker.sock");

  // Write /etc/ld.so.preload as a bind-mount so the interceptor library is loaded
  // into EVERY process from the moment the container starts.  This survives
  // pressure-vessel's bubblewrap sandbox (which rewrites the LD_PRELOAD env var
  // but preserves /etc/ld.so.preload and /home/ bind-mounts).
  // Both 64-bit and 32-bit .so paths are listed — the dynamic linker silently
  // skips libraries whose ELF class doesn't match the process.
  {
    auto ld_preload_dir = std::filesystem::path(args->app_host_state_folder) / ".wolf";
    std::filesystem::create_directories(ld_preload_dir);
    auto ld_preload_file = ld_preload_dir / "ld.so.preload";
    std::ofstream f(ld_preload_file);
    f << "/home/retro/.wolf/libfake-uinput.so\n";
    f << "/home/retro/.wolf/libfake-uinput32.so\n";
    f.close();
    mounted_paths.push_back({ld_preload_file.string(), "/etc/ld.so.preload"});
  }

  /* Finally run the app, this will stop here until over */
  runner->run(args->session_id,
              args->app_local_state_folder,
              args->host->host_xdg_runtime_dir,
              plugged_devices_queue,
              all_devices.persistent(),
              mounted_paths.persistent(),
              full_env.persistent(),
              render_node);

  if (args->audio_server && args->audio_sink) {
    logs::log(logs::debug, "[STREAM_SESSION] Remove virtual audio sink");
    audio::delete_virtual_sink(args->audio_server->server, args->audio_sink);
  }
}

} // namespace wolf::core::sessions
