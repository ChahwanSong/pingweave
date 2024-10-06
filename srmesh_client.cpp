#include <arpa/inet.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

constexpr int QKEY = 0x11111111;  // QKEY 설정 상수
constexpr int BUFFER_SIZE = 50;   // 버퍼 크기 상수
constexpr int TIMEOUT_MS = 500;   // 타임아웃 시간 (500ms)

// 타겟 IP들에 대해 병렬로 각 타겟을 처리하는 부분에 랜덤 딜레이 추가
std::random_device rd;                        // 랜덤 시드
std::mt19937 gen(rd());                       // 랜덤 숫자 생성기
std::uniform_int_distribution<> dist(1, 10);  // 1 ~ 10 마이크로초 범위

// 로컬 머신의 IP 주소 목록을 가져오는 함수
std::vector<std::string> get_local_ip_addresses() {
    std::vector<std::string> local_ips;
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return local_ips;
    }

    // 네트워크 인터페이스를 순회하며 IPv4 주소 수집
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            char host[NI_MAXHOST];
            if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host,
                            NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) == 0) {
                local_ips.push_back(host);
            }
        }
    }

    freeifaddrs(ifaddr);  // 메모리 해제
    return local_ips;
}

// pinglist.yaml 파일에서 RDMA IP 리스트 및 그룹 정보를 가져오는 함수
std::map<std::string, std::vector<std::string>> get_rdma_groups_from_pinglist(
    const std::string& file_path) {
    std::map<std::string, std::vector<std::string>> rdma_groups;
    YAML::Node pinglist = YAML::LoadFile(file_path);

    // pinglist의 RDMA 카테고리에서 그룹별로 IP 리스트를 가져옴
    for (const auto& group : pinglist["rdma"]) {
        std::vector<std::string> ips;
        for (const auto& ip : group.second) {
            ips.push_back(ip.as<std::string>());
        }
        rdma_groups[group.first.as<std::string>()] = ips;
    }

    return rdma_groups;
}

// RDMA 장치의 IP가 특정 IP와 일치하는지 확인하는 함수
bool match_device_to_ip(struct ibv_context* context, const std::string& ip,
                        int& matched_port) {
    struct in_addr addr;
    // 주어진 IP 주소를 `in_addr` 형식으로 변환
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        std::cerr << "Error: Invalid IP address format for " << ip << std::endl;
        return false;
    }

    // RDMA 포트 번호를 순회하며 GID와 IP 주소 비교
    for (int port_num = 1; port_num <= 2; ++port_num) {
        union ibv_gid gid;
        if (ibv_query_gid(context, port_num, 0, &gid) == 0) {
            uint32_t gid_ipv4 = *(reinterpret_cast<uint32_t*>(
                &gid.raw[12]));  // GID의 IPv4 부분 추출
            if (gid_ipv4 == addr.s_addr) {
                matched_port = port_num;  // 매칭되는 포트 발견
                return true;
            }
        }
    }
    return false;
}

// GID와 QPN을 저장하는 구조체
struct GID_QPN {
    uint8_t gid[16];
    uint32_t qpn;
};

// CSV 파일을 파싱하고 IP를 키로 하는 unordered_map을 만드는 함수
std::unordered_map<std::string, GID_QPN> load_gid_qpn_map(
    const std::string& file_path) {
    std::unordered_map<std::string, GID_QPN> gid_qpn_map;
    std::ifstream file(file_path);
    std::string line;

    // CSV 파일의 각 행을 읽어옴
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string ip, gid_str;
        uint32_t qpn;
        uint8_t gid[16];

        // IP, GID, QPN을 추출
        if (std::getline(iss, ip, ',') && std::getline(iss, gid_str, ',') &&
            iss >> qpn) {
            // GID 문자열을 16바이트 배열로 변환
            sscanf(gid_str.c_str(),
                   "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:"
                   "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
                   &gid[0], &gid[1], &gid[2], &gid[3], &gid[4], &gid[5],
                   &gid[6], &gid[7], &gid[8], &gid[9], &gid[10], &gid[11],
                   &gid[12], &gid[13], &gid[14], &gid[15]);

            // IP를 키로 하고 GID와 QPN을 값으로 저장
            gid_qpn_map[ip] = {{gid[0], gid[1], gid[2], gid[3], gid[4], gid[5],
                                gid[6], gid[7], gid[8], gid[9], gid[10],
                                gid[11], gid[12], gid[13], gid[14], gid[15]},
                               qpn};
        }
    }
    return gid_qpn_map;
}

// RDMA 송신 및 수신 처리 함수
void process_target(struct ibv_context* context, struct ibv_pd* pd,
                    struct ibv_qp* queue_pair, struct ibv_cq* send_cq,
                    struct ibv_cq* recv_cq, const std::string& target_ip,
                    const GID_QPN& gid_qpn, char* send_buffer,
                    char* recv_buffer, struct ibv_mr* send_mr,
                    struct ibv_mr* recv_mr) {
    try {
        // Address Handle 생성
        struct ibv_ah_attr ah_attr = {};
        ah_attr.is_global = 1;
        ah_attr.grh.dgid.global.subnet_prefix =
            *(reinterpret_cast<const uint64_t*>(&gid_qpn.gid[0]));
        ah_attr.grh.dgid.global.interface_id =
            *(reinterpret_cast<const uint64_t*>(&gid_qpn.gid[8]));
        ah_attr.grh.sgid_index = 0;  // 로컬 포트에서 첫 번째 GID 사용
        ah_attr.grh.hop_limit = 3;   // 네트워크 홉
        ah_attr.dlid = 0;            // 대상 포트의 LID 사용

        struct ibv_ah* ah = ibv_create_ah(pd, &ah_attr);
        if (!ah) {
            std::cerr << "Error: Failed to create address handle for target "
                      << target_ip << std::endl;
            return;
        }

        // 송신 요청 생성
        struct ibv_sge send_sge = {};
        send_sge.addr = reinterpret_cast<uint64_t>(send_buffer);
        send_sge.length = BUFFER_SIZE;
        send_sge.lkey = send_mr->lkey;

        struct ibv_send_wr send_wr = {};
        send_wr.wr_id = 0;
        send_wr.next = nullptr;
        send_wr.sg_list = &send_sge;
        send_wr.num_sge = 1;
        send_wr.opcode = IBV_WR_SEND;
        send_wr.send_flags = IBV_SEND_SIGNALED;
        send_wr.wr.ud.ah = ah;
        send_wr.wr.ud.remote_qpn = gid_qpn.qpn;
        send_wr.wr.ud.remote_qkey = QKEY;

        struct ibv_send_wr* bad_send_wr = nullptr;
        if (ibv_post_send(queue_pair, &send_wr, &bad_send_wr)) {
            std::cerr << "Error: Failed to post send work request to target "
                      << target_ip << std::endl;
            ibv_destroy_ah(ah);
            return;
        }

        // 송신 완료 대기
        struct ibv_wc send_wc;
        while (ibv_poll_cq(send_cq, 1, &send_wc) == 0) {
            // 대기
        }
        if (send_wc.status != IBV_WC_SUCCESS) {
            std::cerr << "Error: Send work completion failed with status "
                      << send_wc.status << std::endl;
        } else {
            std::cout << "Message successfully sent to " << target_ip
                      << std::endl;

            // 수신 준비
            auto start = std::chrono::steady_clock::now();

            struct ibv_sge recv_sge = {};
            recv_sge.addr = reinterpret_cast<uint64_t>(recv_buffer);
            recv_sge.length = BUFFER_SIZE;
            recv_sge.lkey = recv_mr->lkey;

            struct ibv_recv_wr recv_wr = {};
            recv_wr.wr_id = 0;
            recv_wr.next = nullptr;
            recv_wr.sg_list = &recv_sge;
            recv_wr.num_sge = 1;

            struct ibv_recv_wr* bad_recv_wr = nullptr;
            if (ibv_post_recv(queue_pair, &recv_wr, &bad_recv_wr)) {
                std::cerr
                    << "Error: Failed to post receive work request for target "
                    << target_ip << std::endl;
                return;
            }

            // 응답 대기 및 타임아웃 처리
            struct ibv_wc recv_wc;
            bool received = false;
            auto end_time = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(TIMEOUT_MS);

            while (std::chrono::steady_clock::now() < end_time) {
                if (ibv_poll_cq(recv_cq, 1, &recv_wc) > 0 &&
                    recv_wc.status == IBV_WC_SUCCESS) {
                    received = true;
                    break;
                }
                std::this_thread::sleep_for(
                    std::chrono::microseconds(1));  // 폴링 간격을 조정
            }

            if (received) {
                auto end = std::chrono::steady_clock::now();
                auto rtt =
                    std::chrono::duration_cast<std::chrono::microseconds>(end -
                                                                          start)
                        .count();
                std::cout << "Received response from " << target_ip << " in "
                          << rtt << " microseconds." << std::endl;
            } else {
                std::cerr << "Warning: No response received from " << target_ip
                          << " within timeout." << std::endl;
            }
        }

        ibv_destroy_ah(ah);  // 주소 핸들 해제

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

// RDMA 클라이언트 함수 (병렬 처리 및 주기적 실행)
void run_client(struct ibv_context* context, int port_num,
                const std::string& ip,
                const std::vector<std::string>& target_ips) {
    std::cout << "Running client on port " << port_num << " for IP " << ip
              << std::endl;

    // 보호 도메인 할당
    struct ibv_pd* pd = ibv_alloc_pd(context);
    if (!pd) {
        std::cerr << "Error: Failed to allocate protection domain."
                  << std::endl;
        return;
    }

    // 송수신 큐 및 완료 큐 생성
    struct ibv_comp_channel* comp_channel = ibv_create_comp_channel(context);
    if (!comp_channel) {
        std::cerr << "Error: Failed to create completion channel." << std::endl;
        ibv_dealloc_pd(pd);
        return;
    }

    struct ibv_cq* send_cq =
        ibv_create_cq(context, 16, nullptr, comp_channel, 0);
    struct ibv_cq* recv_cq =
        ibv_create_cq(context, 16, nullptr, comp_channel, 0);
    if (!send_cq || !recv_cq) {
        std::cerr << "Error: Failed to create send or receive completion queue."
                  << std::endl;
        if (send_cq) ibv_destroy_cq(send_cq);
        if (recv_cq) ibv_destroy_cq(recv_cq);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    // Queue Pair (QP) 생성
    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.send_cq = send_cq;
    qp_init_attr.recv_cq = recv_cq;
    qp_init_attr.cap.max_send_wr = 16;
    qp_init_attr.cap.max_recv_wr = 16;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.qp_type = IBV_QPT_UD;  // Unreliable Datagram (UD) QP

    struct ibv_qp* queue_pair = ibv_create_qp(pd, &qp_init_attr);  // QP 생성
    if (!queue_pair) {
        std::cerr << "Error: Failed to create queue pair." << std::endl;
        ibv_destroy_cq(send_cq);
        ibv_destroy_cq(recv_cq);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    // QP 상태 변경: INIT -> RTR -> RTS
    struct ibv_qp_attr qp_attr = {};
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = port_num;
    qp_attr.qkey = QKEY;

    if (ibv_modify_qp(
            queue_pair, &qp_attr,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)) {
        std::cerr << "Error: Failed to modify QP to INIT state." << std::endl;
        ibv_destroy_qp(queue_pair);
        ibv_destroy_cq(send_cq);
        ibv_destroy_cq(recv_cq);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    qp_attr.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(queue_pair, &qp_attr, IBV_QP_STATE)) {
        std::cerr << "Error: Failed to modify QP to RTR state." << std::endl;
        ibv_destroy_qp(queue_pair);
        ibv_destroy_cq(send_cq);
        ibv_destroy_cq(recv_cq);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;
    if (ibv_modify_qp(queue_pair, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        std::cerr << "Error: Failed to modify QP to RTS state." << std::endl;
        ibv_destroy_qp(queue_pair);
        ibv_destroy_cq(send_cq);
        ibv_destroy_cq(recv_cq);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    // 송수신을 위한 버퍼 생성 및 등록
    char send_buffer[BUFFER_SIZE];
    char recv_buffer[BUFFER_SIZE];
    struct ibv_mr* send_mr = ibv_reg_mr(pd, send_buffer, sizeof(send_buffer),
                                        IBV_ACCESS_LOCAL_WRITE);
    struct ibv_mr* recv_mr = ibv_reg_mr(pd, recv_buffer, sizeof(recv_buffer),
                                        IBV_ACCESS_LOCAL_WRITE);
    if (!send_mr || !recv_mr) {
        std::cerr << "Error: Failed to register memory region." << std::endl;
        ibv_destroy_qp(queue_pair);
        ibv_destroy_cq(send_cq);
        ibv_destroy_cq(recv_cq);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    // CSV 파일을 초기에 한번 로드
    std::unordered_map<std::string, GID_QPN> gid_qpn_map =
        load_gid_qpn_map("config/global_server_info.csv");

    // 1초마다 주기적으로 메시지 송수신 실행
    while (true) {
        auto start_time = std::chrono::steady_clock::now();

        // 병렬 실행을 위한 future 리스트
        std::vector<std::future<void>> futures;

        for (const auto& target_ip : target_ips) {
            // 병렬로 각 타겟 처리
            futures.push_back(std::async(
                std::launch::async, process_target, context, pd, queue_pair,
                send_cq, recv_cq, target_ip, gid_qpn_map.at(target_ip),
                send_buffer, recv_buffer, send_mr, recv_mr));
            // random 으로 1 ~ 10 마이크로초 중 랜덤 숫자 선택해서 delay를 넣음
            int random_delay = dist(gen);
            std::this_thread::sleep_for(
                std::chrono::microseconds(random_delay));
        }

        // 모든 타겟 처리 대기
        for (auto& future : futures) {
            future.get();
        }

        // 1초 주기 맞추기
        auto end_time = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(1) -
                                    (end_time - start_time));
    }

    // RDMA 리소스 해제
    ibv_dereg_mr(send_mr);
    ibv_dereg_mr(recv_mr);
    ibv_destroy_qp(queue_pair);
    ibv_destroy_cq(send_cq);
    ibv_destroy_cq(recv_cq);
    ibv_destroy_comp_channel(comp_channel);
    ibv_dealloc_pd(pd);
}

int main() {
    // 로컬 IP 주소 가져오기
    std::vector<std::string> local_ips = get_local_ip_addresses();

    // pinglist.yaml 파일에서 RDMA 그룹별 IP 리스트 가져오기
    std::map<std::string, std::vector<std::string>> rdma_groups =
        get_rdma_groups_from_pinglist("config/pinglist.yaml");

    // RDMA 장치 리스트 가져오기
    int num_devices;
    struct ibv_device** device_list = ibv_get_device_list(&num_devices);
    if (!device_list) {
        std::cerr << "Error: Failed to get RDMA devices." << std::endl;
        return 1;
    }

    std::vector<std::pair<std::thread, struct ibv_context*>> client_threads;

    // 각 RDMA 장치에 대해 RDMA IP와 일치하는 포트를 찾고 클라이언트 실행
    for (int i = 0; i < num_devices; ++i) {
        struct ibv_device* dev = device_list[i];
        struct ibv_context* context = ibv_open_device(dev);
        if (!context) continue;  // 장치 열기에 실패하면 건너뜀

        for (const auto& [group_name, group_ips] : rdma_groups) {
            for (const auto& rdma_ip : group_ips) {
                int matched_port = 0;
                // 매칭되는 포트가 있으면 클라이언트 실행
                if (match_device_to_ip(context, rdma_ip, matched_port)) {
                    // 클라이언트 실행 스레드 추가
                    client_threads.emplace_back(std::make_pair(
                        std::thread(run_client, context, matched_port, rdma_ip,
                                    group_ips),
                        context));
                }
            }
        }
    }

    // 모든 클라이언트 스레드가 종료될 때까지 대기
    for (auto& [thread, context] : client_threads) {
        if (thread.joinable()) {
            thread.join();
        }
        // RDMA 장치 닫기
        ibv_close_device(context);
    }

    // RDMA 장치 리스트 해제
    ibv_free_device_list(device_list);

    return 0;
}
