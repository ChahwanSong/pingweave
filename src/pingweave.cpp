#include <functional>

#include "rdma_client.hpp"
#include "rdma_server.hpp"

// using namespace pingweave;

pid_t start_process(std::function<void()> func, const char* name);
void signal_handler(int sig);
void sigchld_handler(int sig);

// Structure to hold process information
struct ProcessInfo {
    pid_t pid;
    std::function<void()> func;
    const char* name;
    std::string host;
};

// Global variables
std::vector<ProcessInfo> processes_cpp_server;
std::vector<ProcessInfo> processes_cpp_client;
ProcessInfo process_py_client = {0};
ProcessInfo process_py_server = {0};

bool running = true;

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
    message_to_http_server("Main thread starts", "/alarm",
                           spdlog::default_logger());  // alarm to controller
    process_py_client.host = "null";
    process_py_server.host = "null";

    // Register signal handlers in the parent process only
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, sigchld_handler);  // to handle zombie processes

    // Monitor processes for every second and restart them if they exit
    while (running) {
        int status;
        /* 0. Handle zombie child processes */
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            spdlog::info("[main] Child process (PID: {}) terminated.", pid);
        }

        /* 1. Load my RDMA IP addresses periodically (pinglist_abs_path) */
        std::set<std::string> myaddr_rdma;
        get_my_rdma_addr_from_pinglist(pinglist_abs_path, myaddr_rdma);

        // if loading pinglist is failed, myaddr_rdma will be empty.
        spdlog::debug("Myaddr_rdma size: {}", myaddr_rdma.size());

        /* 2. Terminate threads which are not in pinglist. */
        // python programs - only check server
        std::string controller_host;
        int controller_port;
        get_controller_info_from_ini(controller_host, controller_port);
        if (process_py_server.pid > 0 &&
            process_py_server.host != controller_host) {
            spdlog::info("IP {} is no more controller. Exit the python thread",
                         process_py_server.host);
            int result = kill(process_py_server.pid, SIGTERM);
            if (result != 0) {
                spdlog::error("Faile dto send signal to PID {}: {}",
                              process_py_server.pid, strerror(errno));
            }
            process_py_server = {0};
            process_py_server.host = "null";
        }

        // cpp programs
        std::set<std::string> running_programs_cpp_server;
        for (auto it = processes_cpp_server.begin();
             it != processes_cpp_server.end();) {
            if (myaddr_rdma.find(it->host) == myaddr_rdma.end()) {
                spdlog::info(
                    "IP {} is not in pinglist anymore. Exit the thread {}.",
                    it->host, it->name);
                int result = kill(it->pid, SIGTERM);
                if (result != 0) {
                    spdlog::error("Failed to send signal to PID {} ({}): {}",
                                  it->pid, it->name, strerror(errno));
                }
                it = processes_cpp_server.erase(it);
            } else {
                // memorize the running cpp threads
                running_programs_cpp_server.insert(it->host);
                ++it;
            }
        }

        std::set<std::string> running_programs_cpp_client;
        for (auto it = processes_cpp_client.begin();
             it != processes_cpp_client.end();) {
            if (myaddr_rdma.find(it->host) == myaddr_rdma.end()) {
                spdlog::info(
                    "IP {} is not in pinglist anymore. Exit the thread {}.",
                    it->host, it->name);
                int result = kill(it->pid, SIGTERM);
                if (result != 0) {
                    spdlog::error("Failed to send signal to PID {} ({}): {}",
                                  it->pid, it->name, strerror(errno));
                }
                it = processes_cpp_client.erase(it);
            } else {
                // memorize the running cpp threads
                running_programs_cpp_client.insert(it->host);
                ++it;
            }
        }

        /* 3. Start new threads which are added to pinglist */
        std::set<std::string> local_ips = get_all_local_ips();
        // python programs
        if (process_py_server.host == "null") {  // if nothing is running
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

        if (process_py_client.host == "null") {  // if nothing is running
            spdlog::info("Start the pingweave-client thread");
            process_py_client = {
                start_process(
                    [&] {
                        execlp("python3", "python3", py_client_abs_path.c_str(),
                               (char*)nullptr);
                    },
                    "py_client"),  // pid
                std::bind(execlp, "python3", "python3",
                          py_client_abs_path.c_str(),
                          (char*)nullptr),  // function
                "py_client",                // name
                "localhost"                 // host
            };
        }

        // cpp programs
        for (auto it = myaddr_rdma.begin(); it != myaddr_rdma.end(); ++it) {
            if (running_programs_cpp_server.find(*it) ==
                running_programs_cpp_server.end()) {
                spdlog::info("Start the thread - {}, {}", *it, "rdma_server");
                processes_cpp_server.push_back(
                    {start_process([&] { rdma_server(*it); }, "rdma_server"),
                     std::bind(rdma_server, *it), "rdma_server", *it});
            }
        }
        for (auto it = myaddr_rdma.begin(); it != myaddr_rdma.end(); ++it) {
            if (running_programs_cpp_client.find(*it) ==
                running_programs_cpp_client.end()) {
                spdlog::info("Start the thread - {}, {}", *it, "rdma_client");
                processes_cpp_client.push_back(
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
    message_to_http_server("Main thread exits", "/alarm",
                           spdlog::default_logger());  // send to controller
    for (int i = 0; i < processes_cpp_server.size(); ++i) {
        kill(processes_cpp_server[i].pid, SIGTERM);
    }
    for (int i = 0; i < processes_cpp_client.size(); ++i) {
        kill(processes_cpp_client[i].pid, SIGTERM);
    }
    kill(process_py_client.pid, SIGTERM);
    kill(process_py_server.pid, SIGTERM);
    exit(EXIT_SUCCESS);
}

void sigchld_handler(int sig) {
    int saved_errno = errno;  // remember previous errno
    int status;
    int result;
    pid_t pid;
    std::string alarm_msg;

    // collect all terminated child processes status
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        alarm_msg = "Child process termination is detected: ";
        spdlog::info("[sigchld] Child process (PID: {}) terminated.", pid);

        // If accidentally killed, handle the status and make logs
        if (process_py_client.pid == pid) {
            spdlog::info("-> process name: {}", process_py_client.name);
            alarm_msg += process_py_client.name;
            process_py_client = {0};  // renew
            process_py_client.host = "null";
        }
        if (process_py_server.pid == pid) {
            spdlog::info("-> process name: {}", process_py_server.name);
            alarm_msg += process_py_server.name;
            process_py_server = {0};  // renew
            process_py_server.host = "null";
        }
        for (auto it = processes_cpp_server.begin();
             it != processes_cpp_server.end(); ++it) {
            if (it->pid == pid) {
                spdlog::info("-> process name: {}", it->name);
                alarm_msg += it->name;
                processes_cpp_server.erase(it);
                break;
            }
        }
        for (auto it = processes_cpp_client.begin();
             it != processes_cpp_client.end(); ++it) {
            if (it->pid == pid) {
                spdlog::info("-> process name: {}", it->name);
                alarm_msg += it->name;
                processes_cpp_client.erase(it);
                break;
            }
        }

        // send to controller
        message_to_http_server(alarm_msg, "/alarm", spdlog::default_logger());

        // ensure to kill the process
        kill(pid, SIGTERM);
    }

    errno = saved_errno;  // back to previous errno
}