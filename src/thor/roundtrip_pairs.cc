#include "thor/roundtrip_pairs.h"
#include "baldr/graphconstants.h"
#include "midgard/pointll.h"
#include "thor/road_twin_index.h"
#include "thor/roundtrip_expansion.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

using namespace valhalla::baldr;
using namespace valhalla::sif;
using valhalla::midgard::PointLL;

namespace valhalla {
namespace thor {

// =====================================================================================
// ShortestPairs — Suurballe & Tarjan §II (the labeling step) and §III (the incidence
// lists sorted by preorder number, the unlabeled-subtree bookkeeping, the concurrent
// traversals that make the whole thing O(m log n)).
// =====================================================================================

namespace {

// One suspended subtree traversal (§III): a DFS over an unlabeled subtree, visiting each
// vertex's incidence list forward — and, for the child-side subtrees, backward from the
// tail after the first wasted edge.
struct Traversal {
  uint32_t root = ShortestPairs::kNone;
  bool parent_side = false;
  bool fwd = true;
  bool done = false;
  uint32_t cur = ShortestPairs::kNone;    // vertex being visited (kNone = pop next)
  uint32_t cursor = ShortestPairs::kNone; // list node in incident(cur)
  std::vector<uint32_t> stack;
};

// Minimal binary heap with decrease-key over vertex ids keyed by d[].
struct MinHeap {
  std::vector<uint32_t> heap;
  std::vector<uint32_t> pos; // kNone = not in heap
  const std::vector<double>* key = nullptr;
  void init(size_t n, const std::vector<double>* k) {
    pos.assign(n, ShortestPairs::kNone);
    heap.clear();
    key = k;
  }
  bool less(uint32_t a, uint32_t b) const {
    return (*key)[a] < (*key)[b];
  }
  void up(size_t i) {
    while (i > 0) {
      size_t p = (i - 1) / 2;
      if (!less(heap[i], heap[p]))
        break;
      std::swap(heap[i], heap[p]);
      pos[heap[i]] = static_cast<uint32_t>(i);
      pos[heap[p]] = static_cast<uint32_t>(p);
      i = p;
    }
  }
  void down(size_t i) {
    const size_t n = heap.size();
    while (true) {
      size_t l = 2 * i + 1, r = l + 1, m = i;
      if (l < n && less(heap[l], heap[m]))
        m = l;
      if (r < n && less(heap[r], heap[m]))
        m = r;
      if (m == i)
        break;
      std::swap(heap[i], heap[m]);
      pos[heap[i]] = static_cast<uint32_t>(i);
      pos[heap[m]] = static_cast<uint32_t>(m);
      i = m;
    }
  }
  void push_or_decrease(uint32_t v) {
    if (pos[v] == ShortestPairs::kNone) {
      pos[v] = static_cast<uint32_t>(heap.size());
      heap.push_back(v);
      up(heap.size() - 1);
    } else {
      up(pos[v]);
    }
  }
  bool empty() const {
    return heap.empty();
  }
  uint32_t pop() {
    const uint32_t v = heap.front();
    pos[v] = ShortestPairs::kNone;
    heap.front() = heap.back();
    heap.pop_back();
    if (!heap.empty()) {
      pos[heap.front()] = 0;
      down(0);
    }
    return v;
  }
};

} // namespace

void ShortestPairs::Run(std::vector<uint32_t> parent, std::vector<Arc> arcs) {
  parent_ = std::move(parent);
  arcs_ = std::move(arcs);
  n_ = static_cast<uint32_t>(parent_.size());
  d_.assign(n_, kInf);
  p_.assign(n_, kNone);
  q_.assign(n_, kNone);
  pre_.assign(n_, 0);
  post_.assign(n_, 0);
  labeled_count_ = 0;
  steps_ = 0;
  if (n_ == 0)
    return;

  // --- the tree: children CSR, preorder / postorder numbers -------------------------
  root_ = kNone;
  std::vector<uint32_t> cstart(n_ + 1, 0);
  for (uint32_t x = 0; x < n_; ++x) {
    if (parent_[x] == kNone)
      root_ = x;
    else
      ++cstart[parent_[x] + 1];
  }
  for (uint32_t x = 0; x < n_; ++x)
    cstart[x + 1] += cstart[x];
  std::vector<uint32_t> clist(cstart[n_]);
  {
    std::vector<uint32_t> fill(cstart.begin(), cstart.end() - 1);
    for (uint32_t x = 0; x < n_; ++x)
      if (parent_[x] != kNone)
        clist[fill[parent_[x]]++] = x;
  }
  std::vector<uint32_t> order;
  order.reserve(n_);
  {
    std::vector<std::pair<uint32_t, uint32_t>> st; // (vertex, child cursor)
    st.reserve(1024);
    uint32_t pre = 0, post = 0;
    st.emplace_back(root_, cstart[root_]);
    pre_[root_] = pre++;
    order.push_back(root_);
    while (!st.empty()) {
      auto& top = st.back();
      const uint32_t x = top.first;
      if (top.second < cstart[x + 1]) {
        const uint32_t ch = clist[top.second++];
        pre_[ch] = pre++;
        order.push_back(ch);
        st.emplace_back(ch, cstart[ch]);
      } else {
        post_[x] = post++;
        st.pop_back();
      }
    }
  }

  // --- incidence lists, sorted by the preorder number of the other end (§III) --------
  // Arc a owns two list nodes: 2a in incident(u), 2a+1 in incident(w).  Appending in
  // preorder of the OTHER end is the radix sort the paper describes.
  const uint32_t m = static_cast<uint32_t>(arcs_.size());
  std::vector<uint32_t> istart(n_ + 1, 0);
  for (const auto& a : arcs_) {
    ++istart[a.u + 1];
    ++istart[a.w + 1];
  }
  for (uint32_t x = 0; x < n_; ++x)
    istart[x + 1] += istart[x];
  std::vector<uint32_t> iarcs(istart[n_]);
  {
    std::vector<uint32_t> fill(istart.begin(), istart.end() - 1);
    for (uint32_t a = 0; a < m; ++a) {
      iarcs[fill[arcs_[a].u]++] = a;
      iarcs[fill[arcs_[a].w]++] = a;
    }
  }
  std::vector<uint32_t> nxt(2 * static_cast<size_t>(m), kNone), prv(2 * static_cast<size_t>(m), kNone);
  std::vector<uint32_t> head(n_, kNone), tail(n_, kNone);
  auto append = [&](uint32_t x, uint32_t nd) {
    nxt[nd] = kNone;
    prv[nd] = tail[x];
    if (tail[x] != kNone)
      nxt[tail[x]] = nd;
    else
      head[x] = nd;
    tail[x] = nd;
  };
  for (uint32_t y : order) {
    for (uint32_t k = istart[y]; k < istart[y + 1]; ++k) {
      const uint32_t a = iarcs[k];
      if (arcs_[a].u == y)
        append(arcs_[a].w, 2 * a + 1); // w's node, ordered by pre(u = y)
      else
        append(arcs_[a].u, 2 * a); // u's node, ordered by pre(w = y)
    }
  }
  std::vector<uint32_t>().swap(iarcs);
  std::vector<uint32_t>().swap(istart);

  // --- unlabeled subtrees: parent + doubly linked children lists ----------------------
  std::vector<uint32_t> uparent = parent_;
  std::vector<uint32_t> uchead(n_, kNone), ucnext(n_, kNone), ucprev(n_, kNone);
  for (uint32_t x = 0; x < n_; ++x) {
    uint32_t last = kNone;
    for (uint32_t k = cstart[x]; k < cstart[x + 1]; ++k) {
      const uint32_t ch = clist[k];
      ucprev[ch] = last;
      ucnext[ch] = kNone;
      if (last == kNone)
        uchead[x] = ch;
      else
        ucnext[last] = ch;
      last = ch;
    }
  }
  std::vector<uint32_t>().swap(clist);
  std::vector<uint32_t>().swap(cstart);
  std::vector<uint8_t> labeled(n_, 0);

  bytes_ = sizeof(Arc) * arcs_.capacity() + sizeof(uint32_t) * (nxt.size() + prv.size()) +
           sizeof(uint32_t) * n_ * 11 + sizeof(double) * n_ + sizeof(uint32_t) * order.capacity();

  MinHeap heap;
  heap.init(n_, &d_);
  d_[root_] = 0.0;
  heap.push_or_decrease(root_);

  // Traversal objects (and their DFS stacks) are pooled across labelings: with ~1.7 M
  // labelings at 300 km, allocating them per step is the difference between 2.7 s and
  // well under a second.
  std::vector<Traversal> travs;
  travs.reserve(8);
  size_t ntrav = 0;
  auto new_trav = [&](uint32_t root, bool parent_side) {
    if (ntrav == travs.size())
      travs.emplace_back();
    Traversal& t = travs[ntrav++];
    t.root = root;
    t.parent_side = parent_side;
    t.fwd = true;
    t.done = false;
    t.cur = kNone;
    t.cursor = kNone;
    t.stack.clear();
    t.stack.push_back(root);
  };

  auto relax = [&](uint32_t w, double nd, uint32_t arc, uint32_t v) {
    if (nd < d_[w]) {
      d_[w] = nd;
      p_[w] = arc;
      q_[w] = v;
      heap.push_or_decrease(w);
    }
  };
  // Delete arc a from both incidence lists; a suspended scan sitting on one of its
  // list nodes is moved along first (§III: "we must check whether there is a scan of
  // incident(z) suspended at edge (u, w)").
  auto delete_arc = [&](uint32_t a) {
    for (int side = 0; side < 2; ++side) {
      const uint32_t nd = 2 * a + side;
      const uint32_t owner = side == 0 ? arcs_[a].u : arcs_[a].w;
      for (size_t ti = 0; ti < ntrav; ++ti) {
        Traversal& t = travs[ti];
        if (!t.done && t.cursor == nd)
          t.cursor = t.fwd ? nxt[nd] : prv[nd];
      }
      const uint32_t p = prv[nd], nx = nxt[nd];
      if (p != kNone)
        nxt[p] = nx;
      else
        head[owner] = nx;
      if (nx != kNone)
        prv[nx] = p;
      else
        tail[owner] = p;
      nxt[nd] = prv[nd] = kNone;
    }
  };
  auto other_end = [&](uint32_t a, uint32_t x) { return arcs_[a].u == x ? arcs_[a].w : arcs_[a].u; };

  // One step of a suspended traversal: continue until a wasted edge or the end of the
  // current vertex's visit (§III).
  auto step = [&](Traversal& t, uint32_t v) {
    ++steps_;
    if (t.cur == kNone) {
      if (t.stack.empty()) {
        t.done = true;
        return;
      }
      t.cur = t.stack.back();
      t.stack.pop_back();
      for (uint32_t y = uchead[t.cur]; y != kNone; y = ucnext[y])
        t.stack.push_back(y);
      t.cursor = head[t.cur];
      t.fwd = true;
    }
    while (true) {
      if (t.cursor == kNone) {
        t.cur = kNone; // this vertex's visit is complete
        return;
      }
      const uint32_t nd = t.cursor;
      const uint32_t a = nd >> 1;
      const uint32_t other = other_end(a, t.cur);
      // parent side: the other end must be a descendant of v (i.e. in a child subtree);
      // child side: the other end must NOT be a descendant of this subtree's root.
      const bool wasted = t.parent_side ? !is_desc(other, v) : is_desc(other, t.root);
      if (wasted) {
        if (t.parent_side) {
          t.cursor = nxt[nd];
          return;
        }
        if (t.fwd) {
          t.fwd = false;
          t.cursor = tail[t.cur];
          return;
        }
        t.cur = kNone;
        return;
      }
      delete_arc(a); // moves t.cursor along
      relax(arcs_[a].w, d_[v] + arcs_[a].c, a, v);
    }
  };

  while (!heap.empty()) {
    const uint32_t v = heap.pop();
    labeled[v] = 1;
    ++labeled_count_;
    // v leaves its unlabeled subtree: detach from the parent's children list, and every
    // child becomes the root of its own unlabeled subtree.
    const uint32_t pv = uparent[v];
    if (pv != kNone) {
      const uint32_t p = ucprev[v], nx = ucnext[v];
      if (p != kNone)
        ucnext[p] = nx;
      else
        uchead[pv] = nx;
      if (nx != kNone)
        ucprev[nx] = p;
      uparent[v] = kNone;
    }
    ntrav = 0;
    if (pv != kNone) {
      uint32_t r = pv;
      while (uparent[r] != kNone)
        r = uparent[r];
      new_trav(r, true);
    }
    for (uint32_t y = uchead[v]; y != kNone; y = ucnext[y]) {
      uparent[y] = kNone;
      new_trav(y, false);
    }
    uchead[v] = kNone;
    // incident(v): every arc is deleted; the ones leaving v are processed.
    for (uint32_t nd = head[v]; nd != kNone;) {
      const uint32_t next = nxt[nd];
      const uint32_t a = nd >> 1;
      delete_arc(a);
      if (arcs_[a].u == v)
        relax(arcs_[a].w, d_[v] + arcs_[a].c, a, v);
      nd = next;
    }
    // the concurrent traversals of the new unlabeled subtrees, all but one.
    size_t active = ntrav;
    while (active > 1) {
      for (size_t ti = 0; ti < ntrav; ++ti) {
        Traversal& t = travs[ti];
        if (t.done)
          continue;
        step(t, v);
        if (t.done)
          --active;
        if (active <= 1)
          break;
      }
    }
  }
}

void ShortestPairs::Construct(uint32_t v, std::vector<Step>& path_a, std::vector<Step>& path_b) const {
  path_a.clear();
  path_b.clear();
  if (!has_pair(v))
    return;
  // Initialization: mark v, q(v), q(q(v)), ... down to the root.  A hash set keyed on the
  // marked vertices keeps this O(path) rather than O(n) per construction.
  std::unordered_map<uint32_t, uint8_t> marks;
  for (uint32_t x = v; x != root_; x = q_[x])
    marks[x] = 1;
  auto walk = [&](std::vector<Step>& out) {
    out.clear();
    uint32_t x = v;
    while (x != root_) {
      auto it = marks.find(x);
      if (it != marks.end() && it->second) {
        it->second = 0;
        const uint32_t a = p_[x];
        out.push_back({arcs_[a].u, x, a});
        x = arcs_[a].u;
      } else {
        out.push_back({parent_[x], x, kNone});
        x = parent_[x];
      }
    }
    std::reverse(out.begin(), out.end());
  };
  walk(path_a);
  walk(path_b);
}

// =====================================================================================
// RoundTripPairPass — the junction graph H over the harvest forest.
// =====================================================================================

RoundTripPairPass::RoundTripPairPass(const RoundTripExpansion& exp,
                                     GraphReader& reader,
                                     const RoadTwinIndex* twins,
                                     uint32_t access_mask,
                                     double exemption_m,
                                     double twin_join_m)
    : exp_(exp), reader_(reader), twins_(twins), access_mask_(access_mask),
      exemption_m_(exemption_m), twin_join_m_(twin_join_m) {
}

void RoundTripPairPass::set_return_legal(bool on) {
  return_legal_ = on;
}

void RoundTripPairPass::set_prefer_two_way_tree(bool on) {
  prefer_two_way_tree_ = on;
}

double RoundTripPairPass::edge_len(uint32_t label) const {
  const auto& labels = exp_.labels();
  const auto& l = labels[label];
  const uint32_t pd = l.path_distance();
  if (l.predecessor() == kInvalidLabel)
    return pd;
  const uint32_t ppd = labels[l.predecessor()].path_distance();
  return pd >= ppd ? pd - ppd : 0.0;
}

uint64_t RoundTripPairPass::canonical_node(const GraphId& node, graph_tile_ptr& tile) const {
  if (!reader_.GetGraphTile(node, tile))
    return 0;
  const NodeInfo* ni = tile->node(node);
  uint64_t key = node.value;
  for (uint32_t t = 0; t < ni->transition_count(); ++t)
    key = std::min(key, tile->transition(ni->transition_index() + t)->endnode().value);
  return key;
}

bool RoundTripPairPass::same_physical_node(const GraphId& a, const GraphId& b) const {
  if (a == b)
    return true;
  graph_tile_ptr ta, tb;
  const uint64_t ka = canonical_node(a, ta), kb = canonical_node(b, tb);
  return ka != 0 && ka == kb;
}

uint32_t RoundTripPairPass::junction_of_node(const GraphId& node) const {
  graph_tile_ptr tile;
  const uint64_t key = canonical_node(node, tile);
  if (!key)
    return kNone;
  auto it = jindex_.find(key);
  return it == jindex_.end() ? kNone : it->second;
}

namespace {
struct UnionFind {
  std::vector<uint32_t> p;
  explicit UnionFind(uint32_t n) : p(n) {
    for (uint32_t i = 0; i < n; ++i)
      p[i] = i;
  }
  uint32_t find(uint32_t x) {
    while (p[x] != x) {
      p[x] = p[p[x]];
      x = p[x];
    }
    return x;
  }
  bool unite(uint32_t a, uint32_t b) {
    a = find(a);
    b = find(b);
    if (a == b)
      return false;
    p[std::max(a, b)] = std::min(a, b);
    return true;
  }
};
} // namespace

void RoundTripPairPass::Run() {
  const auto t0 = std::chrono::steady_clock::now();
  const auto& labels = exp_.labels();
  n_labels_ = static_cast<uint32_t>(labels.size());
  stats_ = Stats{};
  stats_.labels = n_labels_;
  junction_of_.assign(n_labels_, kNone);
  canonical_of_.resize(n_labels_);
  jparent_.clear();
  jindex_.clear();
  harcs_.clear();
  tree_arc_.clear();
  nontree_arc_.clear();
  n_junctions_ = 0;
  if (n_labels_ == 0)
    return;

  // 1) Aliases.  Dijkstras::SetOriginLocations seeds one label per origin PathEdge and
  //    the round-trip request carries the start twice (F23), so an origin edge can own
  //    two labels; the expansion hangs children off the first.  Everything keys on the
  //    first.
  std::unordered_map<uint64_t, uint32_t> first_origin;
  for (uint32_t i = 0; i < n_labels_; ++i) {
    canonical_of_[i] = i;
    if (labels[i].predecessor() == kInvalidLabel) {
      auto it = first_origin.emplace(labels[i].edgeid().value, i);
      if (!it.second)
        canonical_of_[i] = it.first->second;
    }
  }
  auto canon = [&](uint32_t i) { return canonical_of_[i]; };

  // 2) Physical junctions, pre-unification: the end node of every canonical label,
  //    hierarchy levels folded (min GraphId over the node and its transition twins).
  std::unordered_map<uint64_t, uint32_t> jindex0;
  jindex0.reserve(n_labels_);
  std::vector<uint32_t> jn0(n_labels_, kNone); // per label: its end junction (pre-unification)
  std::vector<GraphId> jnode0;                 // per junction0: a representative node
  std::vector<PointLL> jll0;                   // per junction0: its location
  graph_tile_ptr tile;
  for (uint32_t i = 0; i < n_labels_; ++i) {
    if (canon(i) != i)
      continue;
    ++stats_.canonical_labels;
    const GraphId en = labels[i].endnode();
    const uint64_t key = canonical_node(en, tile);
    if (!key)
      continue;
    auto it = jindex0.emplace(key, static_cast<uint32_t>(jnode0.size()));
    if (it.second) {
      jnode0.push_back(en);
      jll0.push_back(tile->get_node_ll(en));
    }
    jn0[i] = it.first->second;
  }
  const uint32_t n0 = static_cast<uint32_t>(jnode0.size());
  stats_.junctions = n0;
  // the junction a label STARTS at: its predecessor's end junction (roots: none)
  auto start_j0 = [&](uint32_t i) -> uint32_t {
    const uint32_t p = labels[i].predecessor();
    return p == kInvalidLabel ? kNone : jn0[canon(p)];
  };

  // 3) Twin unification.  Where the sidecar pairs two carriageways whose end junctions
  //    sit within twin_join_m of each other, the junctions are folded, so the road
  //    becomes ONE arc per direction in H: out on one carriageway, home on the other,
  //    is a capacity conflict — the twin exclusion by construction.  Where the edge
  //    splits do not line up (an exit on one side only) nothing is folded and the
  //    twin ride stays a selection-time reject (the post-check in route_action.cc).
  UnionFind uf(n0);
  if (twins_) {
    std::vector<uint64_t> tw;
    graph_tile_ptr ttile, otile;
    for (uint32_t i = 0; i < n_labels_; ++i) {
      if (canon(i) != i || jn0[i] == kNone)
        continue;
      const uint32_t a = start_j0(i), b = jn0[i];
      if (a == kNone)
        continue;
      const GraphId en = labels[i].endnode();
      if (!reader_.GetGraphTile(en, tile))
        continue;
      const GraphId opp(en.tileid(), en.level(),
                        tile->node(en)->edge_index() + labels[i].opp_index());
      const uint64_t c = std::min(labels[i].edgeid().value, opp.value);
      if (!twins_->maybe_has_twins(c))
        continue;
      tw.clear();
      twins_->append_twins(c, tw);
      for (uint64_t tv : tw) {
        const GraphId t(tv);
        ttile = nullptr;
        if (!reader_.GetGraphTile(t, ttile))
          continue;
        const GraphId t_end = ttile->directededge(t)->endnode();
        otile = ttile;
        const GraphId topp = reader_.GetOpposingEdgeId(t, otile);
        if (!topp.is_valid() || !otile)
          continue;
        const GraphId t_start = otile->directededge(topp)->endnode();
        graph_tile_ptr k1, k2;
        const uint64_t ks = canonical_node(t_start, k1), ke = canonical_node(t_end, k2);
        auto is = jindex0.find(ks), ie = jindex0.find(ke);
        if (is == jindex0.end() || ie == jindex0.end())
          continue;
        const uint32_t c0 = is->second, d0 = ie->second;
        // pair each end of the label's road with the nearer end of the twin
        const double straight = jll0[a].Distance(jll0[c0]) + jll0[b].Distance(jll0[d0]);
        const double crossed = jll0[a].Distance(jll0[d0]) + jll0[b].Distance(jll0[c0]);
        const uint32_t pa = straight <= crossed ? c0 : d0, pb = straight <= crossed ? d0 : c0;
        if (jll0[a].Distance(jll0[pa]) <= twin_join_m_ && uf.unite(a, pa))
          ++stats_.junctions_unified;
        if (jll0[b].Distance(jll0[pb]) <= twin_join_m_ && uf.unite(b, pb))
          ++stats_.junctions_unified;
      }
    }
  }
  // compact the unified junctions
  std::vector<uint32_t> uni(n0, kNone);
  for (uint32_t j0 = 0; j0 < n0; ++j0) {
    const uint32_t r = uf.find(j0);
    if (uni[r] == kNone)
      uni[r] = n_junctions_++;
    uni[j0] = uni[r];
  }
  for (const auto& kv : jindex0)
    jindex_.emplace(kv.first, uni[kv.second]);
  for (uint32_t i = 0; i < n_labels_; ++i)
    if (jn0[i] != kNone)
      junction_of_[i] = uni[jn0[i]];
  auto start_j = [&](uint32_t i) -> uint32_t {
    const uint32_t s0 = start_j0(i);
    return s0 == kNone ? kNone : uni[s0];
  };

  // 4) Potentials: the cheapest arrival cost per unified junction — the harvest's own
  //    d(s, J), exact under the turn-aware costing.
  std::vector<double> jcost(n_junctions_, std::numeric_limits<double>::infinity());
  for (uint32_t i = 0; i < n_labels_; ++i) {
    if (canon(i) != i || junction_of_[i] == kNone)
      continue;
    const uint32_t j = junction_of_[i];
    if (start_j(i) == j)
      continue; // a crossover inside a folded junction
    jcost[j] = std::min(jcost[j], static_cast<double>(labels[i].cost().cost));
  }
  const uint32_t S = source_vertex();
  auto d_j = [&](uint32_t j) { return j == S ? 0.0 : jcost[j]; };
  // every canonical label's opposite edge and whether the return may ride it
  std::vector<uint64_t> opp_of(n_labels_, 0);
  std::vector<uint8_t> opp_ok(n_labels_, 0);
  {
    graph_tile_ptr rt0;
    for (uint32_t i = 0; i < n_labels_; ++i) {
      if (canon(i) != i || junction_of_[i] == kNone)
        continue;
      rt0 = nullptr;
      const GraphId opp = reader_.GetOpposingEdgeId(labels[i].edgeid(), rt0);
      if (!opp.is_valid() || !rt0)
        continue;
      opp_of[i] = opp.value;
      const DirectedEdge* de = rt0->directededge(opp);
      opp_ok[i] = (!de->is_shortcut() && (de->forwardaccess() & access_mask_)) ? 1 : 0;
    }
  }
  // 4b) The tree: one arrival label per junction.  The FORWARD leg is the tree path,
  //    and the second phase traverses tree arcs for free — so a tree arc the return
  //    cannot ride (a one-way pointing away from the start) is a repair waiting to
  //    happen wherever the second path borrows it.  Arrivals over TWO-WAY roads are
  //    therefore preferred, cheapest first, from junctions of strictly lower potential
  //    (which keeps the tree acyclic); a one-way arrival is the tree arc only where
  //    nothing two-way reaches the junction from below.  TREE APPROXIMATION: where
  //    the pick is not the cheapest arrival, the tree path is a legal but not a
  //    cheapest route, and its reduced cost is treated as zero regardless.
  jparent_.assign(n_junctions_, kNone);
  std::vector<uint8_t> jparent_twoway(n_junctions_, 0);
  std::vector<double> jparent_cost(n_junctions_, std::numeric_limits<double>::infinity());
  for (uint32_t i = 0; i < n_labels_; ++i) {
    if (canon(i) != i || junction_of_[i] == kNone)
      continue;
    const uint32_t j = junction_of_[i];
    const uint32_t from = start_j(i);
    if (from == j)
      continue;
    if (from != kNone && !(jcost[from] < jcost[j]))
      continue; // only from strictly below: the tree must stay acyclic
    const double d = labels[i].cost().cost;
    const bool two_way = prefer_two_way_tree_ ? opp_ok[i] != 0 : true;
    const bool better = (two_way && !jparent_twoway[j]) ||
                        (two_way == static_cast<bool>(jparent_twoway[j]) && d < jparent_cost[j]);
    if (better) {
      jparent_[j] = i;
      jparent_twoway[j] = two_way ? 1 : 0;
      jparent_cost[j] = d;
    }
  }
  for (uint32_t j = 0; j < n_junctions_; ++j) {
    if (jparent_[j] == kNone)
      continue;
    if (jparent_twoway[j] || !prefer_two_way_tree_)
      ++stats_.tree_two_way;
    else
      ++stats_.tree_one_way;
    if (static_cast<double>(labels[jparent_[j]].cost().cost) > jcost[j] + 1e-3)
      ++stats_.tree_not_cheapest;
  }

  // 5) The arcs of H, merged by (from, to): one arc per road-and-direction.
  std::unordered_map<uint64_t, uint32_t> arc_index; // (from << 32 | to) -> harc
  arc_index.reserve(n_labels_ * 2);
  auto arc_key = [](uint32_t f, uint32_t t) { return (static_cast<uint64_t>(f) << 32) | t; };
  auto merge_arc = [&](uint32_t from, uint32_t to, uint32_t fwd_label, uint64_t ret_edge,
                       float len, float cost, bool is_tree) -> uint32_t {
    auto it = arc_index.find(arc_key(from, to));
    if (it == arc_index.end()) {
      HArc h;
      h.from = from;
      h.to = to;
      h.fwd_label = fwd_label;
      h.ret_edge = ret_edge;
      h.len = len;
      h.cost = cost;
      h.tree = is_tree;
      harcs_.push_back(h);
      arc_index.emplace(arc_key(from, to), static_cast<uint32_t>(harcs_.size() - 1));
      return static_cast<uint32_t>(harcs_.size() - 1);
    }
    HArc& h = harcs_[it->second];
    if (is_tree) {
      h.tree = true;
      h.fwd_label = fwd_label;
      h.len = len;
      h.cost = cost;
    } else if (h.fwd_label == kNone && fwd_label != kNone) {
      h.fwd_label = fwd_label;
      h.len = len;
      h.cost = cost;
    } else if (!h.tree && fwd_label != kNone && cost < h.cost) {
      h.fwd_label = fwd_label;
      h.len = len;
      h.cost = cost;
    }
    if (h.ret_edge == 0 && ret_edge != 0)
      h.ret_edge = ret_edge;
    return it->second;
  };
  // tree arcs first, so they own their (from, to)
  tree_arc_.assign(n_junctions_, kNone);
  graph_tile_ptr rt;
  for (uint32_t j = 0; j < n_junctions_; ++j) {
    const uint32_t i = jparent_[j];
    if (i == kNone)
      continue;
    const uint32_t from = start_j(i) == kNone ? S : start_j(i);
    const uint64_t ret = opp_ok[i] ? opp_of[i] : 0;
    const uint32_t p = labels[i].predecessor();
    const float cost = static_cast<float>(labels[i].cost().cost -
                                          (p == kInvalidLabel ? 0.0 : labels[p].cost().cost));
    tree_arc_[j] = merge_arc(from, j, i, ret, static_cast<float>(edge_len(i)), cost, true);
    ++stats_.arcs_tree;
  }
  // every other settled label: its forward arc, and the return-only reverse arc when
  // the opposite direction was never settled
  for (uint32_t i = 0; i < n_labels_; ++i) {
    if (canon(i) != i || junction_of_[i] == kNone)
      continue;
    const uint32_t to = junction_of_[i];
    const uint32_t from = start_j(i) == kNone ? S : start_j(i);
    if (from == to)
      continue;
    const GraphId e = labels[i].edgeid();
    const GraphId opp(opp_of[i]);
    const bool opp_rideable = opp_ok[i] != 0;
    const uint32_t p = labels[i].predecessor();
    const float cost = static_cast<float>(labels[i].cost().cost -
                                          (p == kInvalidLabel ? 0.0 : labels[p].cost().cost));
    const float len = static_cast<float>(edge_len(i));
    if (jparent_[to] != i) {
      merge_arc(from, to, i, opp_rideable ? opp.value : 0, len, cost, false);
      ++stats_.arcs_forward;
    }
    if (opp_rideable)
      ++stats_.arcs_two_way;
    // the return may ride e itself (to -> from), whether or not the expansion ever
    // settled it in that direction
    if (from != S) {
      const uint32_t opp_label = opp.is_valid() ? exp_.label_index(opp) : kInvalidLabel;
      if (opp_label == kInvalidLabel || opp_label >= n_labels_) {
        merge_arc(to, from, kNone, e.value, len, cost, false);
        ++stats_.arcs_return_only;
      }
    } else if (opp.is_valid() && reader_.GetGraphTile(opp, rt)) {
      // an ORIGIN edge: the return arrives at the start along it from its far end (the
      // node behind the snap point), which is the only way home when the start's
      // street is one-way.  S -> that junction, return-only, riding e's first part.
      const GraphId far = rt->directededge(opp)->endnode();
      graph_tile_ptr ft;
      const uint64_t fk = canonical_node(far, ft);
      auto fit = fk ? jindex_.find(fk) : jindex_.end();
      if (fit != jindex_.end() && fit->second != to) {
        merge_arc(S, fit->second, kNone, e.value, len, cost, false);
        ++stats_.arcs_return_only;
        ++stats_.root_return_arcs;
      }
    }
  }

  // 6) ShortestPairs over H: the tree from the parents, the nontree arcs with their
  //    reduced costs c' = c - d(to) + d(from), clamped at zero (the harvest's
  //    potentials carry turn costs the arc costs of H do not — TURN-COST APPROXIMATION).
  std::vector<uint32_t> parent(n_junctions_ + 1, kNone);
  for (uint32_t j = 0; j < n_junctions_; ++j) {
    const uint32_t a = tree_arc_[j];
    parent[j] = a == kNone ? S : harcs_[a].from;
  }
  parent[S] = kNone;
  parent_of_ = parent;
  std::vector<ShortestPairs::Arc> arcs;
  arcs.reserve(harcs_.size());
  nontree_arc_.reserve(harcs_.size());
  for (uint32_t h = 0; h < harcs_.size(); ++h) {
    const HArc& a = harcs_[h];
    if (a.tree)
      continue;
    // RETURN-LEGAL SECOND PHASE: an arc the return cannot ride (a one-way pointing away
    // from the start, with no rideable opposite) is not offered to the second path.
    // The first path still reaches such roads through the tree.
    if (return_legal_ && a.ret_edge == 0) {
      ++stats_.arcs_forward_only_dropped;
      continue;
    }
    double c = static_cast<double>(a.cost) - d_j(a.to) + d_j(a.from);
    if (c < 0.0) {
      c = 0.0;
      ++stats_.arcs_clamped;
    }
    arcs.push_back({a.from, a.to, static_cast<float>(c)});
    nontree_arc_.push_back(h);
  }
  // the Start Exemption: a parallel zero-cost copy of every tree arc whose label ends
  // inside the exempt radius and can be ridden home (capacity two — ADR-0037 §3)
  for (uint32_t j = 0; j < n_junctions_; ++j) {
    const uint32_t a = tree_arc_[j];
    if (a == kNone)
      continue;
    const HArc& h = harcs_[a];
    if (h.ret_edge == 0 || static_cast<double>(labels[h.fwd_label].path_distance()) > exemption_m_)
      continue;
    arcs.push_back({h.from, h.to, 0.f});
    nontree_arc_.push_back(a);
    ++stats_.arcs_exempt;
  }
  stats_.vertices = n_junctions_ + 1;
  stats_.arcs = static_cast<uint32_t>(harcs_.size());
  stats_.build_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  const auto t1 = std::chrono::steady_clock::now();
  sp_.Run(std::move(parent), std::move(arcs));
  stats_.run_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  stats_.bytes = sp_.bytes() + sizeof(HArc) * harcs_.capacity() +
                 sizeof(uint32_t) * (junction_of_.size() + jparent_.size() + canonical_of_.size() +
                                     tree_arc_.size() + nontree_arc_.size() + jn0.size()) +
                 jindex_.size() * 40 + jindex0.size() * 40;
  for (uint32_t j = 0; j < n_junctions_; ++j)
    stats_.sinks_with_pair += sp_.has_pair(j) ? 1 : 0;
  stats_.labeled = sp_.labeled();
  stats_.steps = sp_.steps();
  // the source's fan-out, for the ledger: how many roots and how many arcs leave S
  for (uint32_t j = 0; j < n_junctions_; ++j)
    if (parent_of_[j] == S)
      ++stats_.source_children;
}

void RoundTripPairPass::steps_to_arcs(const std::vector<ShortestPairs::Step>& steps,
                                      std::vector<uint32_t>& out) const {
  out.clear();
  for (const auto& s : steps) {
    if (s.arc == ShortestPairs::kNone) {
      if (s.to < tree_arc_.size() && tree_arc_[s.to] != kNone)
        out.push_back(tree_arc_[s.to]);
    } else if (s.arc < nontree_arc_.size()) {
      out.push_back(nontree_arc_[s.arc]);
    }
  }
}

RoundTripPairPass::Pair RoundTripPairPass::construct(uint32_t junction) const {
  Pair p;
  if (junction >= n_junctions_ || !sp_.has_pair(junction))
    return p;
  std::vector<ShortestPairs::Step> a, b;
  sp_.Construct(junction, a, b);
  std::vector<uint32_t> la, lb;
  steps_to_arcs(a, la);
  steps_to_arcs(b, lb);
  if (la.empty() || lb.empty())
    return p;
  const uint32_t ta = tree_arc_[junction];
  if (lb.back() == ta) {
    p.tree = std::move(lb);
    p.other = std::move(la);
  } else {
    p.tree = std::move(la);
    p.other = std::move(lb);
  }
  p.exists = true;
  p.reduced_cost = sp_.dist(junction);
  const uint32_t tp = jparent_[junction];
  p.pair_cost = 2.0 * (tp == kNone ? 0.0 : static_cast<double>(exp_.labels()[tp].cost().cost)) +
                p.reduced_cost;
  for (uint32_t h : p.tree)
    p.tree_len += harcs_[h].len;
  for (uint32_t h : p.other)
    p.other_len += harcs_[h].len;
  return p;
}

} // namespace thor
} // namespace valhalla
