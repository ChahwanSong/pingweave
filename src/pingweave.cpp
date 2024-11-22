#include <functional>

#include "rdma_client.hpp"
#include "rdma_server.hpp"

// using namespace pingweave;

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
std::vector<ProcessInfo> processes_cpp;
ProcessInfo process_py_client = {};
ProcessInfo process_py_server = {};

bool running = true;

const std::string pingweave_ini_abs_path =
    get_source_directory() + DIR_CONFIG_PATH + "/pingweave_server.py";
const std::string pinglist_abs_path =
    get_source_directory() + DIR_DOWNLOAD_PATH + "/pinglist.yaml";
const std::string py_client_abs_path =
    get_source_directory() + "/pingweave_client.py";
const std::string py_server_abs_path =
    get_source_directory() + "/pingweave_server.py";

/* main function */
int main() {
    spdlog::info("Clear the download / upload directory");
    delete_files_in_directory(get_source_directory() + DIR_UPLOAD_PATH);
    delete_files_in_directory(get_source_directory() + DIR_DOWNLOAD_PATH);

    spdlog::info("--- Main thread starts");
    process_py_client.host = "null";
    process_py_server.host = "null";

    // Register signal handlers in the parent process only
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* initially, pingweave_client / pingweave_server must be running */
    {
        spdlog::info("Run the pingweave-client thread.");
        process_py_client = {
            start_process(
                [&] {
                    execlp("python3", "python3", py_client_abs_path.c_str(),
                           (char*)nullptr);
                },
                "py_client"),  // pid
            std::bind(execlp, "python3", "python3", py_client_abs_path.c_str(),
                      (char*)nullptr),  // function
            "py_client",                // name
            "localhost"                 // host
        };

        std::set<std::string> local_ips = get_all_local_ips();
        std::string controller_host;
        int controller_port;
        get_controller_info_from_ini(pingweave_ini_abs_path, controller_host,
                                     controller_port);
        for (auto it = local_ips.begin(); it != local_ips.end(); ++it) {
            if (controller_host == *it) {
                spdlog::info("Run the pingweave-server thread at {}.", *it);
                process_py_server = {
                    start_process(
                        [&] {
                            execlp("python3", "python3",
                                   py_server_abs_path.c_str(), (char*)nullptr);
                        },
                        "py_server"),  // pid
                    std::bind(execlp, "python3", "python3",
                              py_server_abs_path.c_str(),
                              (char*)nullptr),  // function
                    "py_server",                // name
                    controller_host             // host
                };
            }
        }
    }

    spdlog::info("Start the main loop after 5 seconds...");
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Monitor processes for every second and restart them if they exit
    while (running) {
        /* 1. Load my IP addresses periodically (pinglist_abs_path) */
        std::set<std::string> myaddr;
        get_my_addr(pinglist_abs_path, myaddr);
        int status;

        // if loading pinglist is failed, myaddr will be empty.
        spdlog::debug("Myaddr size: {}", myaddr.size());

        /* 2. Check and restart if threads are exited */
        // python programs
        {  // client
            pid_t result = waitpid(process_py_client.pid, &status, WNOHANG);
            if (result == 0) {
                // Process is still running
            } else if (result == process_py_client.pid) {
                // Process has terminated; restart it
                spdlog::info("{} (PID {}) has terminated",
                             process_py_client.name, process_py_client.pid);
                process_py_client.pid = start_process(process_py_client.func,
                                                      process_py_client.name);
            } else {
                // Error handling
                spdlog::error("Error with waitpid for {}",
                              process_py_client.name);
            }
        }

        {  // server
            pid_t result = waitpid(process_py_server.pid, &status, WNOHANG);
            if (result == 0) {
                // Process is still running
            } else if (result == process_py_server.pid) {
                // Process has terminated; restart it
                spdlog::info("{} (PID {}) has terminated",
                             process_py_server.name, process_py_server.pid);
                process_py_server.pid = start_process(process_py_server.func,
                                                      process_py_server.name);
            } else {
                // Error handling
                spdlog::error("Error with waitpid for {}",
                              process_py_server.name);
            }
        }

        // cpp programs
        for (int i = 0; i < processes_cpp.size(); ++i) {
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

        /* 3. Terminate threads which are not in pinglist. That means, if addr
         * is not in pinglist, we stop the processes. */
        // python programs - only check server
        std::string controller_host;
        int controller_port;
        get_controller_info_from_ini(pingweave_ini_abs_path, controller_host,
                                     controller_port);
        if (process_py_server.pid > 0 &&
            process_py_server.host != controller_host) {
            spdlog::info("IP {} is no more controller. Exit the python thread",
                         process_py_server.host);
            int result = kill(process_py_server.pid, SIGTERM);
            if (result != 0) {
                spdlog::error("Faile dto send signal to PID {}: {}",
                              process_py_server.pid, strerror(errno));
            }
            process_py_server = {};
            process_py_server.host = "null";
        }

        // cpp programs
        std::set<std::string> running_programs_cpp;
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
                // memorize the running cpp threads
                running_programs_cpp.insert(it->host);
                ++it;
            }
        }

        /* 4. Start new threads which are added to pinglist */
        // python programs - only check server
        if (process_py_server.host == "null") {  // if nothing is running
            std::set<std::string> local_ips = get_all_local_ips();
            if (local_ips.find(controller_host) != local_ips.end()) {
                spdlog::info("Start the pingweave-server thread at {}",
                             controller_host);
                process_py_server = {
                    start_process(
                        [&] {
                            execlp("python3", "python3",
                                   py_server_abs_path.c_str(), (char*)nullptr);
                        },
                        "py_server"),  // pid
                    std::bind(execlp, "python3", "python3",
                              py_server_abs_path.c_str(),
                              (char*)nullptr),  // function
                    "py_server",                // name
                    controller_host             // host
                };
            }
        }

        // cpp programs
        for (auto it = myaddr.begin(); it != myaddr.end(); ++it) {
            if (running_programs_cpp.find(*it) == running_programs_cpp.end()) {
                spdlog::info("IP {} is new. Run the cpp threads.", *it);
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
        spdlog::info("-> {} - Child Process (PID: {}) started.", name,
                     getpid());
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
    for (int i = 0; i < processes_cpp.size(); ++i) {
        kill(processes_cpp[i].pid, SIGTERM);
    }
    kill(process_py_client.pid, SIGTERM);
    kill(process_py_server.pid, SIGTERM);
    exit(EXIT_SUCCESS);
}
