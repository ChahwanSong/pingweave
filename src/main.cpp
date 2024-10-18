#include "server_rx.hpp"
#include "server_tx.hpp"

void client_rx();
void client_tx();
pid_t start_process(void (*func)(), const char* name);
void signal_handler(int sig);

// Structure to hold process information
struct ProcessInfo {
    pid_t pid;
    void (*func)();
    const char* name;
};

// Global variables
ProcessInfo processes[4];
bool running = true;

int main() {
    // Register signal handlers in the parent process only
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Start each function in a separate process
    processes[0] = {start_process(server_rx, "server_rx"), server_rx,
                    "server_rx"};
    processes[1] = {start_process(server_tx, "server_tx"), server_tx,
                    "server_tx"};
    processes[2] = {start_process(client_rx, "client_rx"), client_rx,
                    "client_rx"};
    processes[3] = {start_process(client_tx, "client_tx"), client_tx,
                    "client_tx"};

    // Monitor processes and restart them if they exit
    while (running) {
        sleep(1);  // Check every 1 second
        for (int i = 0; i < 4; ++i) {
            int status;
            pid_t result = waitpid(processes[i].pid, &status, WNOHANG);
            if (result == 0) {
                // Process is still running
            } else if (result == processes[i].pid) {
                // Process has terminated; restart it
                std::cout << processes[i].name << " (PID: " << processes[i].pid
                          << ") has terminated. Restarting...\n";
                processes[i].pid =
                    start_process(processes[i].func, processes[i].name);
            } else {
                // Error handling
                std::cerr << "Error with waitpid for " << processes[i].name
                          << "\n";
            }
        }
    }

    return 0;
}

// Function to start a new process running the given function
pid_t start_process(void (*func)(), const char* name) {
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork process\n";
        exit(1);
    } else if (pid == 0) {
        // Child process: reset signal handlers to default
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);  // Reset SIGCHLD if necessary

        // Child process: execute the function
        std::cout << name << " started (PID: " << getpid() << ")\n";
        func();
        exit(0);  // Should not reach here
    } else {
        // Parent process: return child's PID
        std::cout << "Started process " << name << " (PID: " << pid << ")\n";
        return pid;
    }
}

// Signal handler to terminate all child processes
void signal_handler(int sig) {
    running = false;
    std::cout << "\nTerminating all child processes...\n";
    for (int i = 0; i < 4; ++i) {
        kill(processes[i].pid, SIGTERM);
    }
    exit(0);
}

void client_rx() {
    while (true) {
        std::cout << "client_rx running (PID: " << getpid() << ")\n";
        sleep(1);
    }
}

void client_tx() {
    while (true) {
        std::cout << "client_tx running (PID: " << getpid() << ")\n";
        sleep(1);
    }
}

/**

g++ -std=c++17 -I ../libs -pthread -O2 main.cpp common.cpp producer_queue.cpp \
        logger.cpp -o main_executable -l ibverbs

 */