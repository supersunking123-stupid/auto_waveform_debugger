#include "WaveDatabase.h"
#include "AgentAPI.h"
#include <iostream>
#include <thread>
#include <vector>
#include <unistd.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class StdoutLineFilter {
public:
    StdoutLineFilter() {
        int pipe_fd[2];
        if (pipe(pipe_fd) != 0) return;

        saved_stdout_ = dup(STDOUT_FILENO);
        if (saved_stdout_ < 0) {
            (void)close(pipe_fd[0]);
            (void)close(pipe_fd[1]);
            return;
        }

        read_fd_ = pipe_fd[0];
        write_fd_ = pipe_fd[1];
        if (dup2(write_fd_, STDOUT_FILENO) < 0) {
            (void)close(read_fd_);
            (void)close(write_fd_);
            read_fd_ = -1;
            write_fd_ = -1;
            (void)close(saved_stdout_);
            saved_stdout_ = -1;
            return;
        }

        worker_ = std::thread([this]() { run(); });
        active_ = true;
    }

    ~StdoutLineFilter() {
        if (!active_) return;

        std::cout.flush();
        (void)close(STDOUT_FILENO);
        if (write_fd_ >= 0) {
            (void)close(write_fd_);
            write_fd_ = -1;
        }

        if (worker_.joinable()) worker_.join();

        if (saved_stdout_ >= 0) {
            (void)dup2(saved_stdout_, STDOUT_FILENO);
            (void)close(saved_stdout_);
        }
    }

private:
    static bool should_drop_line(const std::string& line) {
        return line.rfind("FSDB Reader, Release", 0) == 0 ||
               line.rfind("(C) 1996 - ", 0) == 0 ||
               line.rfind("logDir = ", 0) == 0;
    }

    void write_all(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            const ssize_t n = write(saved_stdout_, s.data() + off, s.size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
    }

    void flush_line(std::string& line) {
        if (!should_drop_line(line)) {
            write_all(line);
            write_all("\n");
        }
        line.clear();
    }

    void run() {
        std::string line;
        line.reserve(4096);
        std::vector<char> buf(4096);

        while (true) {
            const ssize_t n = read(read_fd_, buf.data(), buf.size());
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; ++i) {
                const char c = buf[static_cast<size_t>(i)];
                if (c == '\n') {
                    flush_line(line);
                } else if (c != '\r') {
                    line.push_back(c);
                }
            }
        }

        if (!line.empty()) flush_line(line);
        if (read_fd_ >= 0) (void)close(read_fd_);
    }

    int read_fd_ = -1;
    int write_fd_ = -1;
    int saved_stdout_ = -1;
    bool active_ = false;
    std::thread worker_;
};

void print_usage() {
    std::cerr << "Usage: wave_agent_cli <waveform_file> <json_query>" << std::endl;
    std::cerr << "Example: wave_agent_cli timer_tb.vcd '{\"cmd\": \"get_signal_info\", \"args\": {\"path\": \"TOP.timer_tb.clk\"}}'" << std::endl;
}

int main(int argc, char* argv[]) {
    StdoutLineFilter stdout_filter;

    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string waveform_path = argv[1];
    WaveDatabase db;
    if (!db.load(waveform_path)) {
        std::cout << json({{"status", "error"}, {"message", "Failed to load waveform file"}}) << std::endl;
        return 1;
    }

    AgentAPI api(db);

    // If a second argument is provided, run in one-shot mode.
    if (argc >= 3) {
        std::string query_str = argv[2];
        json query;
        try {
            query = json::parse(query_str);
        } catch (const std::exception& e) {
            std::cout << json({{"status", "error"}, {"message", "Invalid JSON query"}}) << std::endl;
            return 1;
        }

        auto dispatch = [&](const json& q) {
            std::string cmd = q.value("cmd", "");
            json args = q.value("args", json::object());
            json response;

            if (cmd == "get_signal_info") {
                response = api.get_signal_info(args.value("path", ""));
            } else if (cmd == "list_signals_page") {
                response = api.list_signals_page(
                    args.value("prefix", ""),
                    args.value("cursor", ""),
                    args.value("limit", 1000ULL));
            } else if (cmd == "get_snapshot") {
                response = api.get_snapshot(
                    args.value("signals", std::vector<std::string>()),
                    args.value("time", 0ULL),
                    args.value("radix", "hex"));
            } else if (cmd == "get_value_at_time") {
                response = api.get_value_at_time(
                    args.value("path", ""),
                    args.value("time", 0ULL),
                    args.value("radix", "hex"));
            } else if (cmd == "find_edge") {
                response = api.find_edge(args.value("path", ""), args.value("edge_type", "anyedge"),
                                         args.value("start_time", 0ULL), args.value("direction", "forward"));
            } else if (cmd == "find_value_intervals") {
                response = api.find_value_intervals(
                    args.value("path", ""),
                    args.value("value", ""),
                    args.value("start_time", 0ULL),
                    args.value("end_time", 0ULL),
                    args.value("radix", "hex"));
            } else if (cmd == "find_condition") {
                response = api.find_condition(args.value("expression", ""), args.value("start_time", 0ULL), args.value("direction", "forward"));
            } else if (cmd == "get_transitions") {
                response = api.get_transitions(args.value("path", ""), args.value("start_time", 0ULL),
                                               args.value("end_time", 0ULL), args.value("max_limit", 50));
            } else if (cmd == "analyze_pattern") {
                response = api.analyze_pattern(args.value("path", ""), args.value("start_time", 0ULL), args.value("end_time", 0ULL));
            } else if (cmd == "list_signals") {
                json signals = json::array();
                for (const auto& pair : db.get_all_signals()) {
                    signals.push_back(pair.first);
                }
                response = {{"status", "success"}, {"data", signals}};
            } else {
                response = {{"status", "error"}, {"message", "Unknown command: " + cmd}};
            }
            return response;
        };

        std::cout << dispatch(query).dump() << std::endl;
        return 0;
    }

    // Otherwise, enter interactive server mode (read from stdin)
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line == "exit" || line == "quit") break;

        try {
            json query = json::parse(line);
            std::string cmd = query.value("cmd", "");
            json args = query.value("args", json::object());
            json response;

            if (cmd == "get_signal_info") {
                response = api.get_signal_info(args.value("path", ""));
            } else if (cmd == "list_signals_page") {
                response = api.list_signals_page(
                    args.value("prefix", ""),
                    args.value("cursor", ""),
                    args.value("limit", 1000ULL));
            } else if (cmd == "get_snapshot") {
                response = api.get_snapshot(
                    args.value("signals", std::vector<std::string>()),
                    args.value("time", 0ULL),
                    args.value("radix", "hex"));
            } else if (cmd == "get_value_at_time") {
                response = api.get_value_at_time(
                    args.value("path", ""),
                    args.value("time", 0ULL),
                    args.value("radix", "hex"));
            } else if (cmd == "find_edge") {
                response = api.find_edge(args.value("path", ""), args.value("edge_type", "anyedge"),
                                         args.value("start_time", 0ULL), args.value("direction", "forward"));
            } else if (cmd == "find_value_intervals") {
                response = api.find_value_intervals(
                    args.value("path", ""),
                    args.value("value", ""),
                    args.value("start_time", 0ULL),
                    args.value("end_time", 0ULL),
                    args.value("radix", "hex"));
            } else if (cmd == "find_condition") {
                response = api.find_condition(args.value("expression", ""), args.value("start_time", 0ULL), args.value("direction", "forward"));
            } else if (cmd == "get_transitions") {
                response = api.get_transitions(args.value("path", ""), args.value("start_time", 0ULL),
                                               args.value("end_time", 0ULL), args.value("max_limit", 50));
            } else if (cmd == "analyze_pattern") {
                response = api.analyze_pattern(args.value("path", ""), args.value("start_time", 0ULL), args.value("end_time", 0ULL));
            } else if (cmd == "list_signals") {
                json signals = json::array();
                for (const auto& pair : db.get_all_signals()) {
                    signals.push_back(pair.first);
                }
                response = {{"status", "success"}, {"data", signals}};
            } else {
                response = {{"status", "error"}, {"message", "Unknown command: " + cmd}};
            }

            std::cout << response.dump() << std::endl;
        } catch (const std::exception& e) {
            std::cout << json({{"status", "error"}, {"message", "Invalid JSON query: " + std::string(e.what())}}).dump() << std::endl;
        }
    }

    return 0;
}
