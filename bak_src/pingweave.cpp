#include <functional>

#include "rdma_client.hpp"
#include "rdma_server.hpp"

pid_t start_process(std::function<void()> func, const char* name);
pid_t start_python_process(const std::string& script_path, const char* name);
void signal_handler(int sig);

// Structure to hold process information
struct ProcessInfo {
    pid_t pid;
    std::function<void()> func;
    const char* name;
    std::string host;
};

// Global variables
std::vector<ProcessInfo> processes_cpp;
std::vector<ProcessInfo> processes_py;
bool running = true;

// get ip addresses of this node
const std::string pinglist_abs_path =
    get_source_directory() + DIR_DOWNLOAD_PATH + "pinglist.yaml";

// Python script paths
const std::string py_client_path =
    get_source_directory() + "/pingweave_client.py";
const std::string py_server_path =
    get_source_directory() + "/pingweave_server.py";

/* main function */
int main() {
    spdlog::info("--- Main thread starts");

    // Register signal handlers in the parent process only
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Start and monitor the Python script
    processes_py.push_back({
        start_process(
            [&] {
                execlp("python3", "python3", py_client_path.c_str(),
                       (char*)nullptr);
            },
            "py_client"),  // pid
        std::bind(execlp, "python3", "python3", py_client_path.c_str(),
                  (char*)nullptr),  // function
        "py_client",                // name
        "localhost"                 // host
    });

    processes_py.push_back({
        start_process(
            [&] {
                execlp("python3", "python3", py_server_path.c_str(),
                       (char*)nullptr);
            },
            "py_server"),  // pid
        std::bind(execlp, "python3", "python3", py_server_path.c_str(),
                  (char*)nullptr),  // function
        "py_server",                // name
        "localhost"                 // host
    });

    // Monitor processes for every second and restart them if they exit
    while (running) {
        /* 1. Load my IP addresses periodically (pinglist_abs_path) */
        std::set<std::string> myaddr;
        get_my_addr(pinglist_abs_path, myaddr);

        // if loading pinglist is failed, myaddr will be empty.
        spdlog::debug("Myaddr size: {}", myaddr.size());

        /* 2. Check and restart if threads are exited */
        for (int i = 0; i < processes_cpp.size(); ++i) {
            int status;
            pid_t result = waitpid(processes_cpp[i].pid, &status, WNOHANG);
            if (result == 0) {
                // Process is still running
            } else if (result == processes_cpp[i].pid) {
                // Process has terminated; restart it
                spdlog::info("{} (PID {}) has terminated",
                             processes_cpp[i].name, processes_cpp[i].pid);
                processes_cpp[i].pid =
                    start_process(processes_cpp[i].func, processes_cpp[i].name);
            } else {
                // Error handling
                spdlog::error("Error with waitpid for {}",
                              processes_cpp[i].name);
            }
        }

        for (int i = 0; i < processes_py.size(); ++i) {
            int status;
            pid_t result = waitpid(processes_py[i].pid, &status, WNOHANG);
            if (result == 0) {
                // Process is still running
            } else if (result == processes_py[i].pid) {
                // Process has terminated; restart it
                spdlog::info("{} (PID {}) has terminated", processes_py[i].name,
                             processes_py[i].pid);
                processes_py[i].pid =
                    start_process(processes_py[i].func, processes_py[i].name);
            } else {
                // Error handling
                spdlog::error("Error with waitpid for {}",
                              processes_py[i].name);
            }
        }

        /* 3. Terminate threads which are not in pinglist */
        std::set<std::string> running;
        for (auto it = processes_cpp.begin(); it != processes_cpp.end();) {
            if (myaddr.find(it->host) == myaddr.end()) {
                spdlog::info(
                    "IP {} is not in pinglist anymore. Exit the thread.",
                    it->host);
                int result = kill(it->pid, SIGTERM);
                if (result != 0) {
                    spdlog::error("Faile dto send signal to PID {}: {}",
                                  it->pid, strerror(errno));
                }
                it = processes_cpp.erase(it);
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
                processes_cpp.push_back(
                    {start_process([&] { rdma_server(*it); }, "rdma_server"),
                     std::bind(rdma_server, *it), "rdma_server", *it});
                processes_cpp.push_back(
                    {start_process([&] { rdma_client(*it); }, "rdma_client"),
                     std::bind(rdma_client, *it), "rdma_client", *it});
            }
        }

        // sleep for a while to save CPU resource
        std::this_thread::sleep_for(
            std::chrono::seconds(CHECK_PROCESS_INTERVAL_SEC));
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

// // Function to start the Python script as a new process
// pid_t start_python_process(const std::string& script_path, const char* name)
// {
//     pid_t pid = fork();
//     if (pid < 0) {
//         spdlog::error("Failed to fork process for Python script {}: {}",
//                       script_path, strerror(errno));
//         exit(EXIT_FAILURE);
//     } else if (pid == 0) {
//         // Child process: execute Python script
//         signal(SIGINT, SIG_DFL);
//         signal(SIGTERM, SIG_DFL);
//         signal(SIGCHLD, SIG_DFL);  // Reset SIGCHLD if necessary

//         spdlog::info("{} - Child process (PID: {}) started.", name,
//         getpid()); execlp("python3", "python3", script_path.c_str(),
//         (char*)nullptr); spdlog::error("Failed to execute Python script {}:
//         {}", script_path,
//                       strerror(errno));
//         exit(EXIT_FAILURE);  // Should not reach here
//     } else {
//         // Parent process: return child's PID
//         return pid;
//     }
// }

// Signal handler to terminate all child processes
void signal_handler(int sig) {
    running = false;
    spdlog::critical("*** Main thread exits. ***");
    for (int i = 0; i < processes_cpp.size(); ++i) {
        kill(processes_cpp[i].pid, SIGTERM);
    }
    for (int i = 0; i < processes_py.size(); ++i) {
        kill(processes_py[i].pid, SIGTERM);
    }
    exit(EXIT_SUCCESS);
}
