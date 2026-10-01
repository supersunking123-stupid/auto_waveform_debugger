// Opt-in source witnesses. This walk never substitutes a canonical body and
// never binds statements/expressions inside an inactive generate scope.
struct InactiveSourceSegment {
  std::string file, physical;
  uint32_t buffer = 0;
  size_t begin = 0, end = 0;
  int first_line = 0, last_line = 0;
  size_t first_physical_line = 0;
};

class InactiveSourceNamespaces {
 public:
  InactiveSourceNamespaces(const slang::SourceManager &sm, CompileContext &ctx) {
    std::unordered_map<std::string, std::string_view> physical_text;
    for (const auto buffer : sm.getAllBuffers()) {
      if (sm.getFullPath(buffer).empty()) continue; // Macro buffers have no file namespace.
      const auto text = sm.getSourceText(buffer);
      const std::string physical = GetAbsoluteSourcePath(slang::SourceLocation(buffer, 0), sm, ctx);
      auto [found, inserted] = physical_text.emplace(physical, text);
      if (!inserted && found->second != text) ambiguous_physical_.insert(physical);
      std::optional<InactiveSourceSegment> segment;
      size_t position = 0, physical_line = 1;
      while (position < text.size()) {
        const slang::SourceLocation loc(buffer, position);
        const std::string file = GetStoredSourcePath(loc, sm, ctx);
        const int line = GetStoredSourceLine(loc, sm, ctx);
        size_t next = text.find_first_of("\r\n", position);
        if (next == std::string_view::npos) next = text.size();
        else {
          const char terminator = text[next++];
          if (terminator == '\r' && next < text.size() && text[next] == '\n') ++next;
        }
        if (!segment || segment->file != file || line != segment->last_line + 1) {
          if (segment) segments_[segment->file].push_back(*segment);
          segment = InactiveSourceSegment{file, physical, buffer.getId(), position, next,
                                          line, line, physical_line};
        } else { segment->end = next; segment->last_line = line; }
        position = next; ++physical_line;
      }
      if (segment) segments_[segment->file].push_back(*segment);
    }
  }
  bool Unambiguous(const std::string &file, const std::string &physical) const {
    const auto found = segments_.find(file);
    if (found == segments_.end() || ambiguous_physical_.contains(physical)) return false;
    for (const auto &segment : found->second) if (segment.physical != physical) return false;
    return true;
  }
  bool ExclusiveLines(const std::string &file, const std::string &physical, uint32_t buffer,
                      size_t begin, size_t end, int first, int last) const {
    if (first <= 0 || last <= first || !Unambiguous(file, physical)) return false;
    const auto &segments = segments_.at(file);
    const InactiveSourceSegment *own = nullptr;
    for (const auto &segment : segments)
      if (segment.buffer == buffer && segment.begin <= begin && end <= segment.end &&
          segment.first_line <= first && last <= segment.last_line) { own = &segment; break; }
    if (!own) return false; // Includes directive changes within this source span.
    const int64_t delta = int64_t(own->first_physical_line) - own->first_line;
    for (const auto &segment : segments) {
      if (segment.last_line <= first || segment.first_line >= last) continue;
      if (segment.physical != physical || int64_t(segment.first_physical_line) - segment.first_line != delta)
        return false; // Another physical line has the same stored logical line.
    }
    return true;
  }
 private:
  std::unordered_map<std::string, std::vector<InactiveSourceSegment>> segments_;
  std::unordered_set<std::string> ambiguous_physical_;
};

bool WriteInactiveScopeManifest(const slang::ast::RootSymbol &root,
                                const slang::SourceManager &sm, CompileContext &ctx,
                                const std::string &path) {
  if (!std::filesystem::path(path).is_absolute()) return false;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  const InactiveSourceNamespaces namespaces(sm, ctx);
  auto quoted = [](std::string_view value) { return "\"" + JsonEscape(value) + "\""; };
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (ec) return false;
  out << "{\"record\":\"header\",\"schema\":1,\"semantics_epoch\":9,\"source_path_mode\":"
      << quoted(ctx.source_path_mode == SourcePathMode::kPhysicalAbsolute ? "physical_absolute" : "logical")
      << ",\"cwd\":" << quoted(cwd.string()) << ",\"coverage\":\"first inactive generate ancestor in every actual active instance\",\"top_instances\":[";
  for (size_t i = 0; i < root.topInstances.size(); ++i) {
    if (i) out << ',';
    out << quoted(root.topInstances[i]->getHierarchicalPath());
  }
  out << "]}\n";
  size_t instances = 0, inactive = 0, invalid = 0, cross_buffer = 0;
  auto emit = [&](const slang::ast::GenerateBlockSymbol &block, const slang::ast::InstanceSymbol &owner) {
    ++inactive;
    const auto *syntax = block.getSyntax();
    const slang::SourceRange range = syntax ? syntax->sourceRange() : slang::SourceRange{};
    const bool valid_range = range.start().valid() && range.end().valid();
    const slang::SourceRange original = valid_range ? sm.getFullyOriginalRange(range) : slang::SourceRange{};
    const auto start = original.start(), end = original.end();
    const bool same_buffer = start.valid() && end.valid() && start.buffer() == end.buffer();
    const bool valid_original = same_buffer && start.offset() <= end.offset() &&
        end.offset() <= sm.getSourceText(start.buffer()).size() &&
        end.offset() <= std::numeric_limits<uint32_t>::max();
    if (!valid_original) ++invalid;
    if (start.valid() && end.valid() && !same_buffer) ++cross_buffer;
    const std::string stored_first = valid_range ? GetStoredSourcePath(range.start(), sm, ctx) : "";
    const std::string stored_last = valid_range ? GetStoredSourcePath(range.end(), sm, ctx) : "";
    const int first_line = valid_range ? GetStoredSourceLine(range.start(), sm, ctx) : 0;
    const int last_line = valid_range ? GetStoredSourceLine(range.end(), sm, ctx) : 0;
    const std::string physical = valid_original ? GetAbsoluteSourcePath(start, sm, ctx) : "";
    const bool unambiguous = valid_original && !physical.empty() && namespaces.Unambiguous(stored_first, physical);
    const bool exclusive = unambiguous && stored_first == stored_last &&
        namespaces.ExclusiveLines(stored_first, physical, start.buffer().getId(),
                                   start.offset(), end.offset(), first_line, last_line);
    out << "{\"record\":\"inactive_scope\",\"scope_path\":" << quoted(block.getHierarchicalPath())
        << ",\"owner_instance\":" << quoted(owner.getHierarchicalPath())
        << ",\"definition\":" << quoted(owner.getDefinition().name)
        << ",\"is_uninstantiated\":true,\"construct_index\":" << block.constructIndex
        << ",\"array_index\":" << (block.arrayIndex ? quoted(block.arrayIndex->toString()) : "null")
        << ",\"syntax_kind\":\"GenerateBlock\",\"syntax_node_kind\":";
    if (syntax) out << static_cast<uint32_t>(syntax->kind); else out << "null";
    out << ",\"stored_start\":{\"file\":" << quoted(stored_first) << ",\"line\":" << first_line
        << "},\"stored_end\":{\"file\":" << quoted(stored_last) << ",\"line\":" << last_line
        << "},\"same_original_buffer\":" << (same_buffer ? "true" : "false")
        << ",\"logical_range_exclusive\":" << (exclusive ? "true" : "false") << ",\"original_span\":";
    if (valid_original && !physical.empty()) {
      out << "{\"file\":" << quoted(physical) << ",\"raw_file\":" << quoted(sm.getRawFileName(start.buffer()))
          << ",\"buffer_id\":" << start.buffer().getId()
          << ",\"begin\":" << start.offset() << ",\"end\":" << end.offset()
          << ",\"endpoint_file\":" << quoted(stored_first)
          << ",\"namespace_unambiguous\":" << (unambiguous ? "true" : "false") << '}';
    } else out << "null";
    out << "}\n";
  };
  auto walk = [&](auto &self, const slang::ast::Symbol &symbol,
                  const slang::ast::InstanceSymbol *owner) -> void {
    if (const auto *instance = symbol.as_if<slang::ast::InstanceSymbol>()) {
      ++instances;
      for (const auto &child : instance->body.members()) self(self, child, instance);
    } else if (const auto *array = symbol.as_if<slang::ast::InstanceArraySymbol>()) {
      for (const auto *entry : array->elements) if (entry) self(self, *entry, owner);
    } else if (const auto *block = symbol.as_if<slang::ast::GenerateBlockSymbol>()) {
      if (block->isUninstantiated) { if (owner) emit(*block, *owner); }
      else for (const auto &child : block->members()) self(self, child, owner);
    } else if (const auto *array = symbol.as_if<slang::ast::GenerateBlockArraySymbol>()) {
      for (const auto *entry : array->entries) if (entry) self(self, *entry, owner);
    }
  };
  for (const auto *top : root.topInstances) walk(walk, *top, nullptr);
  out.flush();
  if (!out) return false;
  out << "{\"record\":\"footer\",\"visited_actual_instances\":" << instances
      << ",\"inactive_scopes\":" << inactive << ",\"invalid_original_ranges\":" << invalid
      << ",\"cross_buffer_ranges\":" << cross_buffer
      << ",\"write_ok\":true,\"close_checked_by_compile_exit\":true}\n";
  out.close();
  return !out.fail();
}
