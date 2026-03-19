#include "WaveDatabase.h"
#include "AgentAPI.h"
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void print_usage() {
    std::cerr << "Usage: wave_agent_cli <vcd_file> <json_query>" << std::endl;
    std::cerr << "Example: wave_agent_cli timer_tb.vcd '{\"cmd\": \"get_signal_info\", \"args\": {\"path\": \"TOP.timer_tb.clk\"}}'" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string vcd_path = argv[1];
    WaveDatabase db;
    if (!db.load(vcd_path)) {
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
        
        // Use a lambda or helper for dispatching logic to avoid duplication
        auto dispatch = [&](const json& q) {
            std::string cmd = q.value("cmd", "");
            json args = q.value("args", json::object());
            json response;

            if (cmd == "get_signal_info") {
                response = api.get_signal_info(args.value("path", ""));
            } else if (cmd == "get_snapshot") {
                response = api.get_snapshot(args.value("signals", std::vector<std::string>()), args.value("time", 0ULL));
            } else if (cmd == "get_value_at_time") {
                response = api.get_value_at_time(args.value("path", ""), args.value("time", 0ULL));
            } else if (cmd == "find_edge") {
                response = api.find_edge(args.value("path", ""), args.value("edge_type", "anyedge"), 
                                        args.value("start_time", 0ULL), args.value("direction", "forward"));
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
            
            // Re-use dispatch logic (duplicated here for simplicity since I can't easily refactor main easily in one replace)
            std::string cmd = query.value("cmd", "");
            json args = query.value("args", json::object());
            json response;

            if (cmd == "get_signal_info") {
                response = api.get_signal_info(args.value("path", ""));
            } else if (cmd == "get_snapshot") {
                response = api.get_snapshot(args.value("signals", std::vector<std::string>()), args.value("time", 0ULL));
            } else if (cmd == "get_value_at_time") {
                response = api.get_value_at_time(args.value("path", ""), args.value("time", 0ULL));
            } else if (cmd == "find_edge") {
                response = api.find_edge(args.value("path", ""), args.value("edge_type", "anyedge"), 
                                        args.value("start_time", 0ULL), args.value("direction", "forward"));
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
