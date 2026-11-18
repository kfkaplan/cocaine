// Path translation between the guest's view of the file system and the host's.
//
// The guest sees one file system made of two: its system directories come
// from the Mac OS X system root, and everything else (home directories,
// temporary files, whatever folder the program was started from) is the
// host's own. The exception inside a home directory is its Library and its
// dot-files, which the guest system keeps a private set of.

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>

#include "kernel/process.h"
#include "session/art.h"

namespace {

// Directories that belong to the guest's Mac OS X rather than to this Mac.
const char* const kSystemDirs[] = {
    "/System", "/usr", "/bin", "/sbin", "/Library", "/etc", "/private/etc",
    "/var/db", "/private/var/db", "/var/root", "/private/var/root",
    // Where system daemons keep their sockets. The guest must not reach this
    // Mac's own daemons, which speak newer protocols.
    "/var/run", "/private/var/run",
    // Per-user temporary and cache folders, which the directory helper
    // daemon makes on request. (One that this Mac already has for the user,
    // such as where TMPDIR points, is used as it is: see host_path.)
    "/var/folders", "/private/var/folders",
    "/var/log", "/private/var/log",
};
// Looked up in the system root first, and on the host if not found there, so
// that both the old system's applications and the user's own are reachable.
const char* const kOverlayDirs[] = {"/Applications"};

bool under(const std::string& path, const char* dir) {
  const size_t n = strlen(dir);
  return path.compare(0, n, dir) == 0 && (path.size() == n || path[n] == '/');
}

std::vector<std::string> split(const std::string& path) {
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < path.size()) {
    const size_t j = path.find('/', i);
    const size_t end = j == std::string::npos ? path.size() : j;
    if (end > i) parts.push_back(path.substr(i, end - i));
    i = end + 1;
  }
  return parts;
}

// The guest's user list is the system root's, which has never heard of the
// person running cocaine. Old programs look themselves up in it (to find a
// home or a temporary folder), so they are shown a copy with one more line.
// Returns the copy's path, or an empty string to use the original.
std::string passwd_with_user(const std::string& original) {
  const uid_t uid = getuid();
  const struct passwd* me = getpwuid(uid);
  if (!me || !me->pw_name || uid == 0) return "";
  char tmp[PATH_MAX];
  if (!confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp)) return "";
  const std::string copy = std::string(tmp) + "cocaine.passwd." + std::to_string(uid);
  struct stat have, from;
  if (stat(original.c_str(), &from) != 0) return "";
  if (stat(copy.c_str(), &have) == 0 && have.st_mtime >= from.st_mtime) return copy;

  std::string text;
  if (FILE* f = fopen(original.c_str(), "r")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
  }
  const std::string key = std::string(me->pw_name) + ":";
  if (text.compare(0, key.size(), key) != 0 && text.find("\n" + key) == std::string::npos) {
    if (!text.empty() && text.back() != '\n') text += '\n';
    text += key + "*:" + std::to_string(uid) + ":" + std::to_string(me->pw_gid) + ":" +
            (me->pw_gecos ? me->pw_gecos : "") + ":" + (me->pw_dir ? me->pw_dir : "/") +
            ":/bin/bash\n";
  }
  // Written under another name and moved into place, so no process ever
  // reads half of it.
  const std::string fresh = copy + "." + std::to_string(getpid());
  FILE* out = fopen(fresh.c_str(), "w");
  if (!out) return "";
  const bool ok = fwrite(text.data(), 1, text.size(), out) == text.size();
  fclose(out);
  if (!ok || rename(fresh.c_str(), copy.c_str()) != 0) {
    unlink(fresh.c_str());
    return "";
  }
  return copy;
}

// The home folder of the person running cocaine, as this Mac has it.
const std::string& real_home() {
  static const std::string home = [] {
    const struct passwd* me = getpwuid(getuid());
    std::string dir = me && me->pw_dir ? me->pw_dir : "";
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir.size() > 1 ? dir : std::string();
  }();
  return home;
}

// A path with its ".", ".." and doubled slashes worked out, without looking
// at the disk.
std::string tidy(const std::string& path) {
  std::vector<std::string> kept;
  for (const std::string& part : split(path)) {
    if (part == ".") continue;
    if (part == "..") {
      if (!kept.empty()) kept.pop_back();
      continue;
    }
    kept.push_back(part);
  }
  std::string out;
  for (const std::string& part : kept) out += "/" + part;
  return out.empty() ? "/" : out;
}

// The part of a home folder the guest system keeps to itself. A home folder
// is this Mac's own, so that an old program can open the documents kept
// there. But its Library is where programs keep their settings, caches and
// keychains, under names that haven't changed in twenty years: an old program
// let loose in the real one reads settings it can't understand and writes
// over the ones a current program of the same name depends on. The same goes
// for the dot-files of shells and tools. So those live in the system root,
// at the same path. Returns where `path` (inside the home folder) really is,
// or an empty string if it is one of the shared parts.
std::string private_home_path(const std::string& root, const std::string& path) {
  const std::string& home = real_home();
  if (home.empty() || path.size() <= home.size() + 1 || !under(path, home.c_str())) return "";
  const std::string rest = path.substr(home.size() + 1);
  const std::string first = rest.substr(0, rest.find('/'));
  if (first != "Library" && !(first.size() > 1 && first[0] == '.')) return "";
  static const bool made = [&] {
    std::string dir = root;
    for (const std::string& part : split(home + "/Library")) mkdir((dir += "/" + part).c_str(), 0755);
    for (const char* sub : {"/Preferences", "/Caches", "/Application Support", "/Logs"}) {
      mkdir((dir + sub).c_str(), 0700);
    }
    return true;
  }();
  (void)made;
  return root + home + "/" + rest;
}

}  // namespace

std::string Process::host_path(const std::string& guest, bool follow_last) const {
  const std::string& root = opt_.sysroot;
  if (root.empty() || guest.empty()) return guest;
  if (guest[0] != '/') {
    // A relative path is this Mac's business, except one that leads into
    // the private part of the home folder from the folder above it.
    if (guest.compare(0, 7, "Library") != 0 && guest[0] != '.') return guest;
    if (guest == "." || guest == ".." || guest.compare(0, 2, "./") == 0 || guest.compare(0, 3, "../") == 0) {
      if (guest.find("Library") == std::string::npos) return guest;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return guest;
    const std::string kept = private_home_path(root, tidy(guest_path(cwd) + "/" + guest));
    return kept.empty() ? guest : kept;
  }

  std::string path = guest;
  if (under(path, real_home().c_str())) {
    const std::string kept = private_home_path(root, tidy(path));
    if (!kept.empty()) return kept;
  }
  // The guest should keep this Mac's time zone. Its own zone files are in the
  // system root, so point its /etc/localtime at the one with the host's name.
  if (path == "/etc/localtime" || path == "/private/etc/localtime") {
    char zone[PATH_MAX];
    const ssize_t n = readlink("/etc/localtime", zone, sizeof zone - 1);
    if (n > 0) {
      zone[n] = 0;
      if (const char* name = strstr(zone, "zoneinfo/")) {
        const std::string in_root = root + "/usr/share/zoneinfo/" + (name + 9);
        if (access(in_root.c_str(), R_OK) == 0) return in_root;
      }
    }
  }
  // Leopard's art file, with cocaine's logo in the menu bar (session/art.cpp).
  static const std::string kArtName = "/SArtFile.bin";
  if (path.size() > kArtName.size() && path.compare(path.size() - kArtName.size(), kArtName.size(), kArtName) == 0) {
    const std::string t = tidy(path);
    if (t == "/System/Library/PrivateFrameworks/CoreUI.framework/Resources/SArtFile.bin" ||
        t == "/System/Library/PrivateFrameworks/CoreUI.framework/Versions/A/Resources/SArtFile.bin") {
      const std::string made = menu_logo_file(root);
      if (access(made.c_str(), R_OK) == 0) return made;
    }
  }
  // Carbon's file code names a file by volume number, folder number and
  // name: "/.vol/V/F/name". This Mac understands such paths too, but would
  // walk its own folders, not the guest's view of them. So the folder is
  // found first (number 2 is the root), turned back into the path the guest
  // knows it by, and the result goes through the usual translation.
  if (path.compare(0, 6, "/.vol/") == 0) {
    const std::vector<std::string> parts = split(path.substr(6));
    if (parts.size() >= 2) {
      std::string folder;
      if (parts[1] == "2") {
        folder = "";
      } else {
        const int fd = open(("/.vol/" + parts[0] + "/" + parts[1]).c_str(), O_RDONLY);
        char real[PATH_MAX];
        const bool found = fd >= 0 && fcntl(fd, F_GETPATH, real) == 0;
        if (fd >= 0) close(fd);
        if (!found) return path;
        folder = guest_path(real);
        if (folder == "/") folder = "";
      }
      for (size_t i = 2; i < parts.size(); i++) folder += "/" + parts[i];
      return host_path(folder.empty() ? "/" : folder, follow_last);
    }
  }
  if (path == "/etc/passwd" || path == "/private/etc/passwd") {
    const std::string copy = passwd_with_user(root + "/private/etc/passwd");
    if (!copy.empty()) return copy;
  }
  for (const char* folders : {"/var/folders", "/private/var/folders"}) {
    if (!under(path, folders)) continue;
    const std::vector<std::string> parts = split(path.substr(strlen(folders)));
    if (parts.size() >= 2 &&
        access(("/private/var/folders/" + parts[0] + "/" + parts[1]).c_str(), F_OK) == 0) {
      return path;
    }
  }
  for (int restarts = 0; restarts < 32; restarts++) {
    bool system = false, overlay = false;
    for (const char* d : kSystemDirs) system |= under(path, d);
    for (const char* d : kOverlayDirs) overlay |= under(path, d);
    if (!system && !overlay) return path;

    // Walk the path inside the system root, following symbolic links the way
    // the guest's own kernel would: an absolute link target means the guest's
    // root, not the host's.
    std::vector<std::string> todo = split(path);
    std::string done;  // resolved so far, guest-absolute; empty means "/"
    bool restarted = false;
    for (size_t i = 0; i < todo.size() && !restarted; i++) {
      const std::string& part = todo[i];
      if (part == ".") continue;
      if (part == "..") {
        const size_t slash = done.rfind('/');
        if (slash != std::string::npos) done.erase(slash);
        continue;
      }
      const std::string next = done + "/" + part;
      const bool last = i + 1 == todo.size();
      struct stat st;
      char target[PATH_MAX];
      ssize_t n;
      if ((!last || follow_last) && lstat((root + next).c_str(), &st) == 0 && S_ISLNK(st.st_mode) &&
          (n = readlink((root + next).c_str(), target, sizeof target - 1)) > 0) {
        target[n] = 0;
        std::string rest;
        for (size_t k = i + 1; k < todo.size(); k++) rest += "/" + todo[k];
        // Splice the target in and start over with the new path.
        path = (target[0] == '/' ? std::string(target) : done + "/" + target) + rest;
        restarted = true;
        break;
      }
      done = next;
    }
    if (restarted) continue;

    const std::string in_root = root + done;
    if (overlay && !system && access(in_root.c_str(), F_OK) != 0) return path;
    return in_root;
  }
  return path;  // a symbolic link loop; let the host report it
}

std::string Process::guest_path(const std::string& host) const {
  const std::string& root = opt_.sysroot;
  if (!root.empty() && host.compare(0, root.size(), root) == 0 &&
      (host.size() == root.size() || host[root.size()] == '/')) {
    return host.size() == root.size() ? "/" : host.substr(root.size());
  }
  return host;
}
