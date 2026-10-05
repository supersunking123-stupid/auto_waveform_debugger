// Shared bound-AST relation used by the baseline and canonical port tracers.
// Offsets are physical packed bits. Unpacked logical axes stay separate.
int64_t PortPackedWidth(const slang::ast::Type &type) {
  if (!type.isIntegral()) return 0;
  const auto width = type.getBitWidth();
  return width <= uint64_t(std::numeric_limits<int32_t>::max()) ? int64_t(width) : 0;
}
int64_t PortAxisOffset(int32_t left, int32_t right, int32_t index) {
  return left >= right ? int64_t(index) - right : int64_t(right) - index;
}
struct PortOwnerShape {
  std::vector<std::pair<int32_t, int32_t>> unpacked;
  int64_t packed_width = 0, total_width = 0;
};
std::optional<PortOwnerShape> PortFixedOwnerShape(const slang::ast::Type &root) {
  PortOwnerShape shape;
  const auto *type = &root;
  while (type->isUnpackedArray()) {
    if (!type->hasFixedRange() || shape.unpacked.size() >= kPortMapMaxDepth) return std::nullopt;
    const auto range = type->getFixedRange();
    shape.unpacked.emplace_back(range.left, range.right);
    type = type->getArrayElementType();
    if (!type) return std::nullopt;
  }
  shape.total_width = shape.packed_width = PortPackedWidth(*type);
  if (!shape.packed_width) return std::nullopt;
  for (const auto [left, right] : shape.unpacked) {
    const int64_t count = std::abs(int64_t(left) - right) + 1;
    if (count > std::numeric_limits<int32_t>::max() / shape.total_width) return std::nullopt;
    shape.total_width *= count;
  }
  return shape;
}
// Save the actual affine connection only on standalone compact Port records.
// The target frame is separate from the child's public Port identity.
std::optional<CompactPortTraceResult> PortCompactRoute(
    const slang::ast::PortSymbol *port, const slang::ast::Symbol *owner,
    const std::vector<PortMapPiece> &map, const CanonFrame *target_frame = nullptr,
    const slang::flat_hash_map<const slang::ast::Symbol *,uint32_t> *known = nullptr) {
  const auto *formal = owner->as_if<slang::ast::ValueSymbol>();
  const int64_t width = formal ? PortPackedWidth(formal->getType()) : 0;
  if (!width || map.empty() || map.size() > kPortMapMaxPieces) return std::nullopt;
  CompactPortTraceResult result{port, {}, target_frame};
  for (const auto &p : map) {
    const auto *value = p.source ? p.source->as_if<slang::ast::ValueSymbol>() : nullptr;
    const auto shape = value ? PortFixedOwnerShape(value->getType()) : std::nullopt;
    if (p.kind != PortMapPiece::Exact || !shape || !known || !known->contains(p.source) ||
        p.width <= 0 || p.owner_low < 0 ||
        p.owner_low + p.width > width || p.source_low < 0 ||
        p.source_low + p.width > shape->packed_width ||
        p.logical_axes.size() != shape->unpacked.size()) return std::nullopt;
    int64_t target = p.source_low, stride = shape->packed_width;
    for (size_t i = shape->unpacked.size(); i-- > 0;) {
      const auto [left, right] = shape->unpacked[i];
      const auto [a,b] = p.logical_axes[i];
      const int64_t ordinal = PortAxisOffset(left,right,a);
      const int64_t count = std::abs(int64_t(left)-right)+1;
      if (a != b || ordinal < 0 || ordinal >= count) return std::nullopt;
      target += ordinal * stride; stride *= count;
    }
    if (target < 0 || target + p.width > shape->total_width) return std::nullopt;
    result.routes.push_back({int32_t(p.owner_low), int32_t(p.owner_low+p.width-1),
                             int32_t(target), p.source});
  }
  std::sort(result.routes.begin(),result.routes.end(),[](const auto &a,const auto &b) {
    return a.owner_low < b.owner_low;
  });
  int64_t next = 0;
  std::vector<CompactPortSourceRoute> normalized;
  for (const auto &p : result.routes) {
    if (p.owner_low != next) return std::nullopt;
    next = int64_t(p.owner_high)+1;
    if (!normalized.empty() && normalized.back().target == p.target &&
        int64_t(normalized.back().target_low)-normalized.back().owner_low ==
        int64_t(p.target_low)-p.owner_low) normalized.back().owner_high = p.owner_high;
    else normalized.push_back(p);
  }
  if (next != width) return std::nullopt;
  result.routes = std::move(normalized); return result;
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
std::optional<int32_t> PortKnownBoolean(const slang::ast::Expression &expr,
                                      const slang::ast::Symbol &scope) {
  slang::ast::EvalContext context(scope);
  const auto value = expr.eval(context);
  if (!value || !value.isInteger() || value.integer().hasUnknown()) return std::nullopt;
  const auto integer = value.integer().as<int32_t>();
  return integer && (*integer == 0 || *integer == 1) ? integer : std::nullopt;
}
bool PortCollectBooleanRefs(const slang::ast::Expression &expr, const slang::ast::Symbol &scope,
                            std::vector<PortMapPiece> &out, size_t depth = 0) {
  using namespace slang::ast;
  if (depth >= kPortMapMaxDepth || out.size() >= kPortMapMaxPieces) return false;
  if (PortKnownBoolean(expr, scope)) return true;
  if (PortPackedWidth(*expr.type) != 1) return false;
  if (const auto *conversion = expr.as_if<ConversionExpression>())
    return PortCollectBooleanRefs(conversion->operand(), scope, out, depth + 1);
  if (const auto *binary = expr.as_if<BinaryExpression>()) {
    if (binary->op != BinaryOperator::LogicalAnd && binary->op != BinaryOperator::LogicalOr) return false;
    const auto left = PortKnownBoolean(binary->left(), scope), right = PortKnownBoolean(binary->right(), scope);
    if ((binary->op == BinaryOperator::LogicalAnd && (left == 0 || right == 0)) ||
        (binary->op == BinaryOperator::LogicalOr && (left == 1 || right == 1))) return true;
    return PortCollectBooleanRefs(binary->left(), scope, out, depth + 1) &&
           PortCollectBooleanRefs(binary->right(), scope, out, depth + 1);
  }
  if (const auto *unary = expr.as_if<UnaryExpression>()) {
    if (unary->op != UnaryOperator::LogicalNot) return false;
    return PortCollectBooleanRefs(unary->operand(), scope, out, depth + 1);
  }
  if (const auto *conditional = expr.as_if<ConditionalExpression>()) {
    if (const auto *side = conditional->knownSide()) return PortCollectBooleanRefs(*side, scope, out, depth + 1);
    if (conditional->conditions.size() != 1 || conditional->conditions[0].pattern) return false;
    const auto left = PortKnownBoolean(conditional->left(), scope), right = PortKnownBoolean(conditional->right(), scope);
    if (left && right && left == right) return true;
    return PortCollectBooleanRefs(*conditional->conditions[0].expr, scope, out, depth + 1) &&
           PortCollectBooleanRefs(conditional->left(), scope, out, depth + 1) &&
           PortCollectBooleanRefs(conditional->right(), scope, out, depth + 1);
  }
  const auto reference = PortResolveReference(expr);
  if (!reference.valid || PortPackedWidth(*reference.type) != 1) return false;
  out.push_back({PortMapPiece::Dependency, reference.symbol, &expr, 0, reference.low, 1,
                 reference.logical_axes});
  return true;
}
PortCoverage PortProjectionCoverage(const PortProjection &projection) {
  PortCoverage out;
  for (const auto &p : projection.pieces)
    out.emplace_back(int32_t(p.owner_low), int32_t(p.owner_low + p.width - 1));
  PortNormalizeCoverage(out); return out;
}
bool PortHasUnprovedDependency(const std::vector<PortMapPiece> &map,
                              const slang::ast::Symbol *symbol) {
  for (const auto &piece : map) if (piece.kind == PortMapPiece::Unsupported) {
    if (!piece.expr) return true;
    bool referenced = false;
    piece.expr->visitSymbolReferences([&](const auto &, const auto &source) {
      referenced |= &source == symbol;
    });
    if (referenced) return true;
  }
  return false;
}
// Deduplicate repeated failures of the same transient route, not different
// failed chunks. Native possible-source bounds and owner coverage both matter.
std::string PortFallbackKey(const PortProjection &projection) {
  std::string key;
  for (const auto &p : projection.pieces) {
    key += ";" + std::to_string(p.kind) + ":" + std::to_string(p.owner_low) +
           ":" + std::to_string(p.source_low) + ":" + std::to_string(p.width) +
           ":" + std::to_string(reinterpret_cast<uintptr_t>(p.source)) +
           ":" + std::to_string(reinterpret_cast<uintptr_t>(p.expr));
    for (const auto &axis : p.logical_axes)
      key += "[" + std::to_string(axis.first) + ":" + std::to_string(axis.second) + "]";
  }
  return key;
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
  if (const auto *conditional = expr.as_if<ConditionalExpression>()) {
    if (const auto *side = conditional->knownSide()) return PortBuildExpressionMap(*side, scope, depth + 1);
    if (conditional->conditions.size() == 1 && !conditional->conditions[0].pattern) {
      const auto left = PortKnownBoolean(conditional->left(), scope), right = PortKnownBoolean(conditional->right(), scope);
      if (left && right) {
        if (left == right) return marker(PortMapPiece::Constant);
        std::vector<PortMapPiece> refs;
        if (PortCollectBooleanRefs(*conditional->conditions[0].expr, scope, refs, depth + 1)) {
          if (refs.empty()) return marker(PortMapPiece::Constant);
          if (width > 1) refs.push_back({PortMapPiece::Constant, nullptr, &expr, 1, 0, width - 1, {}});
          return refs;
        }
      }
    }
  }
  if (width == 1 && (expr.as_if<BinaryExpression>() || expr.as_if<ConditionalExpression>() ||
                    (expr.as_if<UnaryExpression>() && expr.as<UnaryExpression>().op == UnaryOperator::LogicalNot))) {
    std::vector<PortMapPiece> refs;
    if (PortCollectBooleanRefs(expr, scope, refs, depth + 1))
      return refs.empty() ? marker(PortMapPiece::Constant) : refs;
    return marker(PortMapPiece::Unsupported);
  }
  if (const auto *u = expr.as_if<UnaryExpression>()) {
    if (u->op == UnaryOperator::BitwiseNot && PortPackedWidth(*u->operand().type) == width)
      return PortBuildExpressionMap(u->operand(), scope, depth + 1);
    return marker(PortMapPiece::Unsupported);
  }
  const auto reference = PortResolveReference(expr);
  if (!reference.valid || !reference.type->isIntegral() ||
      reference.low + width > std::numeric_limits<int32_t>::max()) return marker(PortMapPiece::Unsupported);
  // Static packed element and member selects already resolve to physical bits
  // of their owner. Keep that exact slice for packed arrays of structs too.
  if (const auto *value = reference.symbol->as_if<slang::ast::ValueSymbol>()) {
    const auto *type = &value->getType();
    const bool packed_array = type->isPackedArray();
    while (type->isArray()) type = type->getArrayElementType();
    if (packed_array && type->isPackedUnion()) return marker(PortMapPiece::Unsupported);
  }
  return {{PortMapPiece::Exact, reference.symbol, &expr, 0, reference.low, width,
           reference.logical_axes}};
}
const slang::ast::Expression *PortActualConnection(const slang::ast::InstanceSymbol &instance,
                                                   const slang::ast::PortSymbol &port) {
  const auto *connection = instance.getPortConnection(port);
  if (!connection) return nullptr;
  const auto *expr = connection->getExpression();
  if (expr) if (const auto *assignment = expr->as_if<slang::ast::AssignmentExpression>())
    return &assignment->left();
  return expr;
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
  const auto shape = value ? PortFixedOwnerShape(value->getType()) : std::nullopt;
  const int64_t width = shape ? shape->total_width : type ? PortPackedWidth(*type) : 0;
  if (width > 0) p.pieces.push_back({integral_owner ? PortMapPiece::Exact : PortMapPiece::Unsupported,
                                   symbol, nullptr, 0, 0, width, {}});
  return p;
}
PortProjection PortInitialDownProjection(const slang::ast::Symbol *symbol,
                                        const std::vector<PortMapPiece> &connection,
                                        const CanonFrame *frame = nullptr) {
  auto initial = PortIdentityProjection(symbol, frame);
  const auto *value = symbol->as_if<slang::ast::ValueSymbol>();
  if (!value || value->getType().isIntegral()) return initial;
  const auto shape = PortFixedOwnerShape(value->getType());
  if (!shape || shape->unpacked.empty()) return initial;
  PortProjection exact; exact.owner = symbol; exact.owner_frame = frame;
  for (const auto &piece : connection) {
    if ((piece.kind != PortMapPiece::Exact && piece.kind != PortMapPiece::Dependency) ||
        piece.source != symbol || piece.logical_axes.size() != shape->unpacked.size() ||
        piece.source_low < 0 || piece.width <= 0 || piece.source_low + piece.width > shape->packed_width) continue;
    int64_t prefix = 0;
    bool valid = true;
    for (size_t axis = 0; axis < shape->unpacked.size(); ++axis) {
      const auto [left, right] = shape->unpacked[axis];
      const auto selected = piece.logical_axes[axis];
      const int64_t count = std::abs(int64_t(left) - right) + 1;
      const int64_t offset = PortAxisOffset(left, right, selected.first);
      if (selected.first != selected.second || offset < 0 || offset >= count) { valid = false; break; }
      prefix = prefix * count + offset; // checked total shape bounds imply checked prefix
    }
    if (!valid) continue;
    exact.pieces.push_back({PortMapPiece::Exact, symbol, piece.expr,
                           prefix * shape->packed_width + piece.source_low,
                           piece.source_low, piece.width, piece.logical_axes});
    if (exact.pieces.size() > kPortMapMaxPieces) return initial;
  }
  return exact.pieces.empty() ? initial : exact;
}
// Compose a root->current projection with a formal-current->source connection.
std::vector<PortProjection> PortComposeUp(const PortProjection &current,
                                         const std::vector<PortMapPiece> &connection) {
  std::vector<PortProjection> routes;
  for (const auto &a : current.pieces) for (const auto &b : connection) {
    if (routes.size() > kPortMapMaxPieces) return routes;
    if (a.kind != PortMapPiece::Exact) {
      PortProjection route; route.owner = current.owner; route.owner_frame = current.owner_frame; route.connection = current.connection;
      auto piece = a; piece.kind = PortMapPiece::Unsupported;
      route.pieces.push_back(std::move(piece)); routes.push_back(std::move(route));
      continue;
    }
    if (!a.logical_axes.empty()) continue; // unsupported unpacked formal ports stop at the caller
    const int64_t low = std::max(a.source_low, b.owner_low);
    const int64_t high = std::min(a.source_low + a.width, b.owner_low + b.width);
    if (low >= high) continue;
    PortProjection route; route.owner = current.owner; route.owner_frame = current.owner_frame; route.connection = current.connection;
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
  PortProjection out; out.owner = current.owner; out.owner_frame = current.owner_frame; out.connection = current.connection;
  for (const auto &a : current.pieces) for (const auto &b : connection) {
    if (a.kind != PortMapPiece::Exact ||
        (b.kind != PortMapPiece::Exact && b.kind != PortMapPiece::Dependency) ||
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
                                             const ExprTraceResult &entry, TraceCompileCache &cache) {
  const auto *expr = entry.selectors.empty() ? entry.expr : entry.selectors.front();
  if (expr == nullptr) return std::nullopt;
  const auto *body = entry.trace_body ? entry.trace_body : GetContainingInstance(entry.symbol);
  PortStaticRef reference;
  if (body) {
    auto &memo = GetOrCreateBodyTraceCache(*body, cache).static_refs;
    auto it = memo.find(expr);
    if (it == memo.end()) it = memo.emplace(expr, PortResolveReference(*expr)).first;
    reference = it->second;
  } else reference = PortResolveReference(*expr);
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
  auto coverage = PortProjectionCoverage(projection);
  if (coverage.empty() && (constant || unresolved)) coverage.emplace_back(0, 0);
  return {port, std::move(coverage), constant, unresolved, projection.owner, projection.connection};
}

// The bound output assignment retains the formal-side conversion. Widened
// signed outputs use the conservative hop; unsigned upper bits are constants.
struct PortOutputConversion {
  int64_t formal = 0, actual = 0;
  bool signed_widening = false, zero_widening = false, unsupported = false;
};
PortOutputConversion PortInspectOutput(const slang::ast::InstanceSymbol &instance,
                                      const slang::ast::PortSymbol &port) {
  PortOutputConversion result;
  if (port.direction != slang::ast::ArgumentDirection::Out) return result;
  const auto *connection = instance.getPortConnection(port);
  const auto *expression = connection ? connection->getExpression() : nullptr;
  const auto *assignment = expression ? expression->as_if<slang::ast::AssignmentExpression>() : nullptr;
  if (!assignment) { result.unsupported = true; return result; }
  const auto *formal = &assignment->right();
  if (const auto *conversion = formal->as_if<slang::ast::ConversionExpression>()) formal = &conversion->operand();
  if (formal->kind != slang::ast::ExpressionKind::EmptyArgument) { result.unsupported = true; return result; }
  result.formal = PortPackedWidth(*formal->type);
  result.actual = PortPackedWidth(*assignment->left().type);
  if (!result.formal || !result.actual) { result.unsupported = true; return result; }
  result.signed_widening = result.actual > result.formal && formal->type->isSigned();
  result.zero_widening = result.actual > result.formal && !formal->type->isSigned();
  return result;
}
std::vector<PortMapPiece> PortBoundConnectionMap(const slang::ast::InstanceSymbol &instance,
                                               const slang::ast::PortSymbol &port,
                                               const slang::ast::Expression &actual,
                                               TraceCompileCache &cache) {
  auto map = GetCachedPortConnectionMap(actual, instance, cache);
  const auto conversion = PortInspectOutput(instance, port);
  if (conversion.formal && !conversion.unsupported) {
    // Only low formal bits flow through an output narrowing or zero extension.
    std::vector<PortMapPiece> clipped;
    for (auto piece : map) {
      const int64_t end = std::min(piece.owner_low + piece.width, conversion.formal);
      if (piece.owner_low < end) { piece.width = end - piece.owner_low; clipped.push_back(std::move(piece)); }
    }
    return clipped;
  }
  return map;
}
std::vector<PortMapPiece> PortInternalMap(const slang::ast::PortSymbol &port,
                                        TraceCompileCache &cache) {
  if (const auto *expression = port.getInternalExpr()) {
    const auto *body = GetContainingInstance(&port);
    if (!body) return {};
    auto &maps = GetOrCreateBodyTraceCache(*body, cache).port_maps;
    auto it = maps.find(expression);
    if (it == maps.end()) it = maps.emplace(expression, PortBuildExpressionMap(*expression, port)).first;
    return it->second;
  }
  const auto *value = port.internalSymbol ? port.internalSymbol->as_if<slang::ast::ValueSymbol>() : nullptr;
  const auto width = value ? PortPackedWidth(value->getType()) : 0;
  return width ? std::vector<PortMapPiece>{{PortMapPiece::Exact, port.internalSymbol, nullptr, 0, 0, width, {}}}
               : std::vector<PortMapPiece>{};
}
bool PortInternalIsIdentity(const slang::ast::PortSymbol &port) { return port.getInternalExpr() == nullptr && port.internalSymbol != nullptr; }
void MarkApproximateTrace(TraceResult &result, const PortCoverage &coverage = {}) {
  if (auto *expression = std::get_if<ExprTraceResult>(&result)) {
    expression->mapping_approximate = true; expression->port_query_coverage = coverage;
  } else if (auto *port = std::get_if<const slang::ast::PortSymbol *>(&result)) {
    result = MappedPortTraceResult{*port, coverage, false, false, *port, nullptr, true};
  }
}

const slang::ast::PortSymbol *PortImagePort(const slang::ast::PortSymbol &port,
    const slang::ast::InstanceBodySymbol &representative, const slang::ast::InstanceBodySymbol &image) {
  const auto rep = representative.getPortList(), actual = image.getPortList();
  for (size_t i = 0; i < rep.size() && i < actual.size(); ++i) {
    if (rep[i] == &port) return actual[i]->as_if<slang::ast::PortSymbol>();
    const auto *group = rep[i]->as_if<slang::ast::MultiPortSymbol>();
    const auto *image_group = actual[i]->as_if<slang::ast::MultiPortSymbol>();
    if (!group || !image_group || group->ports.size() != image_group->ports.size()) continue;
    for (size_t j = 0; j < group->ports.size(); ++j) if (group->ports[j] == &port) return image_group->ports[j];
  }
  return nullptr;
}

std::optional<MappedPortTraceResult> PortApproximateBoundary(const PortProjection &projection) {
  const auto *value = projection.owner ? projection.owner->as_if<slang::ast::ValueSymbol>() : nullptr;
  if (!value) return std::nullopt;
  if (const auto *back = value->getFirstPortBackref())
    return MappedPortTraceResult{back->port, PortProjectionCoverage(projection), false, false, value, nullptr, true};
  return std::nullopt;
}
// A legacy local access that is statically outside the known actual slice is
// already disproved. Keep unknown accesses and actual legacy child routes.
bool PortKeepLegacyResult(const PortProjection &projection, const TraceResult &result,
                          TraceCompileCache &cache) {
  const auto *expression = std::get_if<ExprTraceResult>(&result);
  if (!expression) return true;
  // An unsupported concat chunk still has an exact bound AST dependency
  // union. Do not pull in a statically disconnected neighboring chunk.
  std::unordered_set<const slang::ast::Symbol *> possible_sources;
  bool bounded_expression = false;
  for (const auto &piece : projection.pieces) if (piece.kind == PortMapPiece::Unsupported && piece.expr) {
    bounded_expression = true;
    piece.expr->visitSymbolReferences([&](const auto &, const auto &symbol) {
      if (IsTraceable(&symbol)) possible_sources.insert(&symbol);
    });
  }
  if (bounded_expression && !possible_sources.contains(expression->symbol)) return false;
  if (std::none_of(projection.pieces.begin(), projection.pieces.end(),
                  [&](const auto &p) { return p.source == expression->symbol; })) return true;
  const auto possible = PortLocalCoverage(projection, *expression, cache);
  return !possible || !possible->empty();
}
