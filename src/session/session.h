#pragma once

#include "common.h"

// Runs the session server (see session.cpp) for the session called `name`,
// whose first process is `root_pid`, until that process ends. `powerpc` says
// which kind of CPU the session's programs run on, and `windows` that its
// first program is one that will want the window server. Returns an exit
// status.
int run_session_server(const std::string& self_path, const std::string& sysroot, int root_pid,
                       const std::string& name, bool powerpc, bool windows);
