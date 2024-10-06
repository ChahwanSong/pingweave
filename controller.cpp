#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>  // std::unique_ptr 사용을 위해 추가
#include <mutex>
#include <thread>
#include <vector>

const int MAX_EVENTS = 10;
const int PORT = 8080;

std::mutex file_mutex;
std::string file_content;
time_t last_mod_time;

// Custom deleter for unique_ptr to handle socket closing
struct SocketDeleter {
    void operator()(int* sock_fd) const {
        if (*sock_fd != -1) {
            close(*sock_fd);
            std::cout << "Socket closed: " << *sock_fd << std::endl;
        }
    }
};

void set_non_blocking(int socket) {
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1) {
        std::cerr << "Error: fcntl get failed - " << strerror(errno)
                  << std::endl;
        return;
    }
    if (fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        std::cerr << "Error: fcntl set failed - " << strerror(errno)
                  << std::endl;
    }
}

time_t get_file_mod_time(const std::string& filename) {
    struct stat file_stat;
    if (stat(filename.c_str(), &file_stat) == 0) {
        return file_stat.st_mtime;
    }
    std::cerr << "Error: Unable to get file modification time - "
              << strerror(errno) << std::endl;
    return 0;  // Return 0 if file doesn't exist or an error occurs
}

void load_pinglist_file() {
    std::lock_guard<std::mutex> lock(file_mutex);
    std::ifstream file("pinglist.yaml");
    if (!file.is_open()) {
        std::cerr << "Error: Failed to open pinglist.yaml" << std::endl;
        file_content = "File not found!";
        return;
    }
    try {
        file_content.assign((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        file.close();
    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to load pinglist.yaml - " << e.what()
                  << std::endl;
        file_content = "Error loading file!";
    }
}

void monitor_file_changes() {
    const std::string filename = "pinglist.yaml";
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        time_t mod_time = get_file_mod_time(filename);
        if (mod_time > last_mod_time) {
            last_mod_time = mod_time;
            load_pinglist_file();
            std::cout << "pinglist.yaml updated." << std::endl;
        }
    }
}

void handle_client(int client_socket) {
    // Use unique_ptr for automatic socket cleanup
    std::unique_ptr<int, SocketDeleter> socket_ptr(new int(client_socket));

    std::lock_guard<std::mutex> lock(file_mutex);
    ssize_t sent_bytes =
        send(*socket_ptr, file_content.c_str(), file_content.size(), 0);
    if (sent_bytes == -1) {
        std::cerr << "Error: Failed to send data to client - "
                  << strerror(errno) << std::endl;
    }
    // No need to manually close the socket as unique_ptr will handle it
}

void epoll_server() {
    std::unique_ptr<int, SocketDeleter> server_fd(
        new int(socket(AF_INET, SOCK_STREAM, 0)), SocketDeleter());
    if (*server_fd == -1) {
        std::cerr << "Error: Socket creation failed - " << strerror(errno)
                  << std::endl;
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(*server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
                   sizeof(opt)) == -1) {
        std::cerr << "Error: setsockopt failed - " << strerror(errno)
                  << std::endl;
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(*server_fd, (struct sockaddr*)&address, sizeof(address)) == -1) {
        std::cerr << "Error: Bind failed - " << strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    if (listen(*server_fd, 3) == -1) {
        std::cerr << "Error: Listen failed - " << strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "Error: epoll_create1 failed - " << strerror(errno)
                  << std::endl;
        exit(EXIT_FAILURE);
    }

    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = *server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, *server_fd, &event) == -1) {
        std::cerr << "Error: epoll_ctl failed - " << strerror(errno)
                  << std::endl;
        exit(EXIT_FAILURE);
    }

    std::vector<std::thread> thread_pool;

    while (true) {
        struct epoll_event events[MAX_EVENTS];
        int num_fds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (num_fds == -1) {
            std::cerr << "Error: epoll_wait failed - " << strerror(errno)
                      << std::endl;
            continue;
        }

        for (int i = 0; i < num_fds; i++) {
            if (events[i].data.fd == *server_fd) {
                int new_socket = accept(*server_fd, nullptr, nullptr);
                if (new_socket < 0) {
                    std::cerr << "Error: Accept failed - " << strerror(errno)
                              << std::endl;
                    continue;
                }
                set_non_blocking(new_socket);

                event.data.fd = new_socket;
                event.events = EPOLLIN | EPOLLET;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_socket, &event) ==
                    -1) {
                    std::cerr << "Error: epoll_ctl (client socket) failed - "
                              << strerror(errno) << std::endl;
                    close(new_socket);
                }
            } else {
                int client_socket = events[i].data.fd;
                if (events[i].events & EPOLLIN) {
                    thread_pool.emplace_back(handle_client, client_socket);
                }
            }
        }

        // Clean up finished threads
        thread_pool.erase(
            std::remove_if(thread_pool.begin(), thread_pool.end(),
                           [](std::thread& t) { return !t.joinable(); }),
            thread_pool.end());
    }
}

int main() {
    last_mod_time = get_file_mod_time("pinglist.yaml");
    if (last_mod_time == 0) {
        std::cerr << "Warning: Could not get initial file modification time."
                  << std::endl;
    }
    load_pinglist_file();

    std::thread monitor_thread(monitor_file_changes);
    monitor_thread.detach();

    epoll_server();

    return 0;
}
