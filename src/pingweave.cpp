#include "rdma_client.hpp"
#include "rdma_server.hpp"
#include "tcp_client.hpp"
#include "tcp_server.hpp"
#include "udp_client.hpp"
#include "udp_server.hpp"

/** TODO: namespace */
// using namespace pingweave;

pid_t start_process(std::function<void()> func, const std::string& name);
void signal_handler(int sig);
void sigchld_handler(int sig);

// Structure to hold process information
struct ProcessInfo {
    pid_t pid;
    std::function<void()> func;
    std::string name;
    std::string host;
};

// Global variables
std::vector<ProcessInfo> processes_cpp_programs;
ProcessInfo process_py_client = {0};
ProcessInfo process_py_server = {0};

bool running = true;

const std::string alias_roce_server = "pingweave_roce_server";
const std::string alias_roce_client = "pingweave_roce_client";
const std::string alias_ib_server = "pingweave_ib_server";
const std::string alias_ib_client = "pingweave_ib_client";
const std::string alias_tcp_server = "pingweave_tcp_server";
const std::string alias_tcp_client = "pingweave_tcp_client";
const std::string alias_udp_server = "pingweave_udp_server";
const std::string alias_udp_client = "pingweave_udp_client";

void terminate_invalid_cpp_program(const std::set<std::string>& myaddr,
                                   const std::string& program_name,
                                   std::vector<ProcessInfo>& cpp_programs,
                                   std::set<std::string>& running);

void start_python_process(ProcessInfo& process, const std::string& script_name,
                          const std::string& process_name,
                          const std::string& host);

void start_cpp_programs(
    const std::set<std::string>& myaddr, const std::string& program_name,
    const std::string& protocol,
    std::function<void(const std::string&, const std::string&)>
        program_function,
    std::vector<ProcessInfo>& processes_cpp_programs,
    std::set<std::string>& running_cpp_programs);
    
/* main function */
int main() {
    spdlog::info("Clear the download / upload directory");
    delete_files_in_directory(get_src_dir() + DIR_UPLOAD_PATH);
    delete_files_in_directory(get_src_dir() + DIR_DOWNLOAD_PATH);

    spdlog::info("--- Main thread starts");
    message_to_http_server("Main thread starts", "/alarm",
                           spdlog::default_logger());  // alarm to controller
    process_py_client.host = "null";
    process_py_server.host = "null";

    // sanity check - table expiry timer
    if (PINGWEAVE_TABLE_EXPIRY_TIME_RDMA_MS > 2000) {
        spdlog::error(
            "PINGWEAVE_TABLE_EXPIRY_TIME_RDMA_MS must be shorter than 2 "
            "seconds");
        exit(EXIT_FAILURE);
    }

    // Register signal handlers in the parent process only
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, sigchld_handler);  // to handle zombie processes

    int pinglist_load_retry_cnt = -1;
    // Monitor processes for every second and restart them if they exit
    while (running) {
        int status;
        /* 0. Handle zombie child processes */
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            spdlog::info("[main] Child process (PID: {}) terminated.", pid);
        }

        /* Load the controler address and my RDMA/UDP IP addresses */
        std::string controller_host;
        int controller_port;
        // get controller address from config/pingweave.ini
        get_controller_info_from_ini(controller_host, controller_port);

        // get local ip addresses
        std::set<std::string> local_ips = get_all_local_ips();

        // get the absolute path of pinglist.yaml
        std::string pinglist_filepath;
        pinglist_filepath =
            local_ips.find(controller_host) != local_ips.end()
                ? get_src_dir() + DIR_CONFIG_PATH + "/pinglist.yaml"
                : get_src_dir() + DIR_DOWNLOAD_PATH + "/pinglist.yaml";

        // get my "ping-valid" addresses from pinglist.yaml
        std::set<std::string> myaddr_roce, myaddr_ib, myaddr_tcp, myaddr_udp;
        if (get_my_addr_from_pinglist(pinglist_filepath, myaddr_roce, myaddr_ib,
                                      myaddr_tcp, myaddr_udp)) {
            if (pinglist_load_retry_cnt >= 0 &&
                ++pinglist_load_retry_cnt < 10) {
                spdlog::warn(
                    "Loading a pinglist is failed. Retry count: {}/10.",
                    pinglist_load_retry_cnt);
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
        } else {
            pinglist_load_retry_cnt = 0;

            // if loading pinglist is failed, return the empty set.
            spdlog::debug("Size of myaddr_roce({})/ib({})/tcp({})/udp({})",
                          myaddr_roce.size(), myaddr_ib.size(),
                          myaddr_tcp.size(), myaddr_udp.size());
            if (myaddr_roce.empty()) {
                spdlog::debug("Empty RoCE info in pinglist.yaml.");
            }
            if (myaddr_ib.empty()) {
                spdlog::debug("Empty IB info in pinglist.yaml.");
            }
            if (myaddr_tcp.empty()) {
                spdlog::debug("Empty TCP info in pinglist.yaml.");
            }
            if (myaddr_udp.empty()) {
                spdlog::debug("Empty UDP info in pinglist.yaml.");
            }
        }

        /* 2. Terminate threads which are not in pinglist. */
        // python - check only server because a client must always be running
        if (process_py_server.pid > 0 &&
            process_py_server.host != controller_host) {
            spdlog::info("IP {} is no more controller. Exit the python thread",
                         process_py_server.host);
            int result = kill(process_py_server.pid, SIGTERM);
            if (result != 0) {
                spdlog::error("Failed to send signal to PID {}: {}",
                              process_py_server.pid, strerror(errno));
            }
            process_py_server = {0};
            process_py_server.host = "null";
        }

        // cpp programs running on this node
        std::set<std::string> running_cpp_programs;

        // check roce_server
        terminate_invalid_cpp_program(myaddr_roce, alias_roce_server,
                                      processes_cpp_programs,
                                      running_cpp_programs);
        terminate_invalid_cpp_program(myaddr_roce, alias_roce_client,
                                      processes_cpp_programs,
                                      running_cpp_programs);
        terminate_invalid_cpp_program(myaddr_ib, alias_ib_server,
                                      processes_cpp_programs,
                                      running_cpp_programs);
        terminate_invalid_cpp_program(myaddr_ib, alias_ib_client,
                                      processes_cpp_programs,
                                      running_cpp_programs);
        terminate_invalid_cpp_program(myaddr_tcp, alias_tcp_server,
                                      processes_cpp_programs,
                                      running_cpp_programs);
        terminate_invalid_cpp_program(myaddr_tcp, alias_tcp_client,
                                      processes_cpp_programs,
                                      running_cpp_programs);
        terminate_invalid_cpp_program(myaddr_udp, alias_udp_server,
                                      processes_cpp_programs,
                                      running_cpp_programs);
        terminate_invalid_cpp_program(myaddr_udp, alias_udp_client,
                                      processes_cpp_programs,
                                      running_cpp_programs);

        /* 3. Start new threads which are added to pinglist */
        // Start Python server
        if (local_ips.find(controller_host) != local_ips.end()) {
            start_python_process(process_py_server, "pingweave_server.py",
                                 "py_server", controller_host);
        }

        // Start Python client
        start_python_process(process_py_client, "pingweave_client.py",
                             "py_client", "localhost");

        // cpp programs
        start_cpp_programs(myaddr_roce, alias_roce_server, "roce", rdma_server,
                           processes_cpp_programs, running_cpp_programs);
        start_cpp_programs(myaddr_roce, alias_roce_client, "roce", rdma_client,
                           processes_cpp_programs, running_cpp_programs);
        start_cpp_programs(myaddr_ib, alias_ib_server, "ib", rdma_server,
                           processes_cpp_programs, running_cpp_programs);
        start_cpp_programs(myaddr_ib, alias_ib_client, "ib", rdma_client,
                           processes_cpp_programs, running_cpp_programs);
        start_cpp_programs(myaddr_tcp, alias_tcp_server, "tcp", tcp_server,
        processes_cpp_programs, running_cpp_programs);
        start_cpp_programs(myaddr_tcp, alias_tcp_client, "tcp", tcp_client,
        processes_cpp_programs, running_cpp_programs);
        start_cpp_programs(myaddr_udp, alias_udp_server, "udp", udp_server,
                           processes_cpp_programs, running_cpp_programs);
        start_cpp_programs(myaddr_udp, alias_udp_client, "udp", udp_client,
                           processes_cpp_programs, running_cpp_programs);

        // sleep for a while to save CPU resource
        std::this_thread::sleep_for(
            std::chrono::seconds(CHECK_PROCESS_INTERVAL_SEC));
    }

    return 0;
}

void set_process_name(const std::string& new_name) {
    // `program_invocation_name` points to the original `argv[0]`
    extern char* program_invocation_name;
    strncpy(program_invocation_name, new_name.c_str(), strlen(program_invocation_name));
    program_invocation_name[strlen(new_name.c_str())] = '\0';
}

// Function to start a new process running the given function
pid_t start_process(std::function<void()> func, const std::string& name) {
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

        // Set the process name for child
        set_process_name(name);

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
    message_to_http_server("*** Main thread exits", "/alarm",
                           spdlog::default_logger());  // send to controller
    for (int i = 0; i < processes_cpp_programs.size(); ++i) {
        kill(processes_cpp_programs[i].pid, SIGTERM);
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

    // collect all terminated child processes status and craft a alarm msg
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        alarm_msg = "Child process termination is detected: ";
        spdlog::info("[sigchld] Child process (PID: {}) terminated.", pid);

        // If accidentally killed, handle the status and make logs
        if (process_py_client.pid == pid) {
            spdlog::info("-> process name: {}", process_py_client.name);
            alarm_msg += process_py_client.host + ":" + process_py_client.name;
            process_py_client = {0};  // renew
            process_py_client.host = "null";
        }
        if (process_py_server.pid == pid) {
            spdlog::info("-> process name: {}", process_py_server.name);
            alarm_msg += process_py_server.host + ":" + process_py_server.name;
            process_py_server = {0};  // renew
            process_py_server.host = "null";
        }
        for (auto it = processes_cpp_programs.begin();
             it != processes_cpp_programs.end(); ++it) {
            if (it->pid == pid) {
                spdlog::info("-> process name: {}", it->name);
                alarm_msg += it->host + ":" + it->name;
                processes_cpp_programs.erase(it);
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


void terminate_invalid_cpp_program(const std::set<std::string>& myaddr,
                                   const std::string& program_name,
                                   std::vector<ProcessInfo>& cpp_programs,
                                   std::set<std::string>& running) {
    for (auto it = cpp_programs.begin(); it != cpp_programs.end();) {
        if (myaddr.find(it->host) == myaddr.end() && it->name == program_name) {
            spdlog::info("IP {} is not in pinglist. Exit the thread: {}.",
                         it->host, it->name);
            int result = kill(it->pid, SIGTERM);
            if (result != 0) {
                spdlog::error("Failed to send signal to pid {} ({}): {}",
                              it->pid, it->name, strerror(errno));
            }
            it = cpp_programs.erase(it);
        } else {
            // memorize the running cpp threads
            running.insert(it->host + it->name);
            ++it;
        }
    }
}

void start_python_process(ProcessInfo& process, const std::string& script_name,
                          const std::string& process_name,
                          const std::string& host) {
    const std::string script_path = get_src_dir() + "/" + script_name;
    if (process.host == "null") {  // if nothing is running
        spdlog::info("Start the {} thread at {}", process_name, host);
        process = {
            start_process(
                [&] {
                    execlp("python3", "python3", script_path.c_str(),
                           (char*)nullptr);
                },
                process_name),  // pid
            std::bind(execlp, "python3", "python3", script_path.c_str(),
                      (char*)nullptr),  // function
            process_name,               // name
            host                        // host
        };
    }
}

void start_cpp_programs(
    const std::set<std::string>& myaddr, const std::string& program_name,
    const std::string& protocol,
    std::function<void(const std::string&, const std::string&)>
        program_function,
    std::vector<ProcessInfo>& processes_cpp_programs,
    std::set<std::string>& running_cpp_programs) {
    for (const auto& addr : myaddr) {
        if (running_cpp_programs.find(addr + program_name) ==
            running_cpp_programs.end()) {
            spdlog::info("Start the thread - {}, {}", addr, program_name);
            processes_cpp_programs.push_back(
                {start_process([&] { program_function(addr, protocol); },
                               program_name),
                 std::bind(program_function, addr, protocol), program_name,
                 addr});
        }
    }
}
