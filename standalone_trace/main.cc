#include "db/GraphDb.h"
#include <iostream>
#include <string>

using namespace rtl_trace;

static void PrintGeneralHelp() {
  std::cout << "Usage: rtl_trace <subcommand> [options]\n\n";
  std::cout << "Subcommands:\n";
  std::cout << "  compile           Compile RTL sources into a trace database\n";
  std::cout << "  trace             Trace signal drivers/loads\n";
  std::cout << "  hier              Show design hierarchy\n";
  std::cout << "  find              Search for signals\n";
  std::cout << "  whereis-instance  Locate instance source\n";
  std::cout << "  serve             Interactive serve mode\n";
  std::cout << "\nRun 'rtl_trace <subcommand> --help' for details.\n";
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    PrintGeneralHelp();
    return 1;
  }

  const std::string subcmd = argv[1];
  if (subcmd == "-h" || subcmd == "--help") {
    PrintGeneralHelp();
    return 0;
  }
  if (subcmd == "compile") {
    return RunCompile(argc - 2, argv + 2);
  }
  if (subcmd == "trace") {
    return RunTrace(argc - 2, argv + 2);
  }
  if (subcmd == "hier") {
    return RunHier(argc - 2, argv + 2);
  }
  if (subcmd == "whereis-instance") {
    return RunWhereInstance(argc - 2, argv + 2);
  }
  if (subcmd == "find") {
    return RunFind(argc - 2, argv + 2);
  }
  if (subcmd == "serve") {
    return RunServe(argc - 2, argv + 2);
  }

  std::cerr << "Unknown subcommand: " << subcmd << "\n";
  PrintGeneralHelp();
  return 1;
}
