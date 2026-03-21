#include "slang/ast/Compilation.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/types/DeclaredType.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/driver/Driver.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

enum class OutputFormat { kJson, kText };

struct ParamRecord {
  std::string name;
  std::string kind;
  std::string value;
};

struct ModuleVariantRecord {
  std::string uniq_module;
  std::string orig_module;
  std::vector<ParamRecord> parameters;
};

struct CliOptions {
  std::string top;
  OutputFormat format = OutputFormat::kJson;
  bool relax_defparam = false;
  std::vector<std::string> slang_args;
};

enum class ParseStatus { kOk, kHelp, kError };

std::string JsonEscape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (unsigned char ch : value) {
    switch (ch) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (ch < 0x20) {
        std::ostringstream os;
        os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
           << static_cast<unsigned int>(ch);
        out += os.str();
      } else {
        out.push_back(static_cast<char>(ch));
      }
      break;
    }
  }
  return out;
}

void PrintQuoted(std::ostream &os, std::string_view value) {
  os << '"' << JsonEscape(value) << '"';
}

std::string LeafName(const std::string &path) {
  const size_t pos = path.rfind('.');
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

uint64_t Fnv1a64(std::string_view text) {
  constexpr uint64_t kOffset = 14695981039346656037ull;
  constexpr uint64_t kPrime = 1099511628211ull;
  uint64_t hash = kOffset;
  for (unsigned char ch : text) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= kPrime;
  }
  return hash;
}

std::string ShortHex(uint64_t value) {
  std::ostringstream os;
  os << std::hex << std::nouppercase << std::setfill('0') << std::setw(8)
     << static_cast<uint32_t>(value & 0xffffffffu);
  return os.str();
}

bool HasTimescaleArg(const std::vector<std::string> &args) {
  for (const std::string &arg : args) {
    if (arg == "--timescale") return true;
    if (arg.rfind("--timescale=", 0) == 0) return true;
  }
  return false;
}

ParseStatus ParseCommandLine(int argc, char **argv, CliOptions &opts) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      return ParseStatus::kHelp;
    }
    if (arg == "--relax-defparam") {
      opts.relax_defparam = true;
      continue;
    }
    if (arg == "--format") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --format\n";
        return ParseStatus::kError;
      }
      arg = argv[++i];
      if (arg == "json") {
        opts.format = OutputFormat::kJson;
      } else if (arg == "text") {
        opts.format = OutputFormat::kText;
      } else {
        std::cerr << "Unsupported format: " << arg << "\n";
        return ParseStatus::kError;
      }
      continue;
    }
    if (arg.rfind("--format=", 0) == 0) {
      const std::string value = arg.substr(std::string("--format=").size());
      if (value == "json") {
        opts.format = OutputFormat::kJson;
      } else if (value == "text") {
        opts.format = OutputFormat::kText;
      } else {
        std::cerr << "Unsupported format: " << value << "\n";
        return ParseStatus::kError;
      }
      continue;
    }

    if (arg == "-top" || arg == "--top") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << arg << "\n";
        return ParseStatus::kError;
      }
      opts.top = argv[++i];
      opts.slang_args.emplace_back("--top");
      opts.slang_args.push_back(opts.top);
      continue;
    }
    if (arg.rfind("-top=", 0) == 0) {
      opts.top = arg.substr(std::string("-top=").size());
      opts.slang_args.emplace_back("--top=" + opts.top);
      continue;
    }
    if (arg.rfind("--top=", 0) == 0) {
      opts.top = arg.substr(std::string("--top=").size());
      opts.slang_args.push_back(arg);
      continue;
    }

    opts.slang_args.push_back(arg);
  }

  if (opts.top.empty()) {
    std::cerr << "Missing required option: -top <module>\n";
    return ParseStatus::kError;
  }
  if (!HasTimescaleArg(opts.slang_args)) {
    opts.slang_args.emplace_back("--timescale");
    opts.slang_args.emplace_back("1ns/1ps");
  }

  bool has_unknown_sys_name_control = false;
  for (const std::string &arg : opts.slang_args) {
    if (arg == "-Wno-unknown-sys-name" || arg.rfind("-Wno-unknown-sys-name=", 0) == 0 ||
        arg == "-Wunknown-sys-name" || arg.rfind("-Wunknown-sys-name=", 0) == 0) {
      has_unknown_sys_name_control = true;
      break;
    }
  }
  if (!has_unknown_sys_name_control) {
    opts.slang_args.emplace_back("-Wno-unknown-sys-name");
  }

  return ParseStatus::kOk;
}

std::vector<ParamRecord> CollectParameters(const slang::ast::InstanceSymbol &inst) {
  std::vector<ParamRecord> out;
  for (const slang::ast::ParameterSymbolBase *base : inst.body.getParameters()) {
    if (base == nullptr) continue;
    const slang::ast::Symbol &symbol = base->symbol;
    ParamRecord rec;
    rec.name = std::string(symbol.name);
    if (symbol.kind == slang::ast::SymbolKind::Parameter) {
      rec.kind = "value";
      rec.value = symbol.as<slang::ast::ParameterSymbol>().getValue().toString();
    } else if (symbol.kind == slang::ast::SymbolKind::TypeParameter) {
      rec.kind = "type";
      const auto &type_param = symbol.as<slang::ast::TypeParameterSymbol>();
      rec.value = type_param.targetType.getType().toString();
    } else {
      continue;
    }
    out.push_back(std::move(rec));
  }
  return out;
}

std::string BuildParameterSignature(const std::vector<ParamRecord> &params) {
  std::vector<std::string> fields;
  fields.reserve(params.size());
  for (const ParamRecord &param : params) {
    fields.push_back(param.name + "\n" + param.kind + "\n" + param.value);
  }
  std::sort(fields.begin(), fields.end());

  std::string sig;
  for (const std::string &field : fields) {
    sig += field;
    sig.push_back('\0');
  }
  return sig;
}

std::string BuildUniqueModuleName(const std::string &orig_module,
                                  const std::vector<ParamRecord> &params) {
  if (params.empty()) return orig_module;
  const std::string sig = BuildParameterSignature(params);
  return orig_module + "__P" + ShortHex(Fnv1a64(sig));
}

struct ModuleVariantBuilder {
  std::unordered_map<std::string, ModuleVariantRecord> variants;

  void Note(const std::string &uniq_module, const std::string &orig_module,
            const std::vector<ParamRecord> &params) {
    auto [it, inserted] = variants.try_emplace(uniq_module);
    if (inserted) {
      it->second.uniq_module = uniq_module;
      it->second.orig_module = orig_module;
      it->second.parameters = params;
    }
  }
};

bool IsHierarchyNode(const slang::ast::Symbol &symbol) {
  switch (symbol.kind) {
  case slang::ast::SymbolKind::Instance:
  case slang::ast::SymbolKind::InstanceArray:
  case slang::ast::SymbolKind::GenerateBlock:
  case slang::ast::SymbolKind::GenerateBlockArray:
    return true;
  default:
    return false;
  }
}

void PrintJsonParam(std::ostream &os, const ParamRecord &param) {
  os << "{";
  os << "\"name\":";
  PrintQuoted(os, param.name);
  os << ",\"kind\":";
  PrintQuoted(os, param.kind);
  os << ",\"value\":";
  PrintQuoted(os, param.value);
  os << "}";
}

void WriteJsonChildren(std::ostream &os, const slang::ast::Scope &scope, ModuleVariantBuilder &variants);

void WriteJsonNode(std::ostream &os, const slang::ast::Symbol &symbol, ModuleVariantBuilder &variants) {
  switch (symbol.kind) {
  case slang::ast::SymbolKind::Instance: {
    const auto &inst = symbol.as<slang::ast::InstanceSymbol>();
    const std::string path = inst.getHierarchicalPath();
    const std::string name = LeafName(path);
    const std::string orig_module = std::string(inst.getDefinition().name);
    const std::vector<ParamRecord> params = CollectParameters(inst);
    const std::string uniq_module = BuildUniqueModuleName(orig_module, params);
    variants.Note(uniq_module, orig_module, params);

    os << "{";
    os << "\"kind\":\"instance\"";
    os << ",\"name\":";
    PrintQuoted(os, name);
    os << ",\"path\":";
    PrintQuoted(os, path);
    os << ",\"orig_module\":";
    PrintQuoted(os, orig_module);
    os << ",\"uniq_module\":";
    PrintQuoted(os, uniq_module);
    os << ",\"parameters\":[";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i) os << ",";
      PrintJsonParam(os, params[i]);
    }
    os << "]";
    os << ",\"children\":[";
    WriteJsonChildren(os, inst.body, variants);
    os << "]}";
    return;
  }
  case slang::ast::SymbolKind::InstanceArray: {
    const auto &array = symbol.as<slang::ast::InstanceArraySymbol>();
    const std::string path = array.getHierarchicalPath();
    os << "{";
    os << "\"kind\":\"scope\"";
    os << ",\"name\":";
    PrintQuoted(os, LeafName(path));
    os << ",\"path\":";
    PrintQuoted(os, path);
    os << ",\"scope_kind\":\"instance_array\"";
    os << ",\"children\":[";
    bool first = true;
    for (const slang::ast::Symbol *elem : array.elements) {
      if (elem == nullptr) continue;
      if (!first) os << ",";
      WriteJsonNode(os, *elem, variants);
      first = false;
    }
    os << "]}";
    return;
  }
  case slang::ast::SymbolKind::GenerateBlock: {
    const auto &gen = symbol.as<slang::ast::GenerateBlockSymbol>();
    const std::string path = gen.getHierarchicalPath();
    std::string name = gen.getExternalName();
    if (name.empty()) name = LeafName(path);
    os << "{";
    os << "\"kind\":\"scope\"";
    os << ",\"name\":";
    PrintQuoted(os, name);
    os << ",\"path\":";
    PrintQuoted(os, path);
    os << ",\"scope_kind\":\"generate\"";
    os << ",\"children\":[";
    if (!gen.isUninstantiated) {
      WriteJsonChildren(os, gen, variants);
    }
    os << "]}";
    return;
  }
  case slang::ast::SymbolKind::GenerateBlockArray: {
    const auto &gen_array = symbol.as<slang::ast::GenerateBlockArraySymbol>();
    const std::string path = gen_array.getHierarchicalPath();
    std::string name = gen_array.getExternalName();
    if (name.empty()) name = LeafName(path);
    os << "{";
    os << "\"kind\":\"scope\"";
    os << ",\"name\":";
    PrintQuoted(os, name);
    os << ",\"path\":";
    PrintQuoted(os, path);
    os << ",\"scope_kind\":\"generate_array\"";
    os << ",\"children\":[";
    bool first = true;
    for (const slang::ast::GenerateBlockSymbol *entry : gen_array.entries) {
      if (entry == nullptr || entry->isUninstantiated) continue;
      if (!first) os << ",";
      WriteJsonNode(os, *entry, variants);
      first = false;
    }
    os << "]}";
    return;
  }
  default:
    std::cerr << "Internal error: unsupported hierarchy symbol kind\n";
    std::exit(2);
  }
}

void WriteJsonChildren(std::ostream &os, const slang::ast::Scope &scope, ModuleVariantBuilder &variants) {
  bool first = true;
  for (const slang::ast::Symbol &member : scope.members()) {
    if (!IsHierarchyNode(member)) continue;
    if (!first) os << ",";
    WriteJsonNode(os, member, variants);
    first = false;
  }
}

void PrintJson(std::ostream &os, std::span<const slang::ast::InstanceSymbol *const> roots,
               std::string_view top_name) {
  ModuleVariantBuilder variants;
  os << "{";
  os << "\"schema\":\"rtl_hier_v1\"";
  os << ",\"top\":";
  PrintQuoted(os, top_name);
  os << ",\"roots\":[";
  for (size_t i = 0; i < roots.size(); ++i) {
    if (i) os << ",";
    WriteJsonNode(os, *roots[i], variants);
  }
  os << "],\"module_variants\":[";

  std::vector<const ModuleVariantRecord *> ordered_variants;
  ordered_variants.reserve(variants.variants.size());
  for (const auto &[_, record] : variants.variants) {
    ordered_variants.push_back(&record);
  }
  std::sort(ordered_variants.begin(), ordered_variants.end(),
            [](const ModuleVariantRecord *a, const ModuleVariantRecord *b) {
              return a->uniq_module < b->uniq_module;
            });

  for (size_t i = 0; i < ordered_variants.size(); ++i) {
    if (i) os << ",";
    const ModuleVariantRecord &variant = *ordered_variants[i];
    os << "{";
    os << "\"uniq_module\":";
    PrintQuoted(os, variant.uniq_module);
    os << ",\"orig_module\":";
    PrintQuoted(os, variant.orig_module);
    os << ",\"parameters\":[";
    for (size_t j = 0; j < variant.parameters.size(); ++j) {
      if (j) os << ",";
      PrintJsonParam(os, variant.parameters[j]);
    }
    os << "]}";
  }
  os << "]}\n";
}

std::string FormatParametersInline(const std::vector<ParamRecord> &params) {
  if (params.empty()) return "{}";
  std::ostringstream os;
  os << "{";
  for (size_t i = 0; i < params.size(); ++i) {
    if (i) os << ", ";
    os << params[i].name << "=" << params[i].value;
  }
  os << "}";
  return os.str();
}

void PrintTextNode(std::ostream &os, const slang::ast::Symbol &symbol, size_t depth,
                   ModuleVariantBuilder &variants) {
  const std::string indent(depth * 2, ' ');
  switch (symbol.kind) {
  case slang::ast::SymbolKind::Instance: {
    const auto &inst = symbol.as<slang::ast::InstanceSymbol>();
    const std::string path = inst.getHierarchicalPath();
    const std::string orig_module = std::string(inst.getDefinition().name);
    const std::vector<ParamRecord> params = CollectParameters(inst);
    const std::string uniq_module = BuildUniqueModuleName(orig_module, params);
    variants.Note(uniq_module, orig_module, params);
    os << indent << "instance " << path << " : " << orig_module << " -> " << uniq_module
       << " params=" << FormatParametersInline(params) << "\n";
    for (const slang::ast::Symbol &member : inst.body.members()) {
      if (IsHierarchyNode(member)) PrintTextNode(os, member, depth + 1, variants);
    }
    return;
  }
  case slang::ast::SymbolKind::InstanceArray: {
    const auto &array = symbol.as<slang::ast::InstanceArraySymbol>();
    os << indent << "scope(instance_array) " << array.getHierarchicalPath() << "\n";
    for (const slang::ast::Symbol *elem : array.elements) {
      if (elem != nullptr) PrintTextNode(os, *elem, depth + 1, variants);
    }
    return;
  }
  case slang::ast::SymbolKind::GenerateBlock: {
    const auto &gen = symbol.as<slang::ast::GenerateBlockSymbol>();
    os << indent << "scope(generate) " << gen.getHierarchicalPath() << "\n";
    if (!gen.isUninstantiated) {
      for (const slang::ast::Symbol &member : gen.members()) {
        if (IsHierarchyNode(member)) PrintTextNode(os, member, depth + 1, variants);
      }
    }
    return;
  }
  case slang::ast::SymbolKind::GenerateBlockArray: {
    const auto &gen_array = symbol.as<slang::ast::GenerateBlockArraySymbol>();
    os << indent << "scope(generate_array) " << gen_array.getHierarchicalPath() << "\n";
    for (const slang::ast::GenerateBlockSymbol *entry : gen_array.entries) {
      if (entry != nullptr && !entry->isUninstantiated) {
        PrintTextNode(os, *entry, depth + 1, variants);
      }
    }
    return;
  }
  default:
    return;
  }
}

void PrintText(std::ostream &os, std::span<const slang::ast::InstanceSymbol *const> roots,
               std::string_view top_name) {
  os << "schema: rtl_hier_v1\n";
  os << "top: " << top_name << "\n";
  os << "roots: " << roots.size() << "\n";
  ModuleVariantBuilder variants;
  for (const slang::ast::InstanceSymbol *root : roots) {
    PrintTextNode(os, *root, 0, variants);
  }
  os << "module_variants: " << variants.variants.size() << "\n";
  std::vector<const ModuleVariantRecord *> ordered_variants;
  ordered_variants.reserve(variants.variants.size());
  for (const auto &[_, record] : variants.variants) {
    ordered_variants.push_back(&record);
  }
  std::sort(ordered_variants.begin(), ordered_variants.end(),
            [](const ModuleVariantRecord *a, const ModuleVariantRecord *b) {
              return a->uniq_module < b->uniq_module;
            });
  for (const ModuleVariantRecord *variant : ordered_variants) {
    os << "  " << variant->uniq_module << " <- " << variant->orig_module
       << " params=" << FormatParametersInline(variant->parameters) << "\n";
  }
}

void PrintUsage() {
  std::cout << "Usage: rtl_elab -top <module> [--format json|text] [--relax-defparam] "
               "[slang source args...]\n";
}

bool IsDefparamRelaxDiag(slang::DiagCode code) {
  const std::string_view name = slang::toString(code);
  if (name == "CouldNotResolveHierarchicalPath") return true;
  if (name.rfind("DefParam", 0) == 0 || name.rfind("Defparam", 0) == 0) return true;
  if (name == "VirtualIfaceDefparam") return true;
  return false;
}

bool IsDefparamContextDiagnostic(const slang::Diagnostic &diag, const slang::SourceManager &sm) {
  if (!diag.location.valid()) return false;
  const slang::SourceLocation loc = diag.location;
  const std::string_view text = sm.getSourceText(loc.buffer());
  if (loc.offset() >= text.size()) return false;
  size_t line_begin = text.rfind('\n', loc.offset());
  line_begin = (line_begin == std::string_view::npos) ? 0 : (line_begin + 1);
  size_t line_end = text.find('\n', loc.offset());
  if (line_end == std::string_view::npos) line_end = text.size();
  std::string line(text.substr(line_begin, line_end - line_begin));
  for (char &c : line) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return line.find("defparam") != std::string::npos;
}

bool HasBlockingCompileDiagnostics(slang::ast::Compilation &compilation, bool relax_defparam) {
  const slang::SourceManager &sm = *compilation.getSourceManager();
  for (const slang::Diagnostic &diag : compilation.getAllDiagnostics()) {
    const auto severity = slang::getDefaultSeverity(diag.code);
    if (severity != slang::DiagnosticSeverity::Error &&
        severity != slang::DiagnosticSeverity::Fatal) {
      continue;
    }
    if (relax_defparam &&
        (IsDefparamRelaxDiag(diag.code) || IsDefparamContextDiagnostic(diag, sm))) {
      continue;
    }
    return true;
  }
  return false;
}

} // namespace

int main(int argc, char **argv) {
  CliOptions parsed;
  const ParseStatus parse_status = ParseCommandLine(argc, argv, parsed);
  if (parse_status == ParseStatus::kHelp) {
    PrintUsage();
    return 0;
  }
  if (parse_status == ParseStatus::kError) {
    PrintUsage();
    return 1;
  }

  std::vector<std::string> driver_args;
  driver_args.reserve(parsed.slang_args.size() + 1);
  driver_args.emplace_back("rtl_elab");
  for (const std::string &arg : parsed.slang_args) {
    driver_args.push_back(arg);
  }

  std::vector<char *> driver_argv;
  driver_argv.reserve(driver_args.size());
  for (std::string &arg : driver_args) {
    driver_argv.push_back(arg.data());
  }

  slang::driver::Driver driver;
  driver.addStandardArgs();
  if (!driver.parseCommandLine(static_cast<int>(driver_argv.size()), driver_argv.data())) return 1;
  if (!driver.processOptions()) return 1;
  if (!driver.parseAllSources()) return 1;

  std::unique_ptr<slang::ast::Compilation> compilation = driver.createCompilation();
  driver.reportCompilation(*compilation, /*quiet*/ true);
  if (HasBlockingCompileDiagnostics(*compilation, parsed.relax_defparam)) {
    if (!driver.reportDiagnostics(/*quiet*/ true)) return 1;
  }

  const slang::ast::RootSymbol &root = compilation->getRoot();
  std::vector<const slang::ast::InstanceSymbol *> roots;
  for (const slang::ast::InstanceSymbol *top : root.topInstances) {
    if (top == nullptr) continue;
    if (top->name != parsed.top && top->getDefinition().name != parsed.top) continue;
    roots.push_back(top);
  }
  if (roots.empty()) {
    std::cerr << "Top module not found in elaborated design: " << parsed.top << "\n";
    return 1;
  }

  if (parsed.format == OutputFormat::kJson) {
    PrintJson(std::cout, roots, parsed.top);
  } else {
    PrintText(std::cout, roots, parsed.top);
  }
  return 0;
}
