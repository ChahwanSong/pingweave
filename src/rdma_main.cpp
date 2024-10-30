#include <functional>

#include "rdma_client.hpp"
#include "rdma_common.hpp"
#include "rdma_server.hpp"

pid_t start_process(std::function<void()> func, const char* name);
void signal_handler(int sig);

// Structure to hold process information
struct ProcessInfo {
    pid_t pid;
    std::function<void()> func;
    const char* name;
};

// Global variables
std::vector<ProcessInfo> processes;
bool running = true;

int main() {
    spdlog::info("--- Main thread starts");

    // Register signal handlers in the parent process only
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // get ip addresses of this node
    const std::string config_path =
        get_source_directory() + "/../config/pinglist.yaml";
    std::set<std::string> myaddr;
    get_my_addr(config_path, myaddr);

    // run server/client processes in parallel
    for (auto addr : myaddr) {
        processes.push_back(
            {start_process([&] { rdma_server(addr); }, "rdma_server"),
             std::bind(rdma_server, addr), "rdma_server"});
        processes.push_back(
            {start_process([&] { rdma_client(addr); }, "rdma_client"),
             std::bind(rdma_client, addr), "rdma_client"});
    }

    // Monitor processes for every second and restart them if they exit
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        for (int i = 0; i < processes.size(); ++i) {
            int status;
            pid_t result = waitpid(processes[i].pid, &status, WNOHANG);
            if (result == 0) {
                // Process is still running
            } else if (result == processes[i].pid) {
                // Process has terminated; restart it
                spdlog::info("{} (PID {}) has terminated", processes[i].name,
                             processes[i].pid);
                processes[i].pid =
                    start_process(processes[i].func, processes[i].name);
            } else {
                // Error handling
                spdlog::error("Error with waitpid for {}", processes[i].name);
            }
        }
    }

    return 0;
}

// Function to start a new process running the given function
pid_t start_process(std::function<void()> func, const char* name) {
    pid_t pid = fork();
    if (pid < 0) {
        exit(1);
    } else if (pid == 0) {
        // Child process: reset signal handlers to default
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);  // Reset SIGCHLD if necessary

        // Child process: execute the function
        spdlog::info("{} - Child Process (PID: {}) started.", name, getpid());
        func();
        exit(0);  // Should not reach here
    } else {
        // Parent process: return child's PID
        return pid;
    }
}

// Signal handler to terminate all child processes
void signal_handler(int sig) {
    running = false;
    spdlog::critical("*** Main thread exits. ***");
    for (int i = 0; i < processes.size(); ++i) {
        kill(processes[i].pid, SIGTERM);
    }
    exit(0);
}

/**
 * TODO:
 *
 * 10초에 한번씩
 * - pinglist 파일을 읽고, 해당 노드에 있는 ip 주소를 가져온다
 * - 해당 ip 주소에 해당하지 않는 스레드세트를 종료시킨다 (e.g., pinglist 에서
 * 제외된 경우)
 * - pinglist 에 있지만 실행되고 있지 않는 ip 주소에 대해 스레드세트 새롭게 실행
 * (e.g., new IP)
 * - 실행해야 되는 ip 주소들 중 종료된 스레드가 있다면 재실행
 */