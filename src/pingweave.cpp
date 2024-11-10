#include <functional>

#include "rdma_client.hpp"
#include "rdma_server.hpp"

const std::string pinglist_rel_path = DIR_DOWNLOAD_PATH + "pinglist.yaml";
const uint32_t main_check_period_seconds = 10;

pid_t start_process(std::function<void()> func, const char* name);
void signal_handler(int sig);

// Structure to hold process information
struct ProcessInfo {
    pid_t pid;
    std::function<void()> func;
    const char* name;
    std::string host;
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
    const std::string pinglist_abs_path =
        get_source_directory() + pinglist_rel_path;

    // Monitor processes for every second and restart them if they exit
    while (running) {
        /* 1. Load my IP addresses periodically (pinglist_abs_path) */
        std::set<std::string> myaddr;
        get_my_addr(pinglist_abs_path, myaddr);

        // if loading pinglist is failed, myaddr will be empty.
        spdlog::debug("Myaddr size: {}", myaddr.size());

        /* 2. Check and restart if threads are exited */
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

        /* 3. Terminate threads which are not in pinglist */
        std::set<std::string> running;
        for (auto it = processes.begin(); it != processes.end();) {
            if (myaddr.find(it->host) == myaddr.end()) {
                spdlog::info(
                    "IP {} is not in pinglist anymore. Exit the thread.",
                    it->host);
                int result = kill(it->pid, SIGTERM);
                if (result != 0) {
                    spdlog::error("Faile dto send signal to PID {}: {}",
                                  it->pid, strerror(errno));
                }
                it = processes.erase(it);
            } else {
                // memorize the running threads
                running.insert(it->host);
                ++it;
            }
        }

        /* 4. Start new threads which are added to pinglist */
        for (auto it = myaddr.begin(); it != myaddr.end(); ++it) {
            if (running.find(*it) == running.end()) {
                spdlog::info("IP {} is new. Run the threads.", *it);
                processes.push_back(
                    {start_process([&] { rdma_server(*it); }, "rdma_server"),
                     std::bind(rdma_server, *it), "rdma_server", *it});
                processes.push_back(
                    {start_process([&] { rdma_client(*it); }, "rdma_client"),
                     std::bind(rdma_client, *it), "rdma_client", *it});
            }
        }

        // sleep for a while to save CPU resource
        std::this_thread::sleep_for(
            std::chrono::seconds(main_check_period_seconds));
    }

    return 0;
}

// Function to start a new process running the given function
pid_t start_process(std::function<void()> func, const char* name) {
    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("Failed to fork process for {}: {}", name,
                      strerror(errno));
        exit(EXIT_FAILURE);
    } else if (pid == 0) {
        // // Close any unnecessary file descriptors
        // for (int fd = 3; fd < sysconf(_SC_OPEN_MAX); ++fd) {
        //     close(fd);
        // }

        // Child process: reset signal handlers to default
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);  // Reset SIGCHLD if necessary

        // Child process: execute the function
        spdlog::info("{} - Child Process (PID: {}) started.", name, getpid());
        func();
        exit(EXIT_SUCCESS);  // Should not reach here
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
    exit(EXIT_SUCCESS);
}
