#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <ctime>

/*
  HashRules - simple wildcard-enabled hash-based policy + rate-limiter.

  Rule syntax (hash-rules.txt):
    # comments lines start with '#'
    5a* allow pph=100 p24h=1000 burst=10 penalty_secs=604800
    *5a*cd drop
    abcd1234 allow

  Pattern: hex string with optional '*' wildcards. Case-insensitive.
  Action: allow | drop
  Options:
    pph        = packets per hour (sliding window)
    p24h       = packets per 24-hours
    burst      = allowed burst tokens (replenished over time)
    penalty_secs = seconds to disable the rule on burst exhaustion

  Note: rules are checked in file order: first matching rule applies.
*/

class HashRules {
public:
  enum class Action { Allow, Drop };

  struct Rule {
    std::string pattern;    // pattern string (hex + *)
    Action action;
    uint32_t pph;           // packets per hour (0 = unlimited)
    uint32_t p24h;          // packets per 24h (0 = unlimited)
    uint32_t burst;         // burst tokens
    uint32_t penalty_secs;  // penalty period in seconds when burst exhausted
  };

  struct CheckResult {
    bool allowed;
    bool matched;
    bool rate_limited;
    std::string matched_pattern;
  };

  static HashRules& instance();

  // Load rules from file (returns number of rules loaded, -1 on error)
  int loadFromFile(const std::string& path = "/etc/hash-rules.txt");

  // Save current rules to file (overwrites)
  bool saveToFile(const std::string& path = "/etc/hash-rules.txt") const;

  // Programmatic rule changes:
  void addRule(const Rule& r);
  bool removeRule(const std::string& pattern);
  void clearRules();

  // List rules
  std::vector<Rule> getRules() const;

  // Check whether a given full hash (raw bytes, 32 bytes) is allowed for given origin ("mqtt" or "lora").
  CheckResult isAllowed(const uint8_t* full_hash, size_t full_hash_len /*32*/, const std::string& origin = "mqtt");

  // Convenience: hex string input
  CheckResult isAllowedHex(const std::string& full_hash_hex, const std::string& origin = "mqtt");

private:
  HashRules();
  std::vector<Rule> _rules;

  // Rate limiter storage: key -> vector of timestamps (seconds)
  mutable std::map<std::string, std::vector<time_t>> _seen_timestamps;

  // Per-pattern penalty expiry (pattern -> unix_ts when penalty expires)
  mutable std::map<std::string, time_t> _penalty_expiry;

  // helpers
  static std::string bytesToHex(const uint8_t* bytes, size_t len);
  static std::string toLower(const std::string& s);
  static bool wildcardMatch(const std::string& pattern, const std::string& text);
  bool checkRateLimit(const Rule& r, const std::string& pattern_key, time_t now, const std::string& origin, bool& out_rate_limited) const;

  // make the key we store counters under (pattern + origin)
  static std::string patternOriginKey(const std::string& pattern, const std::string& origin);
};
