#pragma once

namespace tavern::server {

// Runs the IPv4 TCP server until SIGINT or SIGTERM requests shutdown.
// All protocol commands execute serially on this event-loop thread.
int run(int argc, char** argv);

}  // namespace tavern::server
