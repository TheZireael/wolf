/**
 * fake-uinput: LD_PRELOAD library that intercepts uinput ioctl() calls
 * and delegates device node creation/removal to a broker daemon over
 * a Unix socket, instead of calling mknod() directly (which requires
 * CAP_MKNOD inside containers).
 *
 * This provides 100% container isolation: only ioctls from THIS process
 * (and its children) are intercepted.  No sysfs scanning, no race
 * conditions, no cross-container device leakage.
 *
 * The broker socket path is read from the WOLF_BROKER_SOCK env var,
 * defaulting to /home/retro/.wolf/broker.sock.
 *
 * Protocol with broker:
 *   Send: MKNOD <sysfs_path>\n        Recv: OK <devname>\n | ERR <msg>\n
 *   Send: REMOVE <sysfs_path>\n       Recv: OK\n          | ERR <msg>\n
 */

#include <cstdarg>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <iostream>
#include <linux/uinput.h>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// Must match the path the broker actually listens on (set in docker.cpp's exec
// command).  /home/retro/.wolf/ survives pressure-vessel's bubblewrap sandbox
// because /home/ is always bind-mounted.  We do NOT rely on the WOLF_BROKER_SOCK
// env var reaching us — pressure-vessel strips custom env vars.
static constexpr const char *DEFAULT_BROKER_SOCK = "/home/retro/.wolf/broker.sock";
static constexpr const char *TAG = "[fake-uinput]";
static constexpr int MAX_BACKOFF_MS = 5000;
static constexpr int INITIAL_BACKOFF_MS = 10;

using ioctl_fn = int (*)(int, unsigned long, ...);
using close_fn = int (*)(int);
static ioctl_fn real_ioctl = nullptr;
static close_fn real_close = nullptr; // used internally for socket cleanup

static std::mutex g_mutex;
static std::unordered_map<int, std::vector<std::string>> g_tracked;

static void resolve_symbols() {
  if (!real_ioctl)
    real_ioctl = reinterpret_cast<ioctl_fn>(dlsym(RTLD_NEXT, "ioctl"));
  if (!real_close)
    real_close = reinterpret_cast<close_fn>(dlsym(RTLD_NEXT, "close"));
}

static const char *get_broker_sock() {
  static const char *path = nullptr;
  if (!path) {
    path = getenv("WOLF_BROKER_SOCK");
    if (!path || path[0] == '\0')
      path = DEFAULT_BROKER_SOCK;
  }
  return path;
}

/** Connect to the broker with exponential backoff. Returns fd or -1. */
static int broker_connect() {
  resolve_symbols();
  const char *sock_path = get_broker_sock();
  int total_ms = 0, backoff_ms = INITIAL_BACKOFF_MS;
  while (total_ms < MAX_BACKOFF_MS) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
      return -1;

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(sock, reinterpret_cast<struct sockaddr *>(&addr),
                sizeof(addr)) == 0)
      return sock;
    real_close(sock);

    usleep(static_cast<useconds_t>(backoff_ms) * 1000);
    total_ms += backoff_ms;
    backoff_ms *= 2;
  }
  return -1;
}

/** Send a command to the broker. Returns true if broker replied "OK...". */
static bool broker_request(const std::string &command) {
  int sock = broker_connect();
  if (sock < 0) {
    std::cout << TAG << " broker unreachable, skipping: "
              << command.substr(0, command.size() - 1) << std::endl;
    return false;
  }

  ssize_t sent = write(sock, command.data(), command.size());
  if (sent < 0) {
    std::cout << TAG << " send failed: " << strerror(errno) << std::endl;
    real_close(sock);
    return false;
  }

  char buf[256] = {};
  ssize_t n = read(sock, buf, sizeof(buf) - 1);
  real_close(sock);

  if (n <= 0) {
    std::cout << TAG << " no response from broker" << std::endl;
    return false;
  }

  std::string resp(buf, static_cast<size_t>(n));
  if (!resp.empty() && resp.back() == '\n')
    resp.pop_back();

  bool ok = resp.rfind("OK", 0) == 0;
  if (!ok)
    std::cout << TAG << " broker error: " << resp << std::endl;
  return ok;
}

/** Send REMOVE for a list of tracked sysfs paths. */
static void send_removes(const std::vector<std::string> &paths) {
  for (const auto &p : paths) {
    std::cout << TAG << " requesting: REMOVE " << p << std::endl;
    broker_request("REMOVE " + p + "\n");
  }
}

/**
 * After UI_DEV_CREATE succeeds, discover event/js/mouse nodes under the sysfs
 * input directory and ask the broker to create device nodes for each.
 */
static void handle_dev_create(int fd) {
  resolve_symbols();

  char sysname[64] = {};
  if (real_ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
    std::cout << TAG << " UI_GET_SYSNAME failed: " << strerror(errno)
              << std::endl;
    return;
  }

  std::string sysdir = std::string("/sys/devices/virtual/input/") + sysname;
  struct stat dir_st;
  if (stat(sysdir.c_str(), &dir_st) != 0 || !S_ISDIR(dir_st.st_mode)) {
    std::cout << TAG << " sysfs dir not found: " << sysdir << std::endl;
    return;
  }

  DIR *dp = opendir(sysdir.c_str());
  if (!dp) {
    std::cout << TAG << " opendir failed: " << sysdir << std::endl;
    return;
  }

  std::vector<std::string> paths;
  struct dirent *ent;
  while ((ent = readdir(dp)) != nullptr) {
    std::string name = ent->d_name;
    if (name == "." || name == "..")
      continue;
    // Only interested in event*, js*, mouse* sub-directories
    if (name.rfind("event", 0) != 0 && name.rfind("js", 0) != 0 &&
        name.rfind("mouse", 0) != 0)
      continue;
    std::string subpath = sysdir + "/" + name;
    struct stat sub_st;
    if (stat(subpath.c_str(), &sub_st) != 0 || !S_ISDIR(sub_st.st_mode))
      continue;

    std::cout << TAG << " requesting: MKNOD " << subpath << std::endl;
    broker_request("MKNOD " + subpath + "\n");
    paths.push_back(subpath);
  }
  closedir(dp);

  if (!paths.empty()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tracked[fd] = std::move(paths);
  }
}

/** Send REMOVE for all tracked paths on a given fd, then erase tracking. */
static void handle_dev_destroy(int fd) {
  std::vector<std::string> paths;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_tracked.find(fd);
    if (it == g_tracked.end())
      return;
    paths = std::move(it->second);
    g_tracked.erase(it);
  }
  send_removes(paths);
}

// -- Intercepted libc functions -----------------------------------------------

extern "C" int ioctl(int fd, unsigned long request, ...) {
  resolve_symbols();

  va_list args;
  va_start(args, request);
  void *arg = va_arg(args, void *);
  va_end(args);

  int result = real_ioctl(fd, request, arg);

  if (result >= 0) {
    if (request == UI_DEV_CREATE)
      handle_dev_create(fd);
    else if (request == UI_DEV_DESTROY)
      handle_dev_destroy(fd);
  }
  return result;
}

// Note: we intentionally do NOT intercept close().  Steam forks after creating
// the uinput device — the parent closes its copy of the fd, but the kernel
// device stays alive because the child still holds the fd.  If we sent REMOVE
// on close(), we'd delete the device nodes while the device is still active.
// Cleanup happens via explicit UI_DEV_DESTROY, or when the container exits.
