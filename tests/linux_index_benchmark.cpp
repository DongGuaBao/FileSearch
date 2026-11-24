#include "linux_name_index.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>
int main(int argc, char** argv) {
    const auto count = argc > 1 ? std::stoull(argv[1]) : 100000;
    if (count < 100 || count > 2000000) throw std::runtime_error("benchmark size outside bounded range");
    fsengine::LinuxNameIndex index;
    auto path = [](std::size_t n) { return "/data/projects/benchmark/deep/subdirectory/report_" + std::to_string(n) + ".txt"; };
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t n = 0; n < count; ++n) index.Upsert(path(n), "report_" + std::to_string(n) + ".txt");
    const auto built = std::chrono::steady_clock::now();
    const auto query = "report_" + std::to_string(count / 2) + ".txt";
    for (int repetition = 0; repetition < 100; ++repetition)
        if (index.Candidates(query).size() != 1) throw std::runtime_error("selective query mismatch");
    const auto searched = std::chrono::steady_clock::now();
    if (index.Candidates(".txt").size() != count) throw std::runtime_error("broad query mismatch");
    const auto broad = std::chrono::steady_clock::now();
    for (std::size_t n = 0; n < count / 10; ++n) index.Erase(path(n));
    if (!index.Candidates("report_0.txt").empty()) throw std::runtime_error("deleted entry remains");
    rusage usage{}; getrusage(RUSAGE_SELF, &usage);
    auto milliseconds = [](auto duration) { return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count(); };
    std::cout << "{\"entries\":" << count << ",\"build_ms\":" << milliseconds(built-start)
        << ",\"selective_100_queries_ms\":" << milliseconds(searched-built)
        << ",\"selective_100_queries_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(searched-built).count()
        << ",\"broad_query_ms\":" << milliseconds(broad-searched)
        << ",\"peak_rss_kib\":" << usage.ru_maxrss << "}\n";
}
