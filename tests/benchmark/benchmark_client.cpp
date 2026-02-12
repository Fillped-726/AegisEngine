// // tests/benchmark_client.cpp
// #include <iostream>
// #include <vector>
// #include <thread>
// #include <atomic>
// #include <cstring>
// #include <chrono>
// #include <sys/socket.h>
// #include <arpa/inet.h>
// #include <unistd.h>
// #include <algorithm> // for sort
// #include <numeric>   // for accumulate
// #include <iomanip>   // for setprecision

// #include "cs_lobby.pb.h"

// using namespace aegis::cs::lobby;

// // --- 配置 ---
// const int CLIENT_COUNT = 500;
// const int PACKETS_PER_CLIENT = 1000;
// const std::string SERVER_IP = "127.0.0.1";
// const int SERVER_PORT = 8888;

// // --- 全局统计 ---
// std::atomic<int> g_success_count{0};
// std::atomic<int> g_fail_count{0};
// std::atomic<long long> g_total_bytes{0}; // 总传输字节数

// // --- 线程局部统计 ---
// struct ThreadStats
// {
//     std::vector<double> latencies_ms; // 记录每次请求的延迟
//     long long bytes = 0;
// };

// void client_thread_func(int id, ThreadStats &stats)
// {
//     // 预分配内存，避免统计时的 vector 扩容影响测试准确性
//     stats.latencies_ms.reserve(PACKETS_PER_CLIENT);

//     int sock = socket(AF_INET, SOCK_STREAM, 0);
//     struct sockaddr_in serv_addr;
//     serv_addr.sin_family = AF_INET;
//     serv_addr.sin_port = htons(SERVER_PORT);
//     inet_pton(AF_INET, SERVER_IP.c_str(), &serv_addr.sin_addr);

//     if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
//     {
//         g_fail_count++;
//         return;
//     }

//     // 构造包 (提前构造好，不要把序列化时间算进网络延迟里)
//     LoginReq req;
//     req.set_uid(10000 + id);
//     std::string body;
//     req.SerializeToString(&body);

//     uint32_t msg_id = htonl(1001);
//     uint32_t body_len = body.size();
//     uint32_t packet_len = htonl(4 + body_len);

//     std::vector<char> buffer;
//     buffer.resize(4 + 4 + body_len);
//     memcpy(buffer.data(), &packet_len, 4);
//     memcpy(buffer.data() + 4, &msg_id, 4);
//     memcpy(buffer.data() + 8, body.data(), body_len);

//     char recv_buf[4096];

//     for (int i = 0; i < PACKETS_PER_CLIENT; ++i)
//     {
//         // [Timer Start]
//         auto t1 = std::chrono::high_resolution_clock::now();

//         if (send(sock, buffer.data(), buffer.size(), 0) < 0)
//         {
//             g_fail_count++;
//             close(sock);
//             return;
//         }
//         stats.bytes += buffer.size();

//         // 接收回显
//         // 注意：真实场景包大小可能变，这里简单处理
//         int n = read(sock, recv_buf, sizeof(recv_buf));
//         if (n <= 0)
//         {
//             g_fail_count++;
//             close(sock);
//             return;
//         }
//         stats.bytes += n;

//         // [Timer End]
//         auto t2 = std::chrono::high_resolution_clock::now();
//         std::chrono::duration<double, std::milli> ms = t2 - t1;
//         stats.latencies_ms.push_back(ms.count());
//     }

//     g_success_count++;
//     g_total_bytes += stats.bytes;
//     close(sock);
// }

// int main()
// {
//     std::vector<std::thread> threads;
//     std::vector<ThreadStats> all_stats(CLIENT_COUNT); // 每个线程一份统计数据

//     std::cout << "--- Benchmark Starting ---" << std::endl;
//     std::cout << "Target: " << CLIENT_COUNT << " Clients x " << PACKETS_PER_CLIENT << " Requests" << std::endl;

//     auto start = std::chrono::high_resolution_clock::now();

//     for (int i = 0; i < CLIENT_COUNT; ++i)
//     {
//         threads.emplace_back(client_thread_func, i, std::ref(all_stats[i]));
//         if (i % 50 == 0)
//             std::this_thread::sleep_for(std::chrono::milliseconds(10));
//     }

//     for (auto &t : threads)
//     {
//         if (t.joinable())
//             t.join();
//     }

//     auto end = std::chrono::high_resolution_clock::now();
//     std::chrono::duration<double> diff = end - start;

//     // --- 数据汇总与分析 ---
//     std::vector<double> total_latencies;
//     total_latencies.reserve(CLIENT_COUNT * PACKETS_PER_CLIENT);
//     for (const auto &s : all_stats)
//     {
//         total_latencies.insert(total_latencies.end(), s.latencies_ms.begin(), s.latencies_ms.end());
//     }

//     // 排序以计算百分位
//     std::sort(total_latencies.begin(), total_latencies.end());

//     double avg_latency = 0;
//     double p50 = 0, p95 = 0, p99 = 0, max_lat = 0;

//     if (!total_latencies.empty())
//     {
//         double sum = std::accumulate(total_latencies.begin(), total_latencies.end(), 0.0);
//         avg_latency = sum / total_latencies.size();
//         p50 = total_latencies[total_latencies.size() * 0.50];
//         p95 = total_latencies[total_latencies.size() * 0.95];
//         p99 = total_latencies[total_latencies.size() * 0.99];
//         max_lat = total_latencies.back();
//     }

//     double total_mb = g_total_bytes / (1024.0 * 1024.0);
//     double mbps = total_mb / diff.count();

//     std::cout << "\n--- Benchmark Result ---" << std::endl;
//     std::cout << "Time Elapsed:    " << diff.count() << " s" << std::endl;
//     std::cout << "Total Requests:  " << total_latencies.size() << std::endl;
//     std::cout << "Concurrency:     " << CLIENT_COUNT << " threads" << std::endl;
//     std::cout << "Success/Fail:    " << g_success_count << " / " << g_fail_count << std::endl;
//     std::cout << "------------------------" << std::endl;
//     std::cout << "QPS:             " << std::fixed << std::setprecision(2) << (total_latencies.size() / diff.count()) << " req/s" << std::endl;
//     std::cout << "Throughput:      " << mbps << " MB/s" << std::endl;
//     std::cout << "------------------------" << std::endl;
//     std::cout << "Latency (RTT):" << std::endl;
//     std::cout << "  Avg: " << avg_latency << " ms" << std::endl;
//     std::cout << "  P50: " << p50 << " ms" << std::endl;
//     std::cout << "  P95: " << p95 << " ms" << std::endl;
//     std::cout << "  P99: " << p99 << " ms" << std::endl;
//     std::cout << "  Max: " << max_lat << " ms" << std::endl;

//     return 0;
// }