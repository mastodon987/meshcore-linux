#include "HashRules.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sys/stat.h>

HashRules& HashRules::instance() {
  static HashRules inst;
  return inst;
}

static void ensure_state_dir_exists() {
  const char* dir = "/var/lib/meshcore";
  struct stat st;
  if (stat(dir, &st) != 0) {
    mkdir(dir, 0755);
  }
}

int HashRules::loadFromFile(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return -1;
  _rules.clear();
  std::string line;
  while (std::getline(f, line)) {
    auto pos = line.find('#');
    if (pos != std::string::npos) line.erase(pos);
    auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) continue;
    auto end = line.find_last_not_of(" \t\r\n");
    std::string tok = line.substr(start, end - start + 1);
    if (tok.empty()) continue;

    std::istringstream iss(tok);
    std::string pattern, action;
    iss >> pattern >> action;
    if (pattern.empty() || action.empty()) continue;
    Rule r;
    r.pattern = toLower(pattern);
    r.action = (toLower(action) == "drop") ? Action::Drop : Action::Allow;
    r.pph = r.p24h = r.burst = 0;
    r.penalty_secs = 0;
    std::string opt;
    while (iss >> opt) {
      auto eq = opt.find('=');
      if (eq == std::string::npos) continue;
      std::string k = toLower(opt.substr(0, eq));
      std::string v = opt.substr(eq + 1);
      if (k == "pph") r.pph = (uint32_t)atoi(v.c_str());
      else if (k == "p24h") r.p24h = (uint32_t)atoi(v.c_str());
      else if (k == "burst") r.burst = (uint32_t)atoi(v.c_str());
      else if (k == "penalty_secs") r.penalty_secs = (uint32_t)atoi(v.c_str());
    }
    _rules.push_back(r);
  }

  // try to load persisted counters if present
  ensure_state_dir_exists();
  std::string state_path = "/var/lib/meshcore/hash-rules.state";
  std::ifstream sf(state_path);
  if (sf.is_open()) {
    std::string ln;
    while (std::getline(sf, ln)) {
      if (ln.empty()) continue;
      // format: pattern:origin:ts,ts,ts
      auto p1 = ln.find(':');
      auto p2 = ln.find(':', p1+1);
      if (p1==std::string::npos || p2==std::string::npos) continue;
      std::string pattern = ln.substr(0, p1);
      std::string origin = ln.substr(p1+1, p2-p1-1);
      std::string rest = ln.substr(p2+1);
      std::string key = pattern + ":" + origin;
      std::vector<time_t> timestamps;
      std::istringstream iss(rest);
      std::string tok;
      while (std::getline(iss, tok, ',')) {
        if (tok.empty()) continue;
        time_t t = (time_t)atoll(tok.c_str());
        timestamps.push_back(t);
      }
      if (!timestamps.empty()) _seen_timestamps[key] = timestamps;
    }
  }

  return (int)_rules.size();
}

bool HashRules::saveToFile(const std::string& path) const {
  std::ofstream f(path, std::ios::trunc);
  if (!f.is_open()) return false;
  for (const auto& r : _rules) {
    f << r.pattern << ' ' << (r.action == Action::Drop ? "drop" : "allow");
    if (r.pph) f << " pph=" << r.pph;
    if (r.p24h) f << " p24h=" << r.p24h;
    if (r.burst) f << " burst=" << r.burst;
    if (r.penalty_secs) f << " penalty_secs=" << r.penalty_secs;
    f << "\n";
  }

  // also persist state counters
  ensure_state_dir_exists();
  std::string state_path = "/var/lib/meshcore/hash-rules.state";
  std::ofstream sf(state_path, std::ios::trunc);
  if (sf.is_open()) {
    for (const auto& e : _seen_timestamps) {
      sf << e.first << ':';
      bool first = true;
      for (time_t t : e.second) {
        if (!first) sf << ',';
        sf << (long long)t;
        first = false;
      }
      sf << '\n';
    }
    // also save penalty expiries
    for (const auto& p : _penalty_expiry) {
      sf << p.first << ":pen:" << (long long)p.second << '\n';
    }
  }

  return true;
}

void HashRules::addRule(const Rule& r) {
  _rules.push_back(r);
}

bool HashRules::removeRule(const std::string& pattern) {
  auto p = toLower(pattern);
  auto it = std::remove_if(_rules.begin(), _rules.end(), [&](const Rule& rr){
    return rr.pattern == p;
  });
  if (it == _rules.end()) return false;
  _rules.erase(it, _rules.end());
  return true;
}

void HashRules::clearRules() {
  _rules.clear();
}

std::vector<HashRules::Rule> HashRules::getRules() const {
  return _rules;
}

HashRules::CheckResult HashRules::isAllowed(const uint8_t* full_hash, size_t full_hash_len, const std::string& origin) {
  CheckResult res; res.allowed = true; res.matched = false; res.rate_limited = false;
  std::string hex = bytesToHex(full_hash, full_hash_len);
  std::string hex_l = toLower(hex);
  time_t now = std::time(nullptr);

  for (const auto& r : _rules) {
    if (wildcardMatch(r.pattern, hex_l)) {
      res.matched = true;
      res.matched_pattern = r.pattern;

      auto pen_it = _penalty_expiry.find(r.pattern);
      if (pen_it != _penalty_expiry.end() && pen_it->second > now) {
        res.allowed = false;
        res.rate_limited = true;
        return res;
      }

      std::string key = patternOriginKey(r.pattern, origin);
      bool rate_limited = false;
      if (!checkRateLimit(r, key, now, origin, rate_limited)) {
        if (r.penalty_secs) _penalty_expiry[r.pattern] = now + r.penalty_secs;
        res.allowed = false;
        res.rate_limited = true;
        return res;
      }
      res.allowed = (r.action == Action::Allow);
      return res;
    }
  }
  res.allowed = true;
  return res;
}

HashRules::CheckResult HashRules::isAllowedHex(const std::string& full_hash_hex, const std::string& origin) {
  CheckResult res; res.allowed = true; res.matched = false; res.rate_limited = false;
  std::string hex_l = toLower(full_hash_hex);
  time_t now = std::time(nullptr);

  for (const auto& r : _rules) {
    if (wildcardMatch(r.pattern, hex_l)) {
      res.matched = true;
      res.matched_pattern = r.pattern;
      auto pen_it = _penalty_expiry.find(r.pattern);
      if (pen_it != _penalty_expiry.end() && pen_it->second > now) {
        res.allowed = false;
        res.rate_limited = true;
        return res;
      }
      std::string key = patternOriginKey(r.pattern, origin);
      bool rate_limited = false;
      if (!checkRateLimit(r, key, now, origin, rate_limited)) {
        if (r.penalty_secs) _penalty_expiry[r.pattern] = now + r.penalty_secs;
        res.allowed = false;
        res.rate_limited = true;
        return res;
      }
      res.allowed = (r.action == Action::Allow);
      return res;
    }
  }
  res.allowed = true;
  return res;
}

std::string HashRules::bytesToHex(const uint8_t* bytes, size_t len) {
  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (size_t i = 0; i < len; ++i) ss << std::setw(2) << (int)bytes[i];
  return ss.str();
}

std::string HashRules::toLower(const std::string& s) {
  std::string r; r.reserve(s.size());
  for (char c : s) r.push_back((char)std::tolower((unsigned char)c));
  return r;
}

bool HashRules::wildcardMatch(const std::string& pattern, const std::string& text) {
  size_t p = 0, t = 0;
  size_t star = std::string::npos, ss = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == text[t] || pattern[p] == '*')) {
      if (pattern[p] == '*') {
        star = p++;
        ss = t;
      } else { p++; t++; }
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++ss;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') p++;
  return p == pattern.size();
}

bool HashRules::checkRateLimit(const Rule& r, const std::string& pattern_key, time_t now, const std::string& origin, bool& out_rate_limited) const {
  out_rate_limited = false;
  if (r.pph == 0 && r.p24h == 0 && r.burst == 0) return true;

  auto& vec = const_cast<std::map<std::string, std::vector<time_t>>&>(_seen_timestamps)[pattern_key];

  time_t cutoff24 = now - 24*3600;
  vec.erase(std::remove_if(vec.begin(), vec.end(), [&](time_t ts){ return ts < cutoff24; }), vec.end());

  time_t cutoff1 = now - 3600;
  uint32_t cntHour = 0;
  for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
    if (*it >= cutoff1) ++cntHour;
    else break;
  }
  uint32_t cnt24 = (uint32_t)vec.size();

  if (r.pph && cntHour >= r.pph) {
    if (r.burst && cntHour < (r.pph + r.burst)) {
      vec.push_back(now);
      return true;
    } else {
      out_rate_limited = true;
      return false;
    }
  }
  if (r.p24h && cnt24 >= r.p24h) {
    if (r.burst && cnt24 < (r.p24h + r.burst)) {
      vec.push_back(now);
      return true;
    } else {
      out_rate_limited = true;
      return false;
    }
  }
  vec.push_back(now);
  return true;
}

std::string HashRules::patternOriginKey(const std::string& pattern, const std::string& origin) {
  return pattern + ":" + origin;
}
