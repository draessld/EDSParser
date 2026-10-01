// test_transform_fuzz — seeded differential testing of every transform against
// brute-force path expansion.
//
// The other unit tests check hand-written examples. This one generates panels
// biased towards the places the transforms have actually broken — empty and
// duplicate-empty alternatives, symbols at the very start and end, adjacent
// degenerate symbols, runs of regular symbols, alternatives shorter and longer
// than l, minimum-width contexts, path counts either side of the 63-path bitset
// threshold — and checks each output against an oracle that expands paths
// directly:
//
//   P1  eds2leds LINEAR: every path spells the same genome through the l-EDS as
//       through the EDS; source sets still partition at every symbol (when the
//       input's did); internal segments >= l; no two regular symbols in a row;
//       no l-EDS string is carried by no path. Also run on non-partitioning
//       (sample-level heterozygous) sources, where the per-path *set* of
//       spellings must be preserved.
//   P2  eds2leds CARTESIAN: the language of the l-EDS equals the language of the
//       EDS (enumerated on small panels), plus the same structural checks.
//   P3  --block-size block mode is byte-identical to the whole-file transform
//       (LINEAR and CARTESIAN); vcf2eds output is byte-identical across its
//       --block-size values.
//   P4  msa2eds: each MSA row with gaps removed is the genome its path spells,
//       through the EDS and through the direct MSA -> l-EDS output.
//   P5  vcf2eds on haploid VCFs, whole-span and --split-groups: each sample's
//       genome equals an in-test model of vcf2eds's documented overlap rule,
//       through the EDS and through the direct VCF -> l-EDS output; both modes
//       give the same genomes, partition the samples and leave no degenerate
//       symbol whose alternative every sample spells. Against `bcftools
//       consensus`: a sample the overlap-divergence check leaves silent must
//       match it exactly, and a sample it flags must differ (and
//       --strict-overlaps refuses exactly when something is flagged).
//   P6  Parser round trips: EDS text -> parse -> save -> parse is the identity;
//       sources written in hand-varied SEDS encodings -> load -> save_as every
//       format -> load preserve every set.
//
// Every failure prints the seed and a reproducer minimised by greedy shrinking.
//
// Environment:
//   TRANSFORM_FUZZ_SEED=<n>        base seed (default fixed, so ctest is reproducible)
//   TRANSFORM_FUZZ_ITERS=<x>       multiply every property's case count by x
//   TRANSFORM_FUZZ_SOAK=<seconds>  keep running fresh seeds until this much time
//   TRANSFORM_FUZZ_ONLY=<p1,p2..>  run only these properties (linear, het, cartesian,
//                                  block, msa, vcf, roundtrip)
//   TRANSFORM_FUZZ_CASE=<prop>:<seed>  run exactly one case (as printed on failure)
//   TRANSFORM_FUZZ_BCFTOOLS=<path> bcftools binary (default: `bcftools` on PATH)

#include "formats/eds.hpp"
#include "formats/sources.hpp"
#include "transforms/eds_transforms.hpp"
#include "transforms/msa_transforms.hpp"
#include "transforms/vcf_transforms.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>
#include <vector>

using namespace edsparser;
namespace fs = std::filesystem;

namespace {

// ============================================================================
// Infrastructure
// ============================================================================

using Rng = std::mt19937_64;

int rnd(Rng& r, int lo, int hi) {  // inclusive
    return std::uniform_int_distribution<int>(lo, hi)(r);
}
bool chance(Rng& r, double p) { return std::uniform_real_distribution<double>(0, 1)(r) < p; }

std::string rand_dna(Rng& r, int len) {
    static const char* B = "ACGT";
    std::string s;
    for (int i = 0; i < len; ++i) s += B[rnd(r, 0, 3)];
    return s;
}

// Library code reports progress on stdout/stderr. Silence it around every call
// so a failure report is not buried; restored before anything is printed.
struct Silence {
    std::ostringstream sink;
    std::streambuf* out;
    std::streambuf* err;
    Silence() : out(std::cout.rdbuf(sink.rdbuf())), err(std::cerr.rdbuf(sink.rdbuf())) {}
    ~Silence() { std::cout.rdbuf(out); std::cerr.rdbuf(err); }
};

fs::path g_tmp_root;

// A fresh, empty scratch directory per check invocation.
fs::path scratch_dir() {
    static size_t counter = 0;
    fs::path d = g_tmp_root / ("c" + std::to_string(counter++));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void spit(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

struct Failure {
    std::string kind;    // stable tag: the minimiser keeps a shrink only if this matches
    std::string detail;  // human-readable specifics
};
using Result = std::optional<Failure>;

Result fail(const std::string& kind, const std::string& detail) { return Failure{kind, detail}; }

// Greedy shrinking: try each candidate, keep the first that still fails the
// same way, repeat until nothing smaller fails.
template <class Case>
Case minimise(Case c, const std::string& kind,
              const std::function<Result(const Case&)>& check,
              const std::function<std::vector<Case>(const Case&)>& shrinks,
              int budget = 4000) {
    bool progress = true;
    while (progress && budget > 0) {
        progress = false;
        for (const Case& cand : shrinks(c)) {
            if (--budget <= 0) break;
            Result r = check(cand);
            if (r && r->kind == kind) { c = cand; progress = true; break; }
        }
    }
    return c;
}

std::string join_ints(const std::set<int>& s) {
    std::string out;
    for (int x : s) { if (!out.empty()) out += ','; out += std::to_string(x); }
    return out;
}

// Expand a stored PathSet against the path universe. Complement encoding:
// {0} = every path, {0,e1,..} = every path except e1,...
std::set<int> expand(const PathSet& ps, size_t P) {
    std::set<int> out;
    if (ps.empty()) return out;
    if (ps[0] == 0) {
        std::set<int> ex(ps.begin() + 1, ps.end());
        for (int p = 1; p <= static_cast<int>(P); ++p) if (!ex.count(p)) out.insert(p);
        return out;
    }
    out.insert(ps.begin(), ps.end());
    return out;
}

// ============================================================================
// EDS model
// ============================================================================

using Sym = std::vector<std::string>;

std::string full_text(const std::vector<Sym>& syms) {
    std::string s;
    for (const auto& sym : syms) {
        s += '{';
        for (size_t i = 0; i < sym.size(); ++i) { if (i) s += ','; s += sym[i]; }
        s += '}';
    }
    return s;
}

std::string compact_text(const std::vector<Sym>& syms) {
    std::string s;
    for (const auto& sym : syms) {
        if (sym.size() == 1 && !sym[0].empty()) { s += sym[0]; continue; }  // {} keeps brackets
        s += '{';
        for (size_t i = 0; i < sym.size(); ++i) { if (i) s += ','; s += sym[i]; }
        s += '}';
    }
    return s;
}

// A loaded transform output: symbols plus (optionally) expanded sources.
struct Loaded {
    std::vector<Sym> syms;
    std::vector<std::vector<std::set<int>>> src;  // per symbol, per alternative
    size_t num_paths = 0;
    bool has_src = false;
};

std::vector<Sym> load_symbols(const fs::path& p) {
    EDS e = EDS::load(p);
    std::vector<Sym> out;
    for (size_t i = 0; i < e.length(); ++i) out.push_back(e.read_symbol(i));
    return out;
}

// Loads EDS + sources independently, so a cardinality mismatch is reported
// rather than thrown by EDS::load's own validation.
Result load_with_sources(const fs::path& eds, const fs::path& seds, Loaded& L,
                         const std::string& tag) {
    L.syms = load_symbols(eds);
    auto S = Sources::load(seds);
    L.num_paths = S->num_paths();
    L.has_src = true;
    size_t m = 0;
    for (auto& s : L.syms) m += s.size();
    if (S->cardinality() != m)
        return fail(tag + "/cardinality",
                    "EDS has " + std::to_string(m) + " strings, sources " +
                        std::to_string(S->cardinality()));
    size_t id = 0;
    for (auto& sym : L.syms) {
        L.src.emplace_back();
        for (size_t a = 0; a < sym.size(); ++a) {
            PathSet raw = S->read_source(id++);
            for (int x : raw)
                if (x < 0 || x > static_cast<int>(L.num_paths))
                    return fail(tag + "/source-range",
                                "string " + std::to_string(id - 1) + " names path " +
                                    std::to_string(x) + " of " + std::to_string(L.num_paths));
            L.src.back().push_back(expand(raw, L.num_paths));
        }
    }
    return std::nullopt;
}

// All spellings of path p through an EDS with sources (a set, so that
// non-partitioning sources are handled). Returns nullopt if it grows past cap.
std::optional<std::set<std::string>> spell(const std::vector<Sym>& syms,
                                           const std::vector<std::vector<std::set<int>>>& src,
                                           int p, size_t cap = 4096) {
    std::set<std::string> cur{""};
    for (size_t i = 0; i < syms.size(); ++i) {
        std::set<std::string> next;
        for (size_t a = 0; a < syms[i].size(); ++a) {
            if (!src[i][a].count(p)) continue;
            for (const auto& c : cur) next.insert(c + syms[i][a]);
        }
        if (next.size() > cap) return std::nullopt;
        cur.swap(next);
        if (cur.empty()) break;
    }
    return cur;
}

// The language: every spelling of every combination of alternatives.
std::optional<std::set<std::string>> language(const std::vector<Sym>& syms, size_t cap = 20000) {
    std::set<std::string> cur{""};
    for (const auto& sym : syms) {
        std::set<std::string> next;
        for (const auto& c : cur)
            for (const auto& a : sym) next.insert(c + a);
        if (next.size() > cap) return std::nullopt;
        cur.swap(next);
    }
    return cur;
}

// l-EDS property and canonical form: every internal segment (regular symbols
// with a degenerate symbol on both sides) sums to >= l, and no two regular
// symbols are adjacent (TODO 0c — eds2leds and vcf2eds both promise it).
Result check_leds_shape(const std::vector<Sym>& syms, size_t l, const std::string& tag,
                        bool require_canonical = true) {
    long first_deg = -1, last_deg = -1;
    for (size_t i = 0; i < syms.size(); ++i)
        if (syms[i].size() > 1) { if (first_deg < 0) first_deg = i; last_deg = i; }
    if (require_canonical)
        for (size_t i = 0; i + 1 < syms.size(); ++i)
            if (syms[i].size() == 1 && syms[i + 1].size() == 1)
                return fail(tag + "/adjacent-regular",
                            "symbols " + std::to_string(i) + " and " + std::to_string(i + 1) +
                                " are both regular");
    if (first_deg < 0) return std::nullopt;
    size_t run = 0;
    for (long i = first_deg + 1; i <= last_deg; ++i) {
        if (syms[i].size() == 1) { run += syms[i][0].size(); continue; }
        if (run < l)
            return fail(tag + "/leds-property",
                        "internal segment ending before symbol " + std::to_string(i) +
                            " has length " + std::to_string(run) + " < l=" + std::to_string(l));
        run = 0;
    }
    return std::nullopt;
}

// Sources structural checks on a loaded output.
Result check_sources_shape(const Loaded& L, size_t P, bool expect_partition,
                           const std::string& tag) {
    if (L.num_paths != P)
        return fail(tag + "/num-paths", "output records " + std::to_string(L.num_paths) +
                                            " paths, input had " + std::to_string(P));
    for (size_t i = 0; i < L.syms.size(); ++i) {
        for (size_t a = 0; a < L.syms[i].size(); ++a)
            if (L.src[i][a].empty())
                return fail(tag + "/carried-by-none",
                            "symbol " + std::to_string(i) + " alternative " + std::to_string(a) +
                                " (\"" + L.syms[i][a] + "\") is carried by no path");
        if (!expect_partition) continue;
        std::map<int, int> count;
        for (auto& s : L.src[i]) for (int p : s) count[p]++;
        for (int p = 1; p <= static_cast<int>(P); ++p)
            if (count[p] != 1)
                return fail(tag + "/partition",
                            "path " + std::to_string(p) + " is on " + std::to_string(count[p]) +
                                " alternatives of symbol " + std::to_string(i));
    }
    return std::nullopt;
}

std::string describe_spellings(const std::set<std::string>& s) {
    std::string out = "{";
    size_t k = 0;
    for (auto& x : s) {
        if (k++) out += ", ";
        if (k > 4) { out += "..."; break; }
        out += "\"" + x + "\"";
    }
    return out + "}";
}

// ============================================================================
// Panel: an EDS with sources, the input of P1-P3 and P6
// ============================================================================

enum class SrcFormat { SEDS_TEXT, SEDS_SPARSE, EDZ, EDZ_SPARSE, EDZ_COMPRESSED };
const char* fmt_name(SrcFormat f) {
    switch (f) {
        case SrcFormat::SEDS_TEXT: return "seds";
        case SrcFormat::SEDS_SPARSE: return "seds-sparse";
        case SrcFormat::EDZ: return "edz";
        case SrcFormat::EDZ_SPARSE: return "edz-sparse";
        case SrcFormat::EDZ_COMPRESSED: return "edz-compressed";
    }
    return "?";
}

struct Panel {
    std::vector<Sym> syms;
    int P = 1;
    std::vector<std::vector<std::set<int>>> src;  // per symbol, per alternative
    size_t l = 3;
    bool partition = true;
    // execution knobs (part of the case, so the minimiser can simplify them)
    bool compact = true;
    size_t threads = 1;
    SrcFormat fmt = SrcFormat::SEDS_TEXT;
    uint64_t enc_seed = 0;   // drives the hand-varied SEDS text encoding
    uint64_t block_bytes = 0;
    bool canonical = false;  // sources re-encoded by the library's own SEDS writer
};

std::set<int> all_paths(int P) {
    std::set<int> s;
    for (int p = 1; p <= P; ++p) s.insert(p);
    return s;
}

std::string gen_alt(Rng& r, size_t l) {
    int kind = rnd(r, 0, 9);
    if (kind <= 2) return "";                               // empty (deletion)
    if (kind <= 6) return rand_dna(r, rnd(r, 1, std::max<int>(1, l - 1)));  // shorter than l
    if (kind == 7) return rand_dna(r, l);
    return rand_dna(r, rnd(r, l + 1, 2 * l + 2));           // longer than l
}

std::string gen_context(Rng& r, size_t l) {
    int kind = rnd(r, 0, 11);
    if (kind == 0) return "";                               // an empty regular symbol {}
    if (kind <= 3) return rand_dna(r, rnd(r, 1, std::max<int>(1, l - 1)));  // short
    if (kind <= 6) return rand_dna(r, l);                   // minimum width
    if (kind <= 7) return rand_dna(r, l + 1);
    return rand_dna(r, rnd(r, l + 2, 3 * l + 3));
}

// partition=true: each path on exactly one alternative of every symbol.
// partition=false: sample-level heterozygous model — each path on one or two
// alternatives. Either way every alternative is carried by at least one path.
Panel gen_panel(Rng& r, bool with_sources, bool partition, int max_syms, int max_alts) {
    Panel pn;
    pn.l = rnd(r, 1, 6);
    pn.partition = partition;
    if (with_sources) {
        int pk = rnd(r, 0, 19);
        // either side of the 63-path bitset threshold gets a fifth of the cases
        pn.P = pk < 5 ? rnd(r, 2, 4) : pk < 12 ? rnd(r, 2, 9) : pk < 15 ? rnd(r, 10, 40)
             : pk < 19 ? rnd(r, 62, 66) : rnd(r, 1, 1);
    } else {
        pn.P = 1;
    }
    int n = rnd(r, 1, max_syms);
    double p_deg = 0.3 + 0.4 * (rnd(r, 0, 10) / 10.0);
    for (int i = 0; i < n; ++i) {
        bool deg = chance(r, p_deg) && (!with_sources || pn.P >= 2);
        if (!deg) {
            pn.syms.push_back({gen_context(r, pn.l)});
            pn.src.push_back({all_paths(pn.P)});
            continue;
        }
        int k = rnd(r, 2, max_alts);
        if (with_sources) k = std::min(k, pn.P);
        Sym sym;
        for (int a = 0; a < k; ++a) {
            if (a > 0 && chance(r, 0.12)) sym.push_back(sym[rnd(r, 0, a - 1)]);  // duplicate
            else sym.push_back(gen_alt(r, pn.l));
        }
        std::vector<std::set<int>> sets(k);
        if (with_sources) {
            std::vector<int> paths;
            for (int p = 1; p <= pn.P; ++p) paths.push_back(p);
            std::shuffle(paths.begin(), paths.end(), r);
            for (int a = 0; a < k; ++a) sets[a].insert(paths[a]);
            for (int j = k; j < pn.P; ++j) sets[rnd(r, 0, k - 1)].insert(paths[j]);
            if (!partition)
                for (int p = 1; p <= pn.P; ++p)
                    if (chance(r, 0.3)) sets[rnd(r, 0, k - 1)].insert(p);
        }
        pn.syms.push_back(sym);
        pn.src.push_back(sets);
    }
    pn.compact = chance(r, 0.6);
    pn.threads = chance(r, 0.7) ? 1 : (chance(r, 0.5) ? 2 : 4);
    int f = rnd(r, 0, 9);
    pn.fmt = f < 5 ? SrcFormat::SEDS_TEXT : f < 6 ? SrcFormat::SEDS_SPARSE : f < 8 ? SrcFormat::EDZ
           : f < 9 ? SrcFormat::EDZ_SPARSE : SrcFormat::EDZ_COMPRESSED;
    if (pn.fmt == SrcFormat::EDZ_COMPRESSED && !Sources::edz_compressed_available())
        pn.fmt = SrcFormat::EDZ;
    pn.enc_seed = r();
    return pn;
}

// Hand-varied SEDS text: the same set written as an explicit list, with ranges,
// as a complement, or ({0}) as universal — the encodings the writers choose
// between, all of which the reader must accept.
std::string encode_set(const std::set<int>& s, int P, Rng& r) {
    auto ranges = [&](const std::set<int>& ids, bool use_ranges) {
        std::string out;
        auto it = ids.begin();
        while (it != ids.end()) {
            int lo = *it, hi = lo;
            if (use_ranges)
                while (std::next(it) != ids.end() && *std::next(it) == hi + 1) { ++it; ++hi; }
            if (!out.empty()) out += ',';
            out += (hi > lo) ? std::to_string(lo) + "-" + std::to_string(hi) : std::to_string(lo);
            ++it;
        }
        return out;
    };
    bool is_all = static_cast<int>(s.size()) == P;
    int how = rnd(r, 0, 3);
    if (is_all && how <= 1) return "{0}";
    if (how == 2 && !is_all) {  // complement
        std::set<int> ex;
        for (int p = 1; p <= P; ++p) if (!s.count(p)) ex.insert(p);
        return "{0," + ranges(ex, chance(r, 0.5)) + "}";
    }
    return "{" + ranges(s, how == 3) + "}";
}

std::string seds_text(const Panel& pn, bool trailer = true) {
    Rng r(pn.enc_seed);
    std::ostringstream os;
    size_t m = 0;
    for (auto& sets : pn.src)
        for (auto& s : sets) { os << encode_set(s, pn.P, r); ++m; }
    if (trailer) Sources::write_seds_dense_finalize(os, m, pn.P);
    return os.str();
}

// Write the panel's EDS (always FULL: compact input would fuse adjacent regular
// symbols and desynchronise the sources) and its sources in the panel's format.
// Returns the sources path.
fs::path write_panel(const Panel& pn, const fs::path& dir) {
    spit(dir / "in.eds", full_text(pn.syms));
    fs::path text = dir / "in.seds";
    spit(text, seds_text(pn));
    if (pn.canonical) {
        auto C = Sources::load(text);
        C->save_as(dir / "in.canon.seds", Sources::Format::SEDS);
        fs::rename(dir / "in.canon.seds", text);
    }
    if (pn.fmt == SrcFormat::SEDS_TEXT) return text;
    auto S = Sources::load(text);
    Sources::Format f = pn.fmt == SrcFormat::SEDS_SPARSE ? Sources::Format::SEDS_SPARSE
                      : pn.fmt == SrcFormat::EDZ ? Sources::Format::EDZ
                      : pn.fmt == SrcFormat::EDZ_SPARSE ? Sources::Format::EDZ_SPARSE
                      : Sources::Format::EDZ_COMPRESSED;
    fs::path out = dir / (f == Sources::Format::SEDS_SPARSE ? "in.sparse.seds" : "in.edz");
    S->save_as(out, f);
    return out;
}

std::string plain_seds(const Panel& pn) {
    std::string s;
    for (auto& sets : pn.src) for (auto& x : sets) s += "{" + join_ints(x) + "}";
    return s;
}

std::string panel_repro(const Panel& pn, const std::string& prop, bool with_sources) {
    std::ostringstream os;
    os << "  l=" << pn.l << "  paths=" << pn.P << "  compact=" << pn.compact
       << "  threads=" << pn.threads;
    if (with_sources) os << "  sources-format=" << fmt_name(pn.fmt)
                         << (pn.canonical ? " (library-encoded)" : " (hand-encoded)");
    if (pn.block_bytes) os << "  block-bytes=" << pn.block_bytes;
    os << "\n  EDS : " << full_text(pn.syms) << "\n";
    if (with_sources) os << "  SEDS: " << plain_seds(pn) << "   (num_paths=" << pn.P << ")\n";
    os << "  CLI : eds2leds -i repro.eds" << (with_sources ? " -s repro.seds" : "") << " -l "
       << pn.l << (pn.compact ? "" : " --full")
       << (pn.block_bytes ? " --block-size " + std::to_string(pn.block_bytes) : "")
       << "   (" << prop << ")\n";
    return os.str();
}

std::vector<Panel> shrink_panel(const Panel& pn, bool with_sources) {
    std::vector<Panel> out;
    auto renorm = [&](Panel q) {
        for (size_t i = 0; i < q.syms.size(); ++i)
            if (q.syms[i].size() == 1) q.src[i] = {all_paths(q.P)};
        return q;
    };
    // drop a symbol
    for (size_t i = 0; i < pn.syms.size(); ++i) {
        if (pn.syms.size() <= 1) break;
        Panel q = pn;
        q.syms.erase(q.syms.begin() + i);
        q.src.erase(q.src.begin() + i);
        out.push_back(q);
    }
    // drop an alternative, handing its paths to a neighbour (keeps the partition)
    for (size_t i = 0; i < pn.syms.size(); ++i) {
        if (pn.syms[i].size() < 2) continue;
        for (size_t a = 0; a < pn.syms[i].size(); ++a) {
            Panel q = pn;
            size_t b = a == 0 ? 1 : a - 1;
            q.src[i][b].insert(q.src[i][a].begin(), q.src[i][a].end());
            q.syms[i].erase(q.syms[i].begin() + a);
            q.src[i].erase(q.src[i].begin() + a);
            out.push_back(renorm(q));
        }
    }
    // drop a path
    if (with_sources && pn.P > 1) {
        for (int p = pn.P; p >= 1; --p) {
            Panel q = pn;
            q.P--;
            bool ok = true;
            for (size_t i = 0; i < q.syms.size() && ok; ++i) {
                for (size_t a = 0; a < q.syms[i].size(); ++a) {
                    std::set<int> s;
                    for (int x : q.src[i][a]) if (x != p) s.insert(x > p ? x - 1 : x);
                    q.src[i][a] = s;
                }
                for (size_t a = q.syms[i].size(); a-- > 0;)
                    if (q.src[i][a].empty() && q.syms[i].size() > 1) {
                        q.syms[i].erase(q.syms[i].begin() + a);
                        q.src[i].erase(q.src[i].begin() + a);
                    }
                for (auto& s : q.src[i]) if (s.empty()) ok = false;
            }
            if (ok) out.push_back(renorm(q));
        }
    }
    // a path off a second alternative (non-partitioning panels)
    if (with_sources && !pn.partition) {
        for (size_t i = 0; i < pn.syms.size(); ++i)
            for (size_t a = 0; a < pn.syms[i].size(); ++a)
                for (int p : pn.src[i][a]) {
                    int elsewhere = 0;
                    for (size_t b = 0; b < pn.syms[i].size(); ++b)
                        if (b != a && pn.src[i][b].count(p)) elsewhere++;
                    if (!elsewhere || pn.src[i][a].size() < 2) continue;
                    Panel q = pn;
                    q.src[i][a].erase(p);
                    out.push_back(q);
                }
    }
    // shorten a string
    for (size_t i = 0; i < pn.syms.size(); ++i)
        for (size_t a = 0; a < pn.syms[i].size(); ++a) {
            const std::string& s = pn.syms[i][a];
            if (s.empty()) continue;
            Panel q = pn; q.syms[i][a] = s.substr(1); out.push_back(q);
            q = pn; q.syms[i][a] = s.substr(0, s.size() - 1); out.push_back(q);
        }
    // simplify knobs
    if (pn.l > 1) { Panel q = pn; q.l--; out.push_back(q); }
    if (pn.threads != 1) { Panel q = pn; q.threads = 1; out.push_back(q); }
    if (pn.compact) { Panel q = pn; q.compact = false; out.push_back(q); }
    if (pn.fmt != SrcFormat::SEDS_TEXT) { Panel q = pn; q.fmt = SrcFormat::SEDS_TEXT; out.push_back(q); }
    if (!pn.canonical) { Panel q = pn; q.canonical = true; out.push_back(q); }
    // simplify characters
    for (size_t i = 0; i < pn.syms.size(); ++i)
        for (size_t a = 0; a < pn.syms[i].size(); ++a) {
            const std::string& s = pn.syms[i][a];
            if (s.find_first_not_of('A') == std::string::npos) continue;
            Panel q = pn;
            for (char& c : q.syms[i][a]) c = 'A';
            out.push_back(q);
        }
    return out;
}

// ============================================================================
// P1 — eds2leds LINEAR
// ============================================================================

Result check_linear(const Panel& pn) {
    const std::string tag = pn.partition ? "linear" : "het";
    fs::path dir = scratch_dir();
    Loaded L;
    try {
        Silence quiet;
        fs::path seds = write_panel(pn, dir);
        {
            std::ofstream out(dir / "out.leds"), sout(dir / "out.seds");
            eds_to_leds_linear(dir / "in.eds", out, pn.l, &seds, &sout, pn.threads, pn.compact);
        }
        if (Result r = load_with_sources(dir / "out.leds", dir / "out.seds", L, tag)) return r;
    } catch (const std::exception& e) {
        return fail(tag + "/exception", e.what());
    }
    if (Result r = check_sources_shape(L, pn.P, pn.partition, tag)) return r;
    if (Result r = check_leds_shape(L.syms, pn.l, tag)) return r;
    for (int p = 1; p <= pn.P; ++p) {
        auto want = spell(pn.syms, pn.src, p);
        if (!want) continue;  // too many spellings to enumerate
        auto got = spell(L.syms, L.src, p, want->size() * 4 + 16);
        if (!got || *got != *want)
            return fail(tag + "/spelling",
                        "path " + std::to_string(p) + " spells " + describe_spellings(*want) +
                            " through the EDS but " +
                            (got ? describe_spellings(*got) : std::string("(too many)")) +
                            " through the l-EDS");
    }
    return std::nullopt;
}

// ============================================================================
// P2 — eds2leds CARTESIAN
// ============================================================================

Result check_cartesian(const Panel& pn) {
    fs::path dir = scratch_dir();
    std::vector<Sym> out_syms, out_syms_lin;
    try {
        Silence quiet;
        std::string txt = full_text(pn.syms);
        {
            std::istringstream in(txt);
            std::ofstream out(dir / "out.leds");
            eds_to_leds_cartesian(in, out, pn.l, pn.threads, pn.compact);
        }
        out_syms = load_symbols(dir / "out.leds");
        {
            // eds_to_leds_linear without sources is the other cartesian entry
            // point (the stream overload test_merge uses); it must agree.
            std::istringstream in(txt);
            std::ofstream out(dir / "out_lin.leds");
            eds_to_leds_linear(in, out, pn.l, nullptr, nullptr, pn.threads, pn.compact);
        }
        out_syms_lin = load_symbols(dir / "out_lin.leds");
    } catch (const std::exception& e) {
        return fail("cartesian/exception", e.what());
    }
    if (Result r = check_leds_shape(out_syms, pn.l, "cartesian")) return r;
    if (Result r = check_leds_shape(out_syms_lin, pn.l, "cartesian-nosrc")) return r;
    auto want = language(pn.syms);
    if (!want) return std::nullopt;
    auto got = language(out_syms, want->size() * 8 + 64);
    if (!got || *got != *want) {
        std::string ex;
        if (got) {
            for (auto& s : *want) if (!got->count(s)) { ex = " e.g. \"" + s + "\" is lost"; break; }
            if (ex.empty())
                for (auto& s : *got) if (!want->count(s)) { ex = " e.g. \"" + s + "\" is new"; break; }
        }
        return fail("cartesian/language",
                    "EDS language has " + std::to_string(want->size()) + " strings, l-EDS " +
                        (got ? std::to_string(got->size()) : std::string("(too many)")) + ex);
    }
    auto got2 = language(out_syms_lin, want->size() * 8 + 64);
    if (!got2 || *got2 != *want)
        return fail("cartesian-nosrc/language",
                    "eds_to_leds_linear without sources changes the language");
    return std::nullopt;
}

// ============================================================================
// P3 — block mode is byte-identical to whole-file
// ============================================================================

Result check_block(const Panel& pn, bool with_sources) {
    const std::string tag = with_sources ? "block-linear" : "block-cartesian";
    fs::path dir = scratch_dir();
    try {
        Silence quiet;
        fs::path seds = write_panel(pn, dir);
        {
            std::ofstream out(dir / "whole.leds"), sout(dir / "whole.seds");
            if (with_sources)
                eds_to_leds_linear(dir / "in.eds", out, pn.l, &seds, &sout, pn.threads, pn.compact);
            else {
                std::ifstream in(dir / "in.eds");
                eds_to_leds_cartesian(in, out, pn.l, pn.threads, pn.compact);
            }
        }
        {
            std::ofstream out(dir / "block.leds"), sout(dir / "block.seds");
            eds_to_leds_blocked(dir / "in.eds", out, pn.l, with_sources ? &seds : nullptr,
                                with_sources ? &sout : nullptr, pn.block_bytes, pn.threads,
                                pn.compact);
        }
    } catch (const std::exception& e) {
        return fail(tag + "/exception", e.what());
    }
    std::string a = slurp(dir / "whole.leds"), b = slurp(dir / "block.leds");
    if (a != b)
        return fail(tag + "/eds-bytes", "whole-file: " + a.substr(0, 200) + "\n    block-mode: " +
                                            b.substr(0, 200));
    // Byte identity covers the sources too, for every input encoding and
    // format: both modes write untouched entries through copy_range_to_stream().
    if (with_sources && slurp(dir / "whole.seds") != slurp(dir / "block.seds"))
        return fail(tag + "/seds-bytes", "sources differ between whole-file and block mode");
    return std::nullopt;
}

// ============================================================================
// P6 — parser round trips
// ============================================================================

Result check_eds_roundtrip(const Panel& pn) {
    std::string full = full_text(pn.syms);
    fs::path dir = scratch_dir();
    try {
        EDS a = EDS::from_string(full);
        std::vector<Sym> got;
        for (size_t i = 0; i < a.length(); ++i) got.push_back(a.read_symbol(i));
        if (got != pn.syms) return fail("eds-roundtrip/parse", "from_string(full) != model");
        std::ostringstream s1;
        a.save(s1, EDS::OutputFormat::FULL);
        if (s1.str() != full + "\n")
            return fail("eds-roundtrip/save-full", "save(FULL) = " + s1.str());
        spit(dir / "a.eds", full);
        if (load_symbols(dir / "a.eds") != pn.syms)
            return fail("eds-roundtrip/load", "EDS::load(full) != model");
        EDS b = EDS::load(dir / "a.eds");
        if (b.cardinality() != a.cardinality() || b.size() != a.size() || b.length() != a.length())
            return fail("eds-roundtrip/metadata", "file loader and string ctor disagree on n/m/N");
        const auto &ma = a.get_metadata(), &mb = b.get_metadata();
        if (ma.min_context_length != mb.min_context_length ||
            ma.min_internal_context_length != mb.min_internal_context_length ||
            ma.num_empty_strings != mb.num_empty_strings ||
            ma.num_degenerate_symbols != mb.num_degenerate_symbols)
            return fail("eds-roundtrip/metadata", "file loader and string ctor disagree on stats");
        // Compact output fuses adjacent bare (non-empty regular) symbols — a
        // non-canonical EDS is not compact-representable — so compare against
        // the fused model. An empty regular symbol keeps its brackets.
        std::vector<Sym> fused;
        auto bare = [](const Sym& s) { return s.size() == 1 && !s[0].empty(); };
        for (const auto& s : pn.syms) {
            if (bare(s) && !fused.empty() && bare(fused.back()))
                fused.back()[0] += s[0];
            else
                fused.push_back(s);
        }
        std::ostringstream s2;
        b.save(s2, EDS::OutputFormat::COMPACT);
        if (s2.str() != compact_text(pn.syms) + "\n")
            return fail("eds-roundtrip/save-compact", "save(COMPACT) = " + s2.str());
        spit(dir / "c.eds", s2.str());
        if (load_symbols(dir / "c.eds") != fused)
            return fail("eds-roundtrip/compact-reparse",
                        "reparsed compact text is not the fused model: " + s2.str());
    } catch (const std::exception& e) {
        return fail("eds-roundtrip/exception", e.what());
    }
    return std::nullopt;
}

// Sources: arbitrary non-empty sets (not a partition) in every encoding and format.
Result check_sources_roundtrip(const Panel& pn) {
    fs::path dir = scratch_dir();
    std::vector<std::set<int>> want;
    for (auto& sets : pn.src) for (auto& s : sets) want.push_back(s);
    auto compare = [&](const fs::path& p, const std::string& what) -> Result {
        auto S = Sources::load(p);
        if (S->cardinality() != want.size())
            return fail("sources-roundtrip/cardinality",
                        what + ": cardinality " + std::to_string(S->cardinality()));
        if (S->num_paths() != static_cast<size_t>(pn.P))
            return fail("sources-roundtrip/num-paths",
                        what + ": num_paths " + std::to_string(S->num_paths()) + " != " +
                            std::to_string(pn.P));
        for (size_t i = 0; i < want.size(); ++i) {
            auto got = expand(S->read_source(i), pn.P);
            if (got != want[i])
                return fail("sources-roundtrip/set",
                            what + ": entry " + std::to_string(i) + " reads {" + join_ints(got) +
                                "}, wrote {" + join_ints(want[i]) + "}");
        }
        return std::nullopt;
    };
    try {
        Silence quiet;
        spit(dir / "in.seds", seds_text(pn));
        if (Result r = compare(dir / "in.seds", "hand-written SEDS")) return r;
        auto S = Sources::load(dir / "in.seds");
        std::vector<std::pair<Sources::Format, std::string>> fmts = {
            {Sources::Format::SEDS, "a.seds"},        {Sources::Format::SEDS_SPARSE, "b.seds"},
            {Sources::Format::EDZ, "c.edz"},          {Sources::Format::EDZ_SPARSE, "d.edz"}};
        if (Sources::edz_compressed_available())
            fmts.push_back({Sources::Format::EDZ_COMPRESSED, "e.edz"});
        for (auto& [f, name] : fmts) {
            S->save_as(dir / name, f);
            if (Result r = compare(dir / name, "save_as " + name)) return r;
            // and back again: every format must reload to the same sets
            auto T = Sources::load(dir / name);
            T->save_as(dir / ("back_" + name + ".seds"), Sources::Format::SEDS);
            if (Result r = compare(dir / ("back_" + name + ".seds"), name + " -> SEDS")) return r;
        }
        // The library's own text encoding is a fixed point: SEDS -> load -> SEDS
        // is byte-identical. (Hand-written text need not be; it is not canonical.)
        auto A = Sources::load(dir / "a.seds");
        A->save_as(dir / "a2.seds", Sources::Format::SEDS);
        if (slurp(dir / "a2.seds") != slurp(dir / "a.seds"))
            return fail("sources-roundtrip/text-fixed-point",
                        "re-saving the library's own SEDS text changed its bytes");
    } catch (const std::exception& e) {
        return fail("sources-roundtrip/exception", e.what());
    }
    return std::nullopt;
}

Panel gen_sources_panel(Rng& r) {
    // Arbitrary sets for the round trip: not a partition, any non-empty subset.
    Panel pn;
    int pk = rnd(r, 0, 9);
    pn.P = pk < 4 ? rnd(r, 1, 8) : pk < 7 ? rnd(r, 9, 70) : pk < 9 ? rnd(r, 60, 70) : rnd(r, 120, 140);
    int n = rnd(r, 1, 15);
    for (int i = 0; i < n; ++i) {
        int k = rnd(r, 1, 4);
        Sym sym(k, "A");
        std::vector<std::set<int>> sets;
        for (int a = 0; a < k; ++a) {
            std::set<int> s;
            int mode = rnd(r, 0, 4);
            if (mode == 0) s = all_paths(pn.P);
            else if (mode == 1) s.insert(rnd(r, 1, pn.P));
            else if (mode == 2) { s = all_paths(pn.P); s.erase(rnd(r, 1, pn.P)); }
            else {
                double d = mode == 3 ? 0.2 : 0.8;
                for (int p = 1; p <= pn.P; ++p) if (chance(r, d)) s.insert(p);
            }
            if (s.empty()) s.insert(pn.P);  // the max path, sometimes only here
            sets.push_back(s);
        }
        pn.syms.push_back(sym);
        pn.src.push_back(sets);
    }
    pn.enc_seed = r();
    return pn;
}

// ============================================================================
// P4 — msa2eds
// ============================================================================

struct MsaCase {
    std::vector<std::string> rows;  // aligned, '-' = gap; row 0 is the reference
    int width = 60;                 // FASTA line width
    bool trailing_newline = true;
    size_t l = 3;
};

std::string msa_text(const MsaCase& c) {
    std::string s;
    for (size_t i = 0; i < c.rows.size(); ++i) {
        s += ">seq" + std::to_string(i + 1) + "\n";
        const std::string& row = c.rows[i];
        for (size_t j = 0; j < row.size(); j += c.width) {
            s += row.substr(j, c.width);
            if (i + 1 < c.rows.size() || j + c.width < row.size() || c.trailing_newline) s += "\n";
        }
    }
    return s;
}

std::string ungap(const std::string& s) {
    std::string o;
    for (char c : s) if (c != '-') o += c;
    return o;
}

MsaCase gen_msa(Rng& r) {
    MsaCase c;
    int nrows = rnd(r, 1, 7);
    if (chance(r, 0.1)) nrows = rnd(r, 60, 70);
    c.l = rnd(r, 1, 6);
    c.rows.assign(nrows, "");
    int cols = 0, target = rnd(r, 1, 40);
    while (cols < target) {
        int kind = rnd(r, 0, 9);
        int run = rnd(r, 1, kind < 5 ? 2 * static_cast<int>(c.l) + 2 : 3);
        for (int k = 0; k < run; ++k, ++cols) {
            char base = "ACGT"[rnd(r, 0, 3)];
            for (int i = 0; i < nrows; ++i) {
                char ch = base;
                if (kind == 5 && i > 0 && chance(r, 0.5)) ch = "ACGT"[rnd(r, 0, 3)];  // SNP column
                if (kind == 6 && i > 0 && chance(r, 0.5)) ch = '-';                  // deletion
                if (kind == 7) ch = (i == 0 || chance(r, 0.5)) ? '-' : "ACGT"[rnd(r, 0, 3)];  // insertion
                if (kind == 8 && chance(r, 0.3)) ch = '-';                           // gap anywhere
                if (kind == 9) ch = chance(r, 0.4) ? '-' : "ACGT"[rnd(r, 0, 3)];     // chaos
                c.rows[i] += ch;
            }
        }
    }
    c.width = chance(r, 0.5) ? 60 : rnd(r, 1, std::max(1, cols));
    c.trailing_newline = chance(r, 0.7);
    return c;
}

Result check_msa(const MsaCase& c) {
    fs::path dir = scratch_dir();
    spit(dir / "in.msa", msa_text(c));
    const int P = static_cast<int>(c.rows.size());
    for (int leds = 0; leds < 2; ++leds) {
        const std::string tag = leds ? "msa2leds" : "msa2eds";
        Loaded L;
        try {
            Silence quiet;
            {
                std::ifstream in(dir / "in.msa");
                std::ofstream eo(dir / "out.eds"), so(dir / "out.seds");
                if (leds) parse_msa_to_leds_streaming(in, eo, so, c.l);
                else parse_msa_to_eds_streaming(in, eo, so);
            }
            if (Result r = load_with_sources(dir / "out.eds", dir / "out.seds", L, tag)) return r;
        } catch (const std::exception& e) {
            return fail(tag + "/exception", e.what());
        }
        if (Result r = check_sources_shape(L, P, true, tag)) return r;
        // msa2eds makes no canonical-form promise (TODO 0c names vcf2eds and
        // eds2leds only), so only the l-EDS property itself is checked.
        if (leds)
            if (Result r = check_leds_shape(L.syms, c.l, tag, /*require_canonical=*/false)) return r;
        for (int p = 1; p <= P; ++p) {
            auto got = spell(L.syms, L.src, p);
            std::string want = ungap(c.rows[p - 1]);
            if (!got || got->size() != 1 || *got->begin() != want)
                return fail(tag + "/spelling",
                            "row " + std::to_string(p) + " is \"" + want + "\" but its path spells " +
                                (got ? describe_spellings(*got) : std::string("(too many)")));
        }
    }
    return std::nullopt;
}

std::vector<MsaCase> shrink_msa(const MsaCase& c) {
    std::vector<MsaCase> out;
    for (size_t i = 1; i < c.rows.size(); ++i) {
        MsaCase q = c; q.rows.erase(q.rows.begin() + i); out.push_back(q);
    }
    if (c.rows.size() > 1) { MsaCase q = c; q.rows.erase(q.rows.begin()); out.push_back(q); }
    for (size_t j = 0; j < c.rows[0].size() && c.rows[0].size() > 1; ++j) {
        MsaCase q = c;
        for (auto& r : q.rows) r.erase(j, 1);
        out.push_back(q);
    }
    if (c.width != 60) { MsaCase q = c; q.width = 60; out.push_back(q); }
    if (!c.trailing_newline) { MsaCase q = c; q.trailing_newline = true; out.push_back(q); }
    if (c.l > 1) { MsaCase q = c; q.l--; out.push_back(q); }
    return out;
}

std::string msa_repro(const MsaCase& c) {
    std::ostringstream os;
    os << "  l=" << c.l << "  width=" << c.width << "  trailing_newline=" << c.trailing_newline
       << "\n  MSA (rows):\n";
    for (auto& r : c.rows) os << "    " << r << "\n";
    std::string t = msa_text(c);
    std::string esc;
    for (char ch : t) esc += ch == '\n' ? std::string("\\n") : std::string(1, ch);
    os << "  file: printf '" << esc << "' > repro.msa\n"
       << "  CLI : msa2eds -i repro.msa -o repro.eds -s repro.seds   (add -l " << c.l
       << " for l-EDS)\n";
    return os.str();
}

// ============================================================================
// P5 — vcf2eds vs bcftools consensus (haploid)
// ============================================================================

struct VcfRec {
    size_t pos;  // 1-based
    std::string ref;
    std::vector<std::string> alts;
    std::vector<int> gt;  // per sample; -1 = missing
};

struct VcfCase {
    std::string ref;
    int width = 60;
    bool trailing_newline = true;
    std::vector<VcfRec> recs;
    int S = 1;
    size_t l = 3;
    size_t block = 0;       // vcf2eds block size compared against block 0
    int fmt = 0;            // 0 SEDS, 1 SEDS_SPARSE, 2 EDZ, 3 EDZ_SPARSE
};

std::string fasta_text(const VcfCase& c) {
    std::string s = ">chr1\n";
    for (size_t j = 0; j < c.ref.size(); j += c.width) {
        s += c.ref.substr(j, c.width);
        if (j + c.width < c.ref.size() || c.trailing_newline) s += "\n";
    }
    return s;
}

std::string vcf_text(const VcfCase& c) {
    std::ostringstream os;
    os << "##fileformat=VCFv4.2\n##contig=<ID=chr1,length=" << c.ref.size() << ">\n"
       << "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">\n"
       << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT";
    for (int s = 1; s <= c.S; ++s) os << "\tS" << s;
    os << "\n";
    for (auto& r : c.recs) {
        os << "chr1\t" << r.pos << "\t.\t" << r.ref << "\t";
        for (size_t a = 0; a < r.alts.size(); ++a) os << (a ? "," : "") << r.alts[a];
        os << "\t.\tPASS\t.\tGT";
        for (int g : r.gt) os << "\t" << (g < 0 ? std::string(".") : std::to_string(g));
        os << "\n";
    }
    return os.str();
}

bool overlap(const VcfRec& a, const VcfRec& b) {
    size_t as = a.pos - 1, ae = as + a.ref.size(), bs = b.pos - 1, be = bs + b.ref.size();
    return as < be && bs < ae;
}

// vcf2eds's documented rule (merge_variant_group): a copy takes every ALT it is
// called with, except one overlapping an ALT it already took earlier in file
// order. REF and missing calls never block anything.
std::string model_genome(const VcfCase& c, int s) {
    std::vector<const VcfRec*> applied;
    std::vector<int> allele;
    for (auto& r : c.recs) {
        int g = r.gt[s];
        if (g <= 0 || g > static_cast<int>(r.alts.size())) continue;
        bool clash = false;
        for (auto* a : applied) if (overlap(*a, r)) clash = true;
        if (clash) continue;
        applied.push_back(&r);
        allele.push_back(g);
    }
    std::vector<size_t> idx(applied.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return applied[a]->pos < applied[b]->pos; });
    std::string out;
    size_t cur = 0;
    for (size_t i : idx) {
        size_t st = applied[i]->pos - 1;
        out += c.ref.substr(cur, st - cur);
        out += applied[i]->alts[allele[i] - 1];
        cur = st + applied[i]->ref.size();
    }
    return out + c.ref.substr(cur);
}

std::string g_bcftools;
size_t g_bcftools_compared = 0;      // sample genomes compared with bcftools consensus
size_t g_bcftools_divergences = 0;   // ... that differ (each one flagged by vcf2eds, or a failure)
std::string g_divergence_example;
size_t g_flagged_samples = 0;        // samples vcf2eds's overlap check flagged, over all cases
std::string g_unused_example;

std::string mut_allele(Rng& r, const std::string& not_this, int lo, int hi) {
    for (int t = 0; t < 50; ++t) {
        std::string a = rand_dna(r, rnd(r, lo, hi));
        if (a != not_this) return a;
    }
    return not_this == "A" ? "C" : "A";
}

VcfCase gen_vcf(Rng& r) {
    VcfCase c;
    int L = rnd(r, 4, 60);
    c.ref = rand_dna(r, L);
    c.S = chance(r, 0.1) ? rnd(r, 60, 70) : rnd(r, 1, 6);
    c.l = rnd(r, 1, 6);
    c.width = chance(r, 0.4) ? 60 : rnd(r, 1, L);
    c.trailing_newline = chance(r, 0.8);
    int nrec = rnd(r, 0, 9);
    size_t pos = 1;
    for (int k = 0; k < nrec; ++k) {
        if (k > 0) {
            const VcfRec& prev = c.recs.back();
            if (chance(r, 0.45)) pos = prev.pos + rnd(r, 0, static_cast<int>(prev.ref.size()) - 1);  // overlap
            else pos = prev.pos + prev.ref.size() + rnd(r, 0, 6);
        } else {
            pos = rnd(r, 1, std::min(L, 6));
            if (chance(r, 0.2)) pos = 1;
        }
        if (pos > static_cast<size_t>(L)) break;
        size_t room = L - (pos - 1);
        VcfRec v;
        v.pos = pos;
        int type = rnd(r, 0, 5);
        int rl = 1;
        if (type == 1) rl = rnd(r, 2, 3);              // MNP
        else if (type == 2) rl = rnd(r, 2, 7);         // deletion
        else if (type == 4) rl = rnd(r, 1, 4);         // complex
        rl = std::min<int>(rl, room);
        v.ref = c.ref.substr(pos - 1, rl);
        int nalt = chance(r, 0.25) ? rnd(r, 2, 3) : 1;
        for (int a = 0; a < nalt; ++a) {
            std::string alt;
            int t = a == 0 ? type : rnd(r, 0, 5);
            for (int tries = 0; tries < 20; ++tries) {
                if (t == 0) alt = mut_allele(r, v.ref.substr(0, 1), 1, 1) + v.ref.substr(1);
                else if (t == 1) alt = mut_allele(r, v.ref, rl, rl);
                else if (t == 2) alt = v.ref.substr(0, 1);
                else if (t == 3) alt = v.ref + rand_dna(r, rnd(r, 1, 4));
                else alt = mut_allele(r, v.ref, 1, 5);
                if (alt != v.ref && std::find(v.alts.begin(), v.alts.end(), alt) == v.alts.end()) break;
                t = rnd(r, 0, 5);
            }
            if (alt == v.ref || std::find(v.alts.begin(), v.alts.end(), alt) != v.alts.end()) continue;
            v.alts.push_back(alt);
        }
        if (v.alts.empty()) continue;
        double p_alt = chance(r, 0.15) ? 1.0 : 0.4;   // sometimes fixed in the panel
        for (int s = 0; s < c.S; ++s) {
            if (chance(r, 0.08)) v.gt.push_back(-1);
            else if (chance(r, p_alt)) v.gt.push_back(rnd(r, 1, v.alts.size()));
            else v.gt.push_back(0);
        }
        c.recs.push_back(v);
    }
    int b = rnd(r, 0, 4);
    c.block = b == 0 ? 10000000 : b == 1 ? 1 : static_cast<size_t>(rnd(r, 2, L + 2));
    c.fmt = rnd(r, 0, 3);
    return c;
}

Sources::Format vcf_fmt(int f) {
    return f == 0 ? Sources::Format::SEDS : f == 1 ? Sources::Format::SEDS_SPARSE
         : f == 2 ? Sources::Format::EDZ : Sources::Format::EDZ_SPARSE;
}

// bcftools consensus for the given (0-based) samples; genomes[i] is sample samples[i].
bool run_bcftools(const fs::path& dir, const std::vector<int>& samples,
                  std::vector<std::string>& genomes) {
    std::string q = "'" + dir.string() + "'";
    std::string cmd = g_bcftools + " view -Oz -o " + q + "/v.vcf.gz " + q + "/in.vcf >/dev/null 2>&1 && " +
                      g_bcftools + " index -f " + q + "/v.vcf.gz >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) return false;
    genomes.clear();
    for (int s0 : samples) {
        int s = s0 + 1;
        std::string c = g_bcftools + " consensus -f " + q + "/ref.fa -s S" + std::to_string(s) +
                        " -o " + q + "/cons.fa " + q + "/v.vcf.gz >/dev/null 2>&1";
        if (std::system(c.c_str()) != 0) return false;
        std::ifstream in(dir / "cons.fa");
        std::string line, seq;
        while (std::getline(in, line)) if (!line.empty() && line[0] != '>') seq += line;
        genomes.push_back(seq);
    }
    return true;
}

// One vcf2eds run (whole-span or --split-groups) checked against the model:
// identical across --block-size, sources partition the samples, canonical
// form, every sample's genome equals the documented overlap rule, through the
// EDS and the direct VCF -> l-EDS output. Returns the spellings and the
// samples the overlap check flagged.
Result check_vcf_mode(const VcfCase& c, const fs::path& dir, bool split,
                      std::vector<std::string>& genomes, std::set<int>& flagged) {
    const int S = c.S;
    const std::string mode = split ? "vcf2eds-split" : "vcf2eds";
    std::string base_eds, base_seds;
    std::vector<Sym> syms;
    std::vector<std::vector<std::set<int>>> src;
    for (int pass = 0; pass < 2; ++pass) {
        size_t block = pass == 0 ? 0 : c.block;
        std::string ext = c.fmt >= 2 ? ".edz" : ".seds";
        std::string stem = (split ? "split" : "out") + std::to_string(pass);
        fs::path eo = dir / (stem + ".eds");
        fs::path so = dir / (stem + ext);
        VCFStats st;
        try {
            Silence quiet;
            std::istringstream vin(vcf_text(c));
            std::ifstream fin(dir / "ref.fa");
            std::ofstream e(eo), s(so, std::ios::binary);
            parse_vcf_to_eds_streaming(vin, fin, e, s, &st, block, vcf_fmt(c.fmt), split);
        } catch (const std::exception& e) {
            return fail(mode + "/exception", e.what());
        }
        std::set<int> fl;
        for (size_t i : st.overlap_divergent_samples) fl.insert(static_cast<int>(i));
        if (pass == 0) {
            flagged = fl;
            if ((st.overlap_divergent_copies == 0) != fl.empty() ||
                st.overlap_divergence_examples.size() !=
                    std::min<size_t>(st.overlap_divergent_copies, 5))
                return fail(mode + "/overlap-stats", "divergence counters disagree with each other");
            base_eds = slurp(eo); base_seds = slurp(so);
            Loaded L;
            try {
                if (Result r = load_with_sources(eo, so, L, mode)) return r;
            } catch (const std::exception& e) {
                return fail(mode + "/load-exception", e.what());
            }
            if (Result r = check_sources_shape(L, S, true, mode)) return r;
            if (Result r = check_leds_shape(L.syms, 0, mode)) return r;  // canonical form only
            // Text every sample spells is common text, not a one-sided symbol
            // (TODO 0c; for split mode, a segment every copy spells the same).
            for (size_t i = 0; i < L.syms.size(); ++i)
                for (size_t a = 0; a < L.syms[i].size(); ++a)
                    if (L.syms[i].size() > 1 && L.src[i][a].size() == static_cast<size_t>(S))
                        return fail(mode + "/universal-alternative",
                                    "symbol " + std::to_string(i) + " is degenerate but every "
                                    "sample spells \"" + L.syms[i][a] + "\"");
            syms = L.syms; src = L.src;
        } else if (block != 0) {
            if (slurp(eo) != base_eds)
                return fail(mode + "-block/eds-bytes", "EDS differs between block 0 and block " +
                                                            std::to_string(block));
            if (slurp(so) != base_seds)
                return fail(mode + "-block/seds-bytes", "sources differ between block 0 and block " +
                                                             std::to_string(block));
            if (fl != flagged)
                return fail(mode + "-block/overlap-flags", "overlap check flags different samples "
                                                            "at block " + std::to_string(block));
        }
    }
    genomes.assign(S, "");
    for (int s = 0; s < S; ++s) {
        auto got = spell(syms, src, s + 1);
        std::string want = model_genome(c, s);
        if (!got || got->size() != 1 || *got->begin() != want)
            return fail(mode + "/model",
                        "S" + std::to_string(s + 1) + " should be \"" + want + "\" but spells " +
                            (got ? describe_spellings(*got) : std::string("(too many)")));
        genomes[s] = want;
    }
    // direct VCF -> l-EDS
    const std::string ltag = split ? "vcf2leds-split" : "vcf2leds";
    Loaded L;
    try {
        Silence quiet;
        {
            std::istringstream vin(vcf_text(c));
            std::ifstream fin(dir / "ref.fa");
            std::ofstream e(dir / "out.leds"), so(dir / "out.lseds");
            VCFStats st;
            parse_vcf_to_leds_streaming_direct(vin, fin, e, so, c.l, &st, c.block, nullptr,
                                               nullptr, split);
        }
        if (Result r = load_with_sources(dir / "out.leds", dir / "out.lseds", L, ltag)) return r;
    } catch (const std::exception& e) {
        return fail(ltag + "/exception", e.what());
    }
    if (Result r = check_sources_shape(L, S, true, ltag)) return r;
    if (Result r = check_leds_shape(L.syms, c.l, ltag)) return r;
    for (int s = 0; s < S; ++s) {
        auto got = spell(L.syms, L.src, s + 1);
        if (!got || got->size() != 1 || *got->begin() != genomes[s])
            return fail(ltag + "/model",
                        "S" + std::to_string(s + 1) + " should be \"" + genomes[s] + "\" but spells " +
                            (got ? describe_spellings(*got) : std::string("(too many)")));
    }
    return std::nullopt;
}

// `with_bcftools` is off while minimising unless the failure is a bcftools one.
Result check_vcf(const VcfCase& c, bool with_bcftools) {
    fs::path dir = scratch_dir();
    spit(dir / "ref.fa", fasta_text(c));
    spit(dir / "in.vcf", vcf_text(c));
    const int S = c.S;
    std::vector<std::string> whole, split;
    std::set<int> flagged, flagged_split;
    if (Result r = check_vcf_mode(c, dir, false, whole, flagged)) return r;
    if (Result r = check_vcf_mode(c, dir, true, split, flagged_split)) return r;
    if (flagged_split != flagged)
        return fail("vcf2eds-split/overlap-flags",
                    "--split-groups flags samples {" + join_ints(flagged_split) +
                        "}, whole-span {" + join_ints(flagged) + "}");
    // --strict-overlaps refuses exactly when something was flagged.
    {
        bool threw = false;
        try {
            Silence quiet;
            std::istringstream vin(vcf_text(c));
            std::ifstream fin(dir / "ref.fa");
            std::ostringstream e, s;
            parse_vcf_to_eds_streaming(vin, fin, e, s, nullptr, 0, Sources::Format::SEDS, false,
                                       /*strict_overlaps=*/true);
        } catch (const OverlapDivergenceError&) {
            threw = true;
        } catch (const std::exception& e) {
            return fail("vcf2eds-strict/exception", e.what());
        }
        if (threw != !flagged.empty())
            return fail("vcf2eds-strict/refusal", std::string("--strict-overlaps ") +
                                                      (threw ? "refused" : "accepted") +
                                                      " a VCF the check " +
                                                      (flagged.empty() ? "passed" : "flagged"));
    }
    if (!with_bcftools || g_bcftools.empty()) return std::nullopt;
    // Every sample of a small panel; four spread across a large one (each
    // sample is one bcftools process), always including the flagged ones.
    std::set<int> pick_set;
    if (S <= 6) for (int s = 0; s < S; ++s) pick_set.insert(s);
    else pick_set = {0, S / 3, (2 * S) / 3, S - 1};
    for (int s : flagged) if (pick_set.size() < 8) pick_set.insert(s);
    std::vector<int> picked(pick_set.begin(), pick_set.end());
    std::vector<std::string> cons_list;
    if (!run_bcftools(dir, picked, cons_list))
        return fail("bcftools/failed", "bcftools could not process the case");
    for (size_t i = 0; i < picked.size(); ++i) {
        const int s = picked[i];
        const std::string& cons = cons_list[i];
        ++g_bcftools_compared;
        const bool differs = whole[s] != cons;
        const bool is_flagged = flagged.count(s) > 0;
        if (is_flagged) ++g_flagged_samples;
        if (differs && g_bcftools_divergences++ == 0)
            g_divergence_example = "S" + std::to_string(s + 1) + ": vcf2eds \"" + whole[s] +
                                   "\", bcftools \"" + cons + "\"\n" + vcf_text(c);
        // Silent means equal to bcftools; flagged means different. Exactly.
        if (differs && !is_flagged)
            return fail("bcftools/genome", "S" + std::to_string(s + 1) + ": vcf2eds spells \"" +
                                               whole[s] + "\", bcftools consensus gives \"" + cons +
                                               "\", and the overlap check was silent");
        if (!differs && is_flagged)
            return fail("bcftools/false-alarm", "S" + std::to_string(s + 1) +
                                                    " was flagged but vcf2eds and bcftools both "
                                                    "spell \"" + cons + "\"");
    }
    return std::nullopt;
}

std::vector<VcfCase> shrink_vcf(const VcfCase& c) {
    std::vector<VcfCase> out;
    for (size_t i = 0; i < c.recs.size(); ++i) {
        VcfCase q = c; q.recs.erase(q.recs.begin() + i); out.push_back(q);
    }
    for (int s = 0; s < c.S && c.S > 1; ++s) {
        VcfCase q = c; q.S--;
        for (auto& r : q.recs) r.gt.erase(r.gt.begin() + s);
        out.push_back(q);
    }
    for (size_t i = 0; i < c.recs.size(); ++i) {
        for (size_t a = 0; a < c.recs[i].alts.size() && c.recs[i].alts.size() > 1; ++a) {
            VcfCase q = c;
            auto& r = q.recs[i];
            r.alts.erase(r.alts.begin() + a);
            for (int& g : r.gt) { if (g == static_cast<int>(a) + 1) g = 0; else if (g > static_cast<int>(a) + 1) g--; }
            out.push_back(q);
        }
        for (int s = 0; s < c.S; ++s) {
            if (c.recs[i].gt[s] != 0) { VcfCase q = c; q.recs[i].gt[s] = 0; out.push_back(q); }
            if (c.recs[i].gt[s] > 0) { VcfCase q = c; q.recs[i].gt[s] = -1; out.push_back(q); }
        }
    }
    // trim the reference tail beyond the last record
    size_t need = 0;
    for (auto& r : c.recs) need = std::max(need, r.pos - 1 + r.ref.size());
    if (c.ref.size() > std::max<size_t>(need, 1)) {
        VcfCase q = c; q.ref.pop_back(); out.push_back(q);
    }
    // trim the reference head before the first record
    if (!c.ref.empty() && c.ref.size() > 1) {
        bool ok = true;
        for (auto& r : c.recs) if (r.pos <= 1) ok = false;
        if (ok) {
            VcfCase q = c; q.ref.erase(0, 1);
            for (auto& r : q.recs) r.pos--;
            out.push_back(q);
        }
    }
    if (c.width != 60) { VcfCase q = c; q.width = 60; out.push_back(q); }
    if (!c.trailing_newline) { VcfCase q = c; q.trailing_newline = true; out.push_back(q); }
    if (c.block != 10000000) { VcfCase q = c; q.block = 10000000; out.push_back(q); }
    if (c.fmt != 0) { VcfCase q = c; q.fmt = 0; out.push_back(q); }
    if (c.l > 1) { VcfCase q = c; q.l--; out.push_back(q); }
    return out;
}

std::string vcf_repro(const VcfCase& c) {
    std::ostringstream os;
    os << "  l=" << c.l << "  block=" << c.block << "  sources-format=" << c.fmt
       << "  fasta width=" << c.width << " trailing_newline=" << c.trailing_newline << "\n"
       << "  ref.fa: >chr1 " << c.ref << "\n  in.vcf:\n";
    std::istringstream vs(vcf_text(c));
    std::string line;
    while (std::getline(vs, line)) os << "    " << line << "\n";
    for (int s = 0; s < c.S; ++s) os << "  model S" << s + 1 << " = " << model_genome(c, s) << "\n";
    os << "  CLI : vcf2eds -i in.vcf -r ref.fa -o out.eds -b " << c.block << "\n";
    return os.str();
}

// ============================================================================
// Crash isolation
// ============================================================================
//
// Every check runs in a forked child, so a segfault or abort inside the
// library is reported (and minimised) like any other failure instead of
// killing the harness. The bcftools tallies the child updates come back
// through the same pipe. The parent never calls into the library itself, so
// no OpenMP pool exists at fork time.

Result guarded(const std::function<Result()>& f) {
    int fds[2];
    if (pipe(fds) != 0) return f();
    std::cout.flush();
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return f(); }
    if (pid == 0) {
        close(fds[0]);
        Result r = f();
        std::string msg = (r ? "F" : "O") + std::string("\x1f") + (r ? r->kind : "") + "\x1f" +
                          (r ? r->detail : "") + "\x1f" + std::to_string(g_bcftools_compared) +
                          "\x1f" + std::to_string(g_bcftools_divergences) + "\x1f" +
                          g_divergence_example + "\x1f" + std::to_string(g_flagged_samples) +
                          "\x1f" + g_unused_example;
        size_t off = 0;
        while (off < msg.size()) {
            ssize_t w = write(fds[1], msg.data() + off, msg.size() - off);
            if (w <= 0) break;
            off += static_cast<size_t>(w);
        }
        close(fds[1]);
        std::cout.flush();
        _exit(0);
    }
    close(fds[1]);
    std::string msg;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) msg.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status))
        return fail("crash/signal-" + std::to_string(WTERMSIG(status)),
                    std::string("check killed by signal ") + std::to_string(WTERMSIG(status)) +
                        " (" + strsignal(WTERMSIG(status)) + ")");
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= msg.size(); ++i)
        if (i == msg.size() || msg[i] == '\x1f') { parts.push_back(msg.substr(start, i - start)); start = i + 1; }
    if (parts.size() < 8) return fail("crash/no-result", "check exited without a result");
    g_bcftools_compared = std::stoull(parts[3]);
    g_bcftools_divergences = std::stoull(parts[4]);
    g_divergence_example = parts[5];
    g_flagged_samples = std::stoull(parts[6]);
    g_unused_example = parts[7];
    if (parts[0] == "O") return std::nullopt;
    return Failure{parts[1], parts[2]};
}

// ============================================================================
// Driver
// ============================================================================

struct Stats { size_t cases = 0, failures = 0; };
std::map<std::string, Stats> g_stats;
size_t g_total_failures = 0;

// Only the first few failures of each kind are minimised and printed in full
// (TRANSFORM_FUZZ_MAX_REPORTS, default 2); the rest are listed by seed.
std::map<std::string, size_t> g_reported;
bool should_report(const std::string& prop, const Failure& f, uint64_t seed) {
    static const size_t cap = std::getenv("TRANSFORM_FUZZ_MAX_REPORTS")
        ? std::strtoull(std::getenv("TRANSFORM_FUZZ_MAX_REPORTS"), nullptr, 10) : 2;
    if (g_reported[f.kind]++ < cap) return true;
    std::cout << "[FAIL] " << prop << "  seed=" << seed << "  kind=" << f.kind
              << "  (not minimised: kind already reported)\n";
    return false;
}

template <class Case>
void report(const std::string& prop, uint64_t seed, const Failure& f, const Case& original,
            const Case& minimal, const std::function<std::string(const Case&)>& repro,
            const Failure& minimal_failure) {
    if (std::getenv("TRANSFORM_FUZZ_VERBOSE"))
        std::cout << "\n[original case] " << prop << " seed=" << seed << "\n" << repro(original);
    std::cout << "\n[FAIL] " << prop << "  seed=" << seed << "  kind=" << f.kind << "\n"
              << "  first seen: " << f.detail << "\n"
              << "  minimised : " << minimal_failure.detail << "\n"
              << repro(minimal)
              << "  rerun: TRANSFORM_FUZZ_CASE=" << prop << ":" << seed << " ./test_transform_fuzz\n";
}

template <class Case>
void run_case(const std::string& prop, uint64_t seed, const Case& c,
              const std::function<Result(const Case&)>& check,
              const std::function<std::vector<Case>(const Case&)>& shrinks,
              const std::function<std::string(const Case&)>& repro) {
    g_stats[prop].cases++;
    std::function<Result(const Case&)> gcheck = [&check](const Case& q) {
        return guarded([&] { return check(q); });
    };
    Result r = gcheck(c);
    if (!r) return;
    g_stats[prop].failures++;
    g_total_failures++;
    if (!should_report(prop, *r, seed)) return;
    Case m = minimise<Case>(c, r->kind, gcheck, shrinks);
    Result mr = gcheck(m);
    report<Case>(prop, seed, *r, c, m, repro, mr ? *mr : *r);
}

// One case of property `prop` from `seed`.
void run_one(const std::string& prop, uint64_t seed) {
    Rng r(seed);
    if (prop == "linear" || prop == "het") {
        Panel pn = gen_panel(r, true, prop == "linear", 9, 4);
        run_case<Panel>(prop, seed, pn, check_linear,
                        [](const Panel& p) { return shrink_panel(p, true); },
                        [prop](const Panel& p) { return panel_repro(p, prop, true); });
    } else if (prop == "cartesian") {
        Panel pn = gen_panel(r, false, true, 8, 3);
        run_case<Panel>(prop, seed, pn, check_cartesian,
                        [](const Panel& p) { return shrink_panel(p, false); },
                        [prop](const Panel& p) { return panel_repro(p, prop, false); });
    } else if (prop == "block") {
        bool lin = chance(r, 0.5);
        Panel pn = gen_panel(r, lin, true, 14, 3);
        size_t bytes = full_text(pn.syms).size();
        int k = rnd(r, 0, 3);
        pn.block_bytes = k == 0 ? 1 : k == 3 ? bytes + 100 : rnd(r, 1, std::max<int>(1, bytes));
        pn.canonical = chance(r, 0.7);
        if (pn.canonical && chance(r, 0.8)) pn.fmt = SrcFormat::SEDS_TEXT;
        std::string name = lin ? "block-linear" : "block-cartesian";
        run_case<Panel>(name, seed, pn,
                        [lin](const Panel& p) { return check_block(p, lin); },
                        [lin](const Panel& p) {
                            auto v = shrink_panel(p, lin);
                            for (auto& q : v) q.block_bytes = std::max<uint64_t>(1, q.block_bytes);
                            if (p.block_bytes > 1) { Panel q = p; q.block_bytes = 1; v.push_back(q); }
                            return v;
                        },
                        [name, lin](const Panel& p) { return panel_repro(p, name, lin); });
    } else if (prop == "msa") {
        MsaCase c = gen_msa(r);
        run_case<MsaCase>(prop, seed, c, check_msa, shrink_msa, msa_repro);
    } else if (prop == "vcf") {
        VcfCase c = gen_vcf(r);
        bool bt = true;
        g_stats[prop].cases++;
        Result res = guarded([&] { return check_vcf(c, bt); });
        if (!res) return;
        g_stats[prop].failures++;
        g_total_failures++;
        if (!should_report(prop, *res, seed)) return;
        bool needs_bt = res->kind.rfind("bcftools", 0) == 0;
        std::function<Result(const VcfCase&)> chk = [needs_bt](const VcfCase& q) {
            return guarded([&] { return check_vcf(q, needs_bt); });
        };
        size_t saved_div = g_bcftools_divergences, saved_cmp = g_bcftools_compared,
               saved_flg = g_flagged_samples;
        VcfCase m = minimise<VcfCase>(c, res->kind, chk, shrink_vcf, needs_bt ? 600 : 4000);
        Result mr = chk(m);
        g_bcftools_divergences = saved_div; g_bcftools_compared = saved_cmp;
        g_flagged_samples = saved_flg;
        report<VcfCase>(prop, seed, *res, c, m, vcf_repro, mr ? *mr : *res);
    } else if (prop == "roundtrip") {
        Panel pn = gen_panel(r, chance(r, 0.5), chance(r, 0.5), 12, 4);
        run_case<Panel>("roundtrip-eds", seed, pn, check_eds_roundtrip,
                        [](const Panel& p) { return shrink_panel(p, false); },
                        [](const Panel& p) { return panel_repro(p, "roundtrip-eds", false); });
        Panel sp = gen_sources_panel(r);
        run_case<Panel>("roundtrip-sources", seed, sp, check_sources_roundtrip,
                        [](const Panel& p) { return shrink_panel(p, true); },
                        [](const Panel& p) {
                            return "  paths=" + std::to_string(p.P) + "\n  SEDS (hand-encoded): " +
                                   seds_text(p, false) + "\n  sets: " + plain_seds(p) + "\n";
                        });
    }
}

uint64_t env_u64(const char* name, uint64_t dflt) {
    const char* v = std::getenv(name);
    return v ? std::strtoull(v, nullptr, 10) : dflt;
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/edsparser_transform_fuzz_XXXXXX";
    if (!mkdtemp(tmpl)) { std::perror("mkdtemp"); return 1; }
    g_tmp_root = tmpl;

    // bcftools: resolve once, report clearly if absent.
    {
        const char* b = std::getenv("TRANSFORM_FUZZ_BCFTOOLS");
        std::string cand = b ? b : "bcftools";
        std::string cmd = "'" + cand + "' --version >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) g_bcftools = "'" + cand + "'";
        else if (!b && std::system("/usr/bin/bcftools --version >/dev/null 2>&1") == 0)
            g_bcftools = "/usr/bin/bcftools";
    }

    const uint64_t base = env_u64("TRANSFORM_FUZZ_SEED", 20261001);
    const double iters = std::getenv("TRANSFORM_FUZZ_ITERS")
                             ? std::atof(std::getenv("TRANSFORM_FUZZ_ITERS")) : 1.0;
    const uint64_t soak = env_u64("TRANSFORM_FUZZ_SOAK", 0);
    std::set<std::string> only;
    if (const char* o = std::getenv("TRANSFORM_FUZZ_ONLY")) {
        std::stringstream ss(o);
        std::string t;
        while (std::getline(ss, t, ',')) only.insert(t);
    }

    // Default case counts keep the ctest run around 20 s.
    const std::vector<std::pair<std::string, int>> props = {
        {"linear", 500}, {"het", 200}, {"cartesian", 400}, {"block", 300},
        {"msa", 400}, {"vcf", 250}, {"roundtrip", 300}};
    std::map<std::string, int> prop_id;
    for (size_t i = 0; i < props.size(); ++i) prop_id[props[i].first] = i + 1;

    std::cout << "test_transform_fuzz  base seed " << base << "  (TRANSFORM_FUZZ_SEED)\n";
    if (g_bcftools.empty())
        std::cout << "SKIPPED: vcf2eds vs bcftools consensus — bcftools not found "
                     "(set TRANSFORM_FUZZ_BCFTOOLS). vcf2eds is still checked against the model.\n";
    else
        std::cout << "bcftools: " << g_bcftools << "\n";

    auto t0 = std::chrono::steady_clock::now();
    if (const char* one = std::getenv("TRANSFORM_FUZZ_CASE")) {
        std::string s(one);
        auto colon = s.find(':');
        std::string prop = s.substr(0, colon);
        if (prop == "roundtrip-eds" || prop == "roundtrip-sources") prop = "roundtrip";
        if (prop == "block-linear" || prop == "block-cartesian") prop = "block";
        run_one(prop, std::strtoull(s.c_str() + colon + 1, nullptr, 10));
    } else {
        for (uint64_t round = 0;; ++round) {
            for (auto& [prop, n] : props) {
                if (!only.empty() && !only.count(prop)) continue;
                int count = std::max(1, static_cast<int>(n * iters));
                auto tp = std::chrono::steady_clock::now();
                for (int i = 0; i < count; ++i) {
                    uint64_t seed = base + (static_cast<uint64_t>(prop_id[prop]) << 40) +
                                    round * 1000003ULL + i;
                    run_one(prop, seed);
                }
                if (round == 0)
                    std::cout << "  " << prop << ": " << count << " cases, "
                              << std::chrono::duration<double>(std::chrono::steady_clock::now() - tp).count()
                              << " s\n";
            }
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (!soak || el >= soak) break;
        }
    }

    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "\nSummary (" << el << " s):\n";
    for (auto& [k, s] : g_stats)
        std::cout << "  " << k << ": " << s.cases << " cases, " << s.failures << " failures\n";
    if (!g_bcftools.empty())
        std::cout << "  bcftools: " << g_bcftools_compared << " sample genomes compared; "
                  << g_bcftools_divergences << " differ, every one flagged by vcf2eds's overlap "
                     "check, and none it flagged agree (" << g_flagged_samples << " flagged)\n";
    if (g_bcftools_divergences && std::getenv("TRANSFORM_FUZZ_VERBOSE"))
        std::cout << "  first divergence:\n" << g_divergence_example;

    fs::remove_all(g_tmp_root);
    if (g_total_failures) std::cout << "\nFAILED: " << g_total_failures << " failing case(s)\n";
    else std::cout << "\nAll transform fuzz properties PASSED\n";
    std::cout.flush();
    assert(g_total_failures == 0);
    return g_total_failures ? 1 : 0;
}
