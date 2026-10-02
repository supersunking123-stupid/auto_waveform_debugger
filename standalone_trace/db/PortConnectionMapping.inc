// Shared bound-AST relation used by the baseline and canonical port tracers.
// Offsets are physical packed bits. Unpacked logical axes stay separate.
int64_t PortPackedWidth(const slang::ast::Type &type) {
  if (!type.isIntegral()) return 0;
  const auto width = type.getBitWidth();
  return width <= uint64_t(std::numeric_limits<int32_t>::max()) ? int64_t(width) : 0;
}
std::optional<int32_t> PortEvalIndex(const slang::ast::Expression &expr,
                                     const slang::ast::Symbol &scope) {
  slang::ast::EvalContext context(scope);
  const auto v = expr.eval(context);
  if (!v || !v.isInteger()) return std::nullopt;
  if (v.integer().hasUnknown() || (!v.integer().isSigned() && v.integer().getActiveBits() > 31))
    return std::nullopt;
  return v.integer().as<int32_t>();
}
PortStaticRef PortResolveReference(const slang::ast::Expression &expr, size_t depth = 0) {
  using namespace slang::ast;
  if (depth >= kPortMapMaxDepth) return {};
  if (const auto *n = expr.as_if<NamedValueExpression>()) {
    if (!IsTraceable(&n->symbol)) return {};
    return {&n->symbol, expr.type, 0, {}, true};
  }
  if (const auto *m = expr.as_if<MemberAccessExpression>()) {
    auto r = PortResolveReference(m->value(), depth + 1);
    const auto *field = m->member.as_if<FieldSymbol>();
    if (!r.valid || field == nullptr || !m->value().type->isIntegral()) return {};
    if (r.low + int64_t(field->bitOffset) > std::numeric_limits<int32_t>::max()) return {};
    r.low += field->bitOffset; r.type = expr.type; return r;
  }
  const Expression *value = nullptr;
  if (const auto *s = expr.as_if<ElementSelectExpression>()) value = &s->value();
  if (const auto *s = expr.as_if<RangeSelectExpression>()) value = &s->value();
  if (value == nullptr) return {};
  auto r = PortResolveReference(*value, depth + 1);
  if (!r.valid || !value->type->hasFixedRange()) return {};
  const auto declared = value->type->getFixedRange();
  if (value->type->isUnpackedArray()) {
    const auto *element = expr.as_if<ElementSelectExpression>();
    const auto selected = element ? PortEvalIndex(element->selector(), *r.symbol) : std::nullopt;
    if (!selected || !declared.containsPoint(*selected)) return {};
    r.logical_axes.emplace_back(*selected, *selected);
    r.type = expr.type; return r;
  }
  if (!value->type->isIntegral()) return {};
  slang::ast::EvalContext context(*r.symbol);
  const auto selected = expr.evalSelector(context, false);
  if (!selected) return {};
  // Slang already translates packed declarations (and element strides) to
  // physical LSB offsets. Translating them a second time breaks ascending axes.
  const int64_t delta = std::min(selected->left, selected->right);
  const int64_t high = std::max(selected->left, selected->right);
  if (delta < 0 || high >= PortPackedWidth(*value->type) ||
      r.low + delta > std::numeric_limits<int32_t>::max()) return {};
  r.low += delta; r.type = expr.type; return r;
}
void PortNormalizeCoverage(PortCoverage &ranges) {
  for (auto &r : ranges) if (r.first > r.second) std::swap(r.first, r.second);
  std::sort(ranges.begin(), ranges.end());
  size_t used = 0;
  for (const auto r : ranges) {
    if (used && int64_t(r.first) <= int64_t(ranges[used - 1].second) + 1)
      ranges[used - 1].second = std::max(ranges[used - 1].second, r.second);
    else ranges[used++] = r;
  }
  ranges.resize(used);
}
PortCoverage PortProjectionCoverage(const PortProjection &projection) {
  PortCoverage out;
  for (const auto &p : projection.pieces)
    out.emplace_back(int32_t(p.owner_low), int32_t(p.owner_low + p.width - 1));
  PortNormalizeCoverage(out); return out;
}
std::vector<PortMapPiece> PortBuildExpressionMap(const slang::ast::Expression &expr,
                                                 const slang::ast::Symbol &scope,
                                                 size_t depth = 0) {
  using namespace slang::ast;
  const int64_t width = PortPackedWidth(*expr.type);
  if (width <= 0) return {};
  auto marker = [&](PortMapPiece::Kind kind) {
    return std::vector<PortMapPiece>{{kind, nullptr, &expr, 0, 0, width, {}}};
  };
  if (depth >= kPortMapMaxDepth) return marker(PortMapPiece::Unsupported);
  if (const auto *a = expr.as_if<AssignmentExpression>())
    return PortBuildExpressionMap(a->left(), scope, depth + 1);
  slang::ast::EvalContext context(scope);
  const auto constant = expr.eval(context);
  if (constant && constant.isInteger()) return marker(PortMapPiece::Constant);
  if (const auto *c = expr.as_if<ConversionExpression>()) {
    const int64_t old_width = PortPackedWidth(*c->operand().type);
    if (old_width <= 0) return marker(PortMapPiece::Unsupported);
    auto pieces = PortBuildExpressionMap(c->operand(), scope, depth + 1);
    for (auto &p : pieces) p.width = std::min(p.width, std::max<int64_t>(0, width - p.owner_low));
    std::erase_if(pieces, [](const auto &p) { return p.width == 0; });
    if (width > old_width) pieces.push_back({c->operand().type->isSigned() ?
        PortMapPiece::Unsupported : PortMapPiece::Constant, nullptr, &expr, old_width, 0,
        width - old_width, {}});
    return pieces;
  }
  if (const auto *c = expr.as_if<ConcatenationExpression>()) {
    std::vector<PortMapPiece> out;
    int64_t low = 0;
    const auto operands = c->operands();
    for (size_t i = operands.size(); i-- > 0;) {
      const int64_t operand_width = PortPackedWidth(*operands[i]->type);
      if (operand_width <= 0 || low + operand_width > width) return marker(PortMapPiece::Unsupported);
      auto pieces = PortBuildExpressionMap(*operands[i], scope, depth + 1);
      for (auto &p : pieces) { p.owner_low += low; out.push_back(std::move(p)); }
      if (out.size() > kPortMapMaxPieces) return marker(PortMapPiece::Unsupported);
      low += operand_width;
    }
    if (low != width) return marker(PortMapPiece::Unsupported);
    return out;
  }
  if (const auto *u = expr.as_if<UnaryExpression>()) {
    if (u->op == UnaryOperator::BitwiseNot && PortPackedWidth(*u->operand().type) == width)
      return PortBuildExpressionMap(u->operand(), scope, depth + 1);
    return marker(PortMapPiece::Unsupported);
  }
  const auto reference = PortResolveReference(expr);
  if (!reference.valid || !reference.type->isIntegral() ||
      reference.low + width > std::numeric_limits<int32_t>::max()) return marker(PortMapPiece::Unsupported);
  return {{PortMapPiece::Exact, reference.symbol, &expr, 0, reference.low, width,
           reference.logical_axes}};
}
const slang::ast::Expression *PortActualConnection(const slang::ast::InstanceSymbol &instance,
                                                   const slang::ast::PortSymbol &port) {
  for (const auto *connection : instance.getPortConnections()) {
    if (connection->port.as_if<slang::ast::PortSymbol>() != &port) continue;
    const auto *expr = connection->getExpression();
    if (expr) if (const auto *assignment = expr->as_if<slang::ast::AssignmentExpression>())
      return &assignment->left();
    return expr;
  }
  return nullptr;
}
const std::vector<PortMapPiece> &GetCachedPortConnectionMap(const slang::ast::Expression &expression,
    const slang::ast::InstanceSymbol &instance, TraceCompileCache &cache) {
  auto &body = GetOrCreateBodyTraceCache(instance.body, cache);
  auto found = body.port_maps.find(&expression);
  if (found == body.port_maps.end())
    found = body.port_maps.emplace(&expression, PortBuildExpressionMap(expression, instance)).first;
  return found->second;
}
PortProjection PortIdentityProjection(const slang::ast::Symbol *symbol, const CanonFrame *frame = nullptr) {
  PortProjection p; p.owner = symbol; p.owner_frame = frame;
  const auto *value = symbol->as_if<slang::ast::ValueSymbol>();
  const auto *type = value ? &value->getType() : nullptr;
  const bool integral_owner = type && type->isIntegral();
  while (type && type->isUnpackedArray()) type = type->getArrayElementType();
  const int64_t width = type ? PortPackedWidth(*type) : 0;
  if (width > 0) p.pieces.push_back({integral_owner ? PortMapPiece::Exact : PortMapPiece::Unsupported,
                                   symbol, nullptr, 0, 0, width, {}});
  return p;
}
// Compose a root->current projection with a formal-current->source connection.
std::vector<PortProjection> PortComposeUp(const PortProjection &current,
                                         const std::vector<PortMapPiece> &connection) {
  std::vector<PortProjection> routes;
  for (const auto &a : current.pieces) for (const auto &b : connection) {
    if (routes.size() > kPortMapMaxPieces) return routes;
    if (a.kind != PortMapPiece::Exact) {
      PortProjection route; route.owner = current.owner; route.owner_frame = current.owner_frame;
      auto piece = a; piece.kind = PortMapPiece::Unsupported;
      route.pieces.push_back(std::move(piece)); routes.push_back(std::move(route));
      continue;
    }
    if (!a.logical_axes.empty()) continue; // unsupported unpacked formal ports stop at the caller
    const int64_t low = std::max(a.source_low, b.owner_low);
    const int64_t high = std::min(a.source_low + a.width, b.owner_low + b.width);
    if (low >= high) continue;
    PortProjection route; route.owner = current.owner; route.owner_frame = current.owner_frame;
    auto p = b; p.owner_low = a.owner_low + low - a.source_low;
    p.source_low += low - b.owner_low; p.width = high - low;
    route.pieces.push_back(std::move(p)); routes.push_back(std::move(route));
  }
  return routes;
}
// Invert the source slices of a child connection while retaining root ownership.
PortProjection PortComposeDown(const PortProjection &current,
                               const std::vector<PortMapPiece> &connection,
                               const slang::ast::Symbol *child) {
  PortProjection out; out.owner = current.owner; out.owner_frame = current.owner_frame;
  for (const auto &a : current.pieces) for (const auto &b : connection) {
    if (a.kind != PortMapPiece::Exact || b.kind != PortMapPiece::Exact ||
        b.source != a.source || b.logical_axes != a.logical_axes) continue;
    const int64_t low = std::max(a.source_low, b.source_low);
    const int64_t high = std::min(a.source_low + a.width, b.source_low + b.width);
    if (low >= high) continue;
    out.pieces.push_back({PortMapPiece::Exact, child, b.expr, a.owner_low + low - a.source_low,
                         b.owner_low + low - b.source_low, high - low, {}});
    if (out.pieces.size() > kPortMapMaxPieces) { out.pieces.clear(); return out; }
  }
  return out;
}
std::optional<PortCoverage> PortLocalCoverage(const PortProjection &projection,
                                             const ExprTraceResult &entry) {
  const auto *expr = entry.selectors.empty() ? entry.expr : entry.selectors.front();
  if (expr == nullptr) return std::nullopt;
  const auto reference = PortResolveReference(*expr);
  if (!reference.valid || !reference.type->isIntegral()) return std::nullopt;
  const int64_t width = PortPackedWidth(*reference.type);
  if (width <= 0) return std::nullopt;
  PortCoverage coverage;
  for (const auto &p : projection.pieces) {
    if (p.source != reference.symbol || p.logical_axes != reference.logical_axes) continue;
    const int64_t low = std::max(p.source_low, reference.low);
    const int64_t high = std::min(p.source_low + p.width, reference.low + width);
    if (low < high) coverage.emplace_back(int32_t(p.owner_low + low - p.source_low),
                                         int32_t(p.owner_low + high - p.source_low - 1));
  }
  PortNormalizeCoverage(coverage); return coverage;
}
MappedPortTraceResult PortMappingMarker(const PortProjection &projection,
                                         const slang::ast::PortSymbol *port,
                                         bool constant, bool unresolved) {
  return {port, PortProjectionCoverage(projection), constant, unresolved, projection.owner};
}
