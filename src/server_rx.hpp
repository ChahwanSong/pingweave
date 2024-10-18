#pragma once

#include "common.hpp"

void server_rx() {
    std::cout << "server_rx running (PID: " << getpid() << ")\n";
}
