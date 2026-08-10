// SPDX-License-Identifier: MIT
// cpp-sched-watch — Linux scheduler & context-switch monitor (C++23)
// Reads /proc/stat, /proc/schedstat, /proc/loadavg and /proc/uptime to show
// scheduler pressure: load averages, context switches, process forks, run
// queue and per-CPU runnable/blocked time.
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o sched-watch
// Usage: ./sched-watch [--watch N] [--json] [--top N] [--no-color] [--selftest] [--help]

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace schedwatch {

// ── ANSI palette (Catppuccin-ish) ──────────────────────────────────────────
constexpr const char* kRed    = "\x1b[31m";
constexpr const char* kGreen  = "\x1b[32m";
constexpr const char* kYellow = "\x1b[33m";
constexpr const char* kBlue   = "\x1b[34m";
constexpr const char* kMagenta = "\x1b[35m";
constexpr const char* kCyan   = "\x1b[36m";
constexpr const char* kBold   = "\x1b[1m";
constexpr const char* kDim    = "\x1b[2m";
constexpr const char* kReset  = "\x1b[0m";

constexpr const char* RED    = kRed;
constexpr const char* GREEN  = kGreen;
constexpr const char* YEL    = kYellow;
constexpr const char* BLUE   = kBlue;
constexpr const char* MAG    = kMagenta;
constexpr const char* CYAN   = kCyan;
constexpr const char* BOLD   = kBold;
constexpr const char* DIM    = kDim;
constexpr const char* RST    = kReset;

bool g_color = true;

// ── Types ──────────────────────────────────────────────────────────────────
struct LoadAvg {
  double load1 = 0, load5 = 0, load15 = 0;
  long running = 0, total = 0, last_pid = 0;
};

struct ProcStat {
  unsigned long long ctxt = 0;       // context switches since boot
  unsigned long long processes = 0;  // forks since boot
  unsigned long long btime = 0;      // boot time (epoch seconds)
  long procs_running = 0;            // tasks in R state
  long procs_blocked = 0;            // tasks in D state
};

struct CpuSched {
  std::string cpu;             // "cpu0"
  int index = 0;
  unsigned long long runnable_ns = 0;  // time with runnable tasks queued
  unsigned long long blocked_ns = 0;   // time with blocked tasks waiting
  unsigned long long switches = 0;     // scheduler switches
  double runnable_pct = 0;             // runnable_ns / uptime
  double blocked_pct = 0;              // blocked_ns / uptime
  double switches_per_s = 0;
};

struct Snapshot {
  LoadAvg load;
  ProcStat stat;
  double uptime_s = 0;
  std::vector<CpuSched> cpus;
};

// ── Readers ────────────────────────────────────────────────────────────────
static bool read_line(const std::string& path, std::string& out) {
  std::ifstream f(path);
  if (!f.is_open()) return false;
  std::getline(f, out);
  return !out.empty();
}

static LoadAvg read_loadavg() {
  LoadAvg l;
  std::string line;
  if (!read_line("/proc/loadavg", line)) return l;
  std::istringstream ss(line);
  std::string run_total;
  ss >> l.load1 >> l.load5 >> l.load15 >> run_total >> l.last_pid;
  auto slash = run_total.find('/');
  if (slash != std::string::npos) {
    l.running = std::atol(run_total.substr(0, slash).c_str());
    l.total = std::atol(run_total.substr(slash + 1).c_str());
  }
  return l;
}

static ProcStat read_proc_stat() {
  ProcStat ps;
  std::ifstream f("/proc/stat");
  if (!f.is_open()) return ps;
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    std::string key;
    unsigned long long v = 0;
    ss >> key >> v;
    if (key == "ctxt") ps.ctxt = v;
    else if (key == "processes") ps.processes = v;
    else if (key == "procs_running") ps.procs_running = static_cast<long>(v);
    else if (key == "procs_blocked") ps.procs_blocked = static_cast<long>(v);
    else if (key == "btime") ps.btime = v;
  }
  return ps;
}

// /proc/schedstat per-CPU line (Linux 6.x): 9-11 numeric fields.
// fields[6] = runnable_ns, fields[7] = blocked_ns, fields[8] = nr_switches
// (positional layout is stable across 9- and 11-field kernels).
static std::vector<CpuSched> read_schedstat() {
  std::vector<CpuSched> cpus;
  std::ifstream f("/proc/schedstat");
  if (!f.is_open()) return cpus;
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    std::string key;
    ss >> key;
    if (key.size() < 4 || !key.starts_with("cpu") || !std::isdigit(static_cast<unsigned char>(key[3])))
      continue;  // skip version/timestamp/domain lines
    std::vector<unsigned long long> fields;
    unsigned long long v;
    while (ss >> v) fields.push_back(v);
    if (fields.size() < 9) continue;
    CpuSched c;
    c.cpu = key;
    c.index = std::atoi(key.c_str() + 3);
    c.runnable_ns = fields[6];
    c.blocked_ns = fields[7];
    c.switches = fields[8];
    cpus.push_back(std::move(c));
  }
  return cpus;
}

static double read_uptime_s() {
  std::string line;
  if (!read_line("/proc/uptime", line)) return 0.0;
  std::istringstream ss(line);
  double up = 0, idle = 0;
  ss >> up >> idle;
  return up;
}

// ── Derived metrics ────────────────────────────────────────────────────────
struct Rates {
  double ctxt_per_s = 0;   // context switches / second
  double forks_per_s = 0;  // process forks / second
  double runnable_pct = 0; // avg runnable time across CPUs (0-100)
  double blocked_pct = 0;
};

static double pct_of_ns(unsigned long long ns, double uptime_s) {
  if (uptime_s <= 0) return 0.0;
  return std::min(100.0, static_cast<double>(ns) / 1e9 / uptime_s * 100.0);
}

static void derive(Snapshot& s) {
  for (auto& c : s.cpus) {
    c.runnable_pct = pct_of_ns(c.runnable_ns, s.uptime_s);
    c.blocked_pct = pct_of_ns(c.blocked_ns, s.uptime_s);
    c.switches_per_s = s.uptime_s > 0 ? static_cast<double>(c.switches) / s.uptime_s : 0.0;
  }
}

static Rates boot_rates(const Snapshot& s) {
  Rates r;
  if (s.uptime_s > 0) {
    r.ctxt_per_s = static_cast<double>(s.stat.ctxt) / s.uptime_s;
    r.forks_per_s = static_cast<double>(s.stat.processes) / s.uptime_s;
  }
  if (!s.cpus.empty()) {
    double rsum = 0, bsum = 0;
    for (const auto& c : s.cpus) { rsum += c.runnable_pct; bsum += c.blocked_pct; }
    r.runnable_pct = rsum / static_cast<double>(s.cpus.size());
    r.blocked_pct = bsum / static_cast<double>(s.cpus.size());
  }
  return r;
}

static Rates delta_rates(const Snapshot& a, const Snapshot& b, double dt_s) {
  Rates r;
  if (dt_s <= 0) return r;
  r.ctxt_per_s = static_cast<double>(b.stat.ctxt - a.stat.ctxt) / dt_s;
  r.forks_per_s = static_cast<double>(b.stat.processes - a.stat.processes) / dt_s;
  return r;
}

// ── Formatting ────────────────────────────────────────────────────────────
static std::string fmt_count(unsigned long long n) {
  char buf[32];
  if (n >= 1'000'000'000ULL)      std::snprintf(buf, sizeof buf, "%.2fG", n / 1e9);
  else if (n >= 1'000'000ULL)     std::snprintf(buf, sizeof buf, "%.1fM", n / 1e6);
  else if (n >= 1'000ULL)         std::snprintf(buf, sizeof buf, "%.1fK", n / 1e3);
  else                            std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(n));
  return buf;
}

static std::string fmt_duration(double seconds) {
  long long s = static_cast<long long>(seconds);
  long long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%lldd %02lld:%02lld:%02lld", d, h, m, sec);
  return buf;
}

static std::string fmt_boot_time(unsigned long long btime) {
  std::time_t t = static_cast<std::time_t>(btime);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tm);
  return buf;
}

// ── Rendering ─────────────────────────────────────────────────────────────
static const char* pct_color(double pct) {
  return pct > 80.0 ? RED : (pct > 50.0 ? YEL : GREEN);
}

static void print_snapshot(const Snapshot& s, const Rates& r, std::string_view when) {
  std::println(std::cout, "{}{}{} — /proc/stat + /proc/schedstat + /proc/loadavg  ({})",
               BOLD, "Scheduler Watch", RST, when);
  std::println(std::cout, "{}-------------------------------------------------------------------------------{}", DIM, RST);

  // Lines with >9 args are composed with snprintf (GCC 14 <print> constexpr limit)
  char buf[256];
  std::snprintf(buf, sizeof buf,
    "%s%-18s%s%s1min %.2f%s  %s5min %.2f%s  %s15min %.2f%s   %s%ld/%ld tasks%s  last pid %ld",
    CYAN, "LOAD AVERAGE", RST,
    DIM, s.load.load1, RST, DIM, s.load.load5, RST, DIM, s.load.load15, RST,
    DIM, s.load.running, s.load.total, DIM, s.load.last_pid);
  std::println(std::cout, "{}", buf);

  std::println(std::cout, "{}{:<18}{} total {}{}{}   avg {}{:.1f}/s{}",
               CYAN, "CONTEXT SWITCHES", RST,
               BOLD, fmt_count(s.stat.ctxt), RST,
               BOLD, r.ctxt_per_s, RST);

  std::println(std::cout, "{}{:<18}{} total {}{}{}   avg {}{:.1f}/s{}",
               CYAN, "PROCESS FORKS", RST,
               BOLD, fmt_count(s.stat.processes), RST,
               BOLD, r.forks_per_s, RST);

  std::snprintf(buf, sizeof buf,
    "%s%-18s%s running %s%ld%s   blocked %s%ld%s   %savg runnable %s%.1f%%%s  blocked %s%.1f%%%s",
    CYAN, "RUN QUEUE", RST,
    BOLD, s.stat.procs_running, RST,
    BOLD, s.stat.procs_blocked, RST,
    DIM, BOLD, r.runnable_pct, RST, DIM, r.blocked_pct, RST);
  std::println(std::cout, "{}", buf);

  std::println(std::cout, "{}{:<18}{} {}{}   (uptime {})",
               CYAN, "BOOT TIME", RST,
               BOLD, fmt_boot_time(s.stat.btime),
               RST + fmt_duration(s.uptime_s));

  std::println(std::cout);
  std::println(std::cout, "{}{}{}  (runnable + blocked queue time since boot)",
               BOLD, "PER-CPU SCHEDULER PRESSURE", RST);
  std::println(std::cout, "{}{:<6} {:>9} {:>9} {:>10} {:>9}{}",
               DIM, "CPU", "RUNNABLE", "BLOCKED", "SWITCHES", "SW/s", RST);
  for (const auto& c : s.cpus) {
    char row[160];
    std::snprintf(row, sizeof row,
      "%s%-6s%s %s%7.1f%%%s %s%7.1f%%%s %10s %8.1f",
      BOLD, c.cpu.c_str(), RST,
      pct_color(c.runnable_pct), c.runnable_pct, RST,
      pct_color(c.blocked_pct), c.blocked_pct, RST,
      fmt_count(c.switches).c_str(), c.switches_per_s);
    std::println(std::cout, "{}", row);
  }
  std::println(std::cout, "{}cpus: {}{}", DIM, s.cpus.size(), RST);
}

// ── JSON output ───────────────────────────────────────────────────────────
static void print_json(const Snapshot& s, const Rates& r) {
  std::string out = "{\n";
  char buf[256];
  std::snprintf(buf, sizeof buf, "  \"timestamp\": %llu,\n",
                static_cast<unsigned long long>(std::time(nullptr)));
  out += buf;
  std::snprintf(buf, sizeof buf, "  \"uptime_s\": %.1f,\n", s.uptime_s);
  out += buf;
  std::snprintf(buf, sizeof buf,
    "  \"loadavg\": {\"load1\":%.2f,\"load5\":%.2f,\"load15\":%.2f,"
    "\"running\":%ld,\"total\":%ld,\"last_pid\":%ld},\n",
    s.load.load1, s.load.load5, s.load.load15, s.load.running, s.load.total, s.load.last_pid);
  out += buf;
  std::snprintf(buf, sizeof buf,
    "  \"counters\": {\"ctxt\":%llu,\"forks\":%llu,\"procs_running\":%ld,"
    "\"procs_blocked\":%ld,\"btime\":%llu},\n",
    s.stat.ctxt, s.stat.processes, s.stat.procs_running, s.stat.procs_blocked, s.stat.btime);
  out += buf;
  std::snprintf(buf, sizeof buf,
    "  \"rates\": {\"ctxt_per_s\":%.1f,\"forks_per_s\":%.1f,"
    "\"avg_runnable_pct\":%.1f,\"avg_blocked_pct\":%.1f},\n",
    r.ctxt_per_s, r.forks_per_s, r.runnable_pct, r.blocked_pct);
  out += buf;
  out += "  \"cpus\": [\n";
  for (size_t i = 0; i < s.cpus.size(); ++i) {
    const auto& c = s.cpus[i];
    std::snprintf(buf, sizeof buf,
      "    {\"cpu\":\"%s\",\"runnable_pct\":%.1f,\"blocked_pct\":%.1f,"
      "\"switches\":%llu,\"switches_per_s\":%.1f}%s\n",
      c.cpu.c_str(), c.runnable_pct, c.blocked_pct, c.switches, c.switches_per_s,
      (i + 1 < s.cpus.size() ? "," : ""));
    out += buf;
  }
  out += "  ],\n";
  std::snprintf(buf, sizeof buf,
    "  \"summary\": {\"cpus\":%zu,\"avg_runnable_pct\":%.1f,\"avg_blocked_pct\":%.1f}\n",
    s.cpus.size(), r.runnable_pct, r.blocked_pct);
  out += buf;
  out += "}\n";
  std::cout << out;
}

// ── Self tests ────────────────────────────────────────────────────────────
static void print_help();  // defined below (free function ordering)

static int g_pass = 0, g_fail = 0;
static void t_check(bool ok, const std::string& name) {
  if (ok) { ++g_pass; std::println(std::cout, "  {}ok{}  {}", GREEN, RST, name); }
  else    { ++g_fail; std::println(std::cout, "  {}FAIL{} {}", RED, RST, name); }
}

static int run_selftest() {
  std::println(std::cout, "sched-watch --selftest");

  auto load = read_loadavg();
  t_check(load.load1 >= 0.0 && std::isfinite(load.load1), "parsea /proc/loadavg real (load1 >= 0)");
  t_check(load.running >= 1 && load.running <= load.total,
          "run queue coherente: 1 <= running <= total");
  t_check(load.last_pid > 0, "last pid > 0");

  auto ps = read_proc_stat();
  t_check(ps.ctxt > 0, "ctxt (context switches) > 0");
  t_check(ps.processes > 0, "processes (forks) > 0");
  t_check(ps.procs_running >= 0, "procs_running >= 0");
  std::time_t now = std::time(nullptr);
  t_check(ps.btime > 0 && static_cast<long long>(ps.btime) < now + 86400LL,
          "btime en el pasado (epoch valido)");

  auto cpus = read_schedstat();
  t_check(!cpus.empty(), "parsea /proc/schedstat: al menos 1 CPU");
  bool all_runnable = std::all_of(cpus.begin(), cpus.end(),
                                  [](const CpuSched& c) { return c.runnable_ns > 0; });
  t_check(all_runnable, "todas las CPUs con runnable_ns > 0");
  bool all_switches = std::all_of(cpus.begin(), cpus.end(),
                                  [](const CpuSched& c) { return c.switches > 0; });
  t_check(all_switches, "todas las CPUs con nr_switches > 0");

  double up = read_uptime_s();
  t_check(up > 0, "uptime /proc/uptime > 0");

  Snapshot s;
  s.load = load; s.stat = ps; s.uptime_s = up; s.cpus = cpus;
  derive(s);
  bool pct_ok = std::all_of(s.cpus.begin(), s.cpus.end(),
                            [](const CpuSched& c) { return c.runnable_pct > 0.0 && c.runnable_pct <= 100.0; });
  t_check(pct_ok, "runnable_pct derivado en (0, 100]");

  // Formatting
  t_check(fmt_count(2'826'895'041ULL) == "2.83G", "fmt_count 2826895041 -> 2.83G");
  t_check(fmt_count(7'138'853ULL) == "7.1M", "fmt_count 7138853 -> 7.1M");
  t_check(fmt_count(400'792'491ULL) == "400.8M", "fmt_count 400792491 -> 400.8M");
  t_check(fmt_duration(2'447'338.0) == "28d 07:48:58", "fmt_duration 2447338s -> 28d 07:48:58");
  t_check(fmt_boot_time(1'783'884'256ULL) == "2026-07-12 19:24:16 UTC",
          "fmt_boot_time 1783884256 -> 2026-07-12 19:24:16 UTC");

  // Derived rate math
  Snapshot a, b;
  a.stat.ctxt = 100; b.stat.ctxt = 200; a.stat.processes = 10; b.stat.processes = 16;
  auto r = delta_rates(a, b, 2.0);
  t_check(std::abs(r.ctxt_per_s - 50.0) < 1e-9, "ctxt delta: 100 switches / 2s = 50.0/s");
  t_check(std::abs(r.forks_per_s - 3.0) < 1e-9, "forks delta: 6 forks / 2s = 3.0/s");

  t_check(std::abs(pct_of_ns(10'000'000'000ULL, 100.0) - 10.0) < 1e-9,
          "pct_of_ns: 10e9 ns en 100s = 10.0%");
  t_check(std::abs(pct_of_ns(500'000'000'000ULL, 100.0) - 100.0) < 1e-9,
          "pct_of_ns clamp: 500e9 ns en 100s -> 100.0% (no 500)");

  // JSON structural validation
  Rates jr = boot_rates(s);
  std::string json;
  {
    std::ostringstream captured;
    auto* old = std::cout.rdbuf(captured.rdbuf());
    print_json(s, jr);
    std::cout.rdbuf(old);
    json = captured.str();
  }
  int depth = 0; bool ok_braces = true;
  bool in_str = false, esc = false;
  for (char ch : json) {
    if (in_str) {
      if (esc) esc = false;
      else if (ch == '\\') esc = true;
      else if (ch == '"') in_str = false;
      continue;
    }
    if (ch == '"') in_str = true;
    else if (ch == '{' || ch == '[') ++depth;
    else if (ch == '}' || ch == ']') { --depth; if (depth < 0) { ok_braces = false; break; } }
  }
  t_check(ok_braces && depth == 0, "JSON: llaves/corchetes balanceados");
  t_check(json.find("\"loadavg\"") != std::string::npos &&
          json.find("\"cpus\"") != std::string::npos &&
          json.find("\"summary\"") != std::string::npos,
          "JSON: claves con comillas y secciones loadavg/cpus/summary");

  // --help output contains usage
  std::string help_out;
  {
    std::ostringstream captured;
    auto* old = std::cout.rdbuf(captured.rdbuf());
    print_help();
    std::cout.rdbuf(old);
    help_out = captured.str();
  }
  t_check(help_out.find("Usage: ./sched-watch") != std::string::npos, "--help documenta Usage");

  std::println(std::cout, "RESULT: {} passed, {} failed", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

// ── CLI ───────────────────────────────────────────────────────────────────
static void print_help() {
  std::println(std::cout, "{}{}{} — Linux scheduler & context-switch monitor",
               BOLD, "sched-watch", RST);
  std::println(std::cout, "Reads /proc/stat, /proc/schedstat, /proc/loadavg, /proc/uptime");
  std::println(std::cout, "Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o sched-watch");
  std::println(std::cout, "Usage: ./sched-watch [options]");
  std::println(std::cout, "  -w, --watch N    refresh every N seconds (delta rates)");
  std::println(std::cout, "  -j, --json       JSON output (single snapshot)");
  std::println(std::cout, "  -t, --top N      show only top N CPUs by runnable pressure");
  std::println(std::cout, "  -c, --no-color   disable ANSI colors");
  std::println(std::cout, "  -h, --help       this help");
  std::println(std::cout, "      --selftest   run self-verification checks");
}

static Snapshot take_snapshot() {
  Snapshot s;
  s.load = read_loadavg();
  s.stat = read_proc_stat();
  s.uptime_s = read_uptime_s();
  s.cpus = read_schedstat();
  derive(s);
  return s;
}

int main(int argc, char** argv) {
  bool watch = false, json = false, selftest = false;
  int interval = 2, top = 0;

  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    if ((a == "-w" || a == "--watch") && i + 1 < argc) { interval = std::stoi(argv[++i]); watch = true; }
    else if (a == "-j" || a == "--json") json = true;
    else if ((a == "-t" || a == "--top") && i + 1 < argc) top = std::stoi(argv[++i]);
    else if (a == "-c" || a == "--no-color") g_color = false;
    else if (a == "-h" || a == "--help") { print_help(); return 0; }
    else if (a == "--selftest") selftest = true;
    else {
      std::println(std::cerr, "error: unknown option {}", a);
      print_help();
      return 2;
    }
  }

  if (selftest) return run_selftest();

  if (watch) {
    auto prev = take_snapshot();
    auto t0 = std::chrono::steady_clock::now();
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(interval));
      auto now = take_snapshot();
      double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      t0 = std::chrono::steady_clock::now();
      auto r = delta_rates(prev, now, dt);
      if (g_color) std::cout << "\x1b[2J\x1b[H";
      std::string when = "rates: last " + std::to_string(interval) + "s";
      print_snapshot(now, r, when);
      std::cout << std::flush;
      prev = std::move(now);
    }
  }

  auto s = take_snapshot();
  auto r = boot_rates(s);

  if (top > 0 && static_cast<size_t>(top) < s.cpus.size()) {
    std::vector<size_t> idx(s.cpus.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(),
              [&](size_t x, size_t y) { return s.cpus[x].runnable_pct > s.cpus[y].runnable_pct; });
    idx.resize(top);
    std::vector<CpuSched> nc;
    for (size_t i : idx) nc.push_back(s.cpus[i]);
    s.cpus = std::move(nc);
  }

  if (json) {
    print_json(s, r);
    return 0;
  }

  std::string when = "rates: avg since boot, uptime " + fmt_duration(s.uptime_s);
  print_snapshot(s, r, when);
  return 0;
}

}  // namespace schedwatch

int main(int argc, char** argv) { return schedwatch::main(argc, argv); }
