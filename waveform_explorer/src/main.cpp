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
    if (argc < 3) {
        print_usage();
        return 1;
    }

    std::string vcd_path = argv[1];
    std::string query_str = argv[2];

    WaveDatabase db;
    if (!db.load_vcd(vcd_path)) {
        std::cout << json({{"status", "error"}, {"message", "Failed to load VCD"}}) << std::endl;
        return 1;
    }

    AgentAPI api(db);
    json query;
    try {
        query = json::parse(query_str);
    } catch (const std::exception& e) {
        std::cout << json({{"status", "error"}, {"message", "Invalid JSON query"}}) << std::endl;
        return 1;
    }

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

    std::cout << response.dump(2) << std::endl;

    return 0;
}
