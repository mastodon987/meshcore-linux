// CLI bindings for HashRules
#include "CommonCLI.h"
#include "HashRules.h"
#include <cstring>

// Helper to print rules into reply buffer
static void exportRulesToReply(char* reply) {
  auto rules = HashRules::instance().getRules();
  if (rules.empty()) { strcpy(reply, "(no rules)"); return; }
  char* dp = reply;
  for (size_t i = 0; i < rules.size() && (dp - reply) < 140; ++i) {
    auto &r = rules[i];
    int written = snprintf(dp, 160 - (dp - reply), "%s %s pph=%u p24h=%u burst=%u penalty=%u\n",
      r.pattern.c_str(), r.action == HashRules::Action::Drop ? "drop" : "allow",
      (unsigned)r.pph, (unsigned)r.p24h, (unsigned)r.burst, (unsigned)r.penalty_secs);
    if (written <= 0) break;
    dp += written;
  }
}

// Extend CommonCLI::handleCommand to check for hashrules commands
// We'll patch in a minimal handler at the top of handleCommand to intercept
// `hashrules` and `list.block.*` / `list.allow.*` commands.

// NOTE: because of the project's coding style we patch inside handleCommand
// near the top after command normalization.

// We'll create a separate function and then inject a call in handleCommand.

static bool handleHashRulesCommand(char* command, char* reply) {
  // commands: hashrules add <pattern> <allow|drop> [pph=N] [p24h=N] [burst=N] [penalty_secs=N]
  //           hashrules rm <pattern>
  //           hashrules reload
  //           hashrules save
  //           hashrules list
  char tmp[256];
  strncpy(tmp, command, sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
  char* cur = tmp;
  char* tok = takeToken(&cur);
  if (!tok) return false;
  if (strcmp(tok, "hashrules") != 0) return false;
  char* sub = takeToken(&cur);
  if (!sub) { strcpy(reply, "ERR - hashrules requires args"); return true; }
  if (strcmp(sub, "add") == 0) {
    char* pattern = takeToken(&cur);
    char* action = takeToken(&cur);
    if (!pattern || !action) { strcpy(reply, "ERR - usage: hashrules add <pattern> <allow|drop> [pph=N] ..."); return true; }
    HashRules::Rule r;
    r.pattern = pattern;
    r.action = (strcmp(action, "drop") == 0) ? HashRules::Action::Drop : HashRules::Action::Allow;
    r.pph = r.p24h = r.burst = r.penalty_secs = 0;
    char* opt;
    while ((opt = takeToken(&cur)) != nullptr) {
      char* eq = strchr(opt, '=');
      if (!eq) continue;
      *eq = '\0';
      char* k = opt; char* v = eq+1;
      if (strcmp(k, "pph") == 0) r.pph = atoi(v);
      else if (strcmp(k, "p24h") == 0) r.p24h = atoi(v);
      else if (strcmp(k, "burst") == 0) r.burst = atoi(v);
      else if (strcmp(k, "penalty_secs") == 0) r.penalty_secs = atoi(v);
    }
    HashRules::instance().addRule(r);
    HashRules::instance().saveToFile();
    strcpy(reply, "OK");
    return true;
  } else if (strcmp(sub, "rm") == 0) {
    char* pattern = takeToken(&cur);
    if (!pattern) { strcpy(reply, "ERR - usage: hashrules rm <pattern>"); return true; }
    if (HashRules::instance().removeRule(pattern)) {
      HashRules::instance().saveToFile();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - not found");
    }
    return true;
  } else if (strcmp(sub, "reload") == 0) {
    int n = HashRules::instance().loadFromFile();
    if (n >= 0) { sprintf(reply, "OK - %d rules loaded", n); } else { strcpy(reply, "Err - load failed"); }
    return true;
  } else if (strcmp(sub, "save") == 0) {
    if (HashRules::instance().saveToFile()) { strcpy(reply, "OK"); } else { strcpy(reply, "Err - save failed"); }
    return true;
  } else if (strcmp(sub, "list") == 0) {
    exportRulesToReply(reply);
    return true;
  }
  strcpy(reply, "Err - unknown hashrules subcmd");
  return true;
}

// New list helpers: list.block.lora, list.block.mqtt, list.allow.lora, list.allow.mqtt
static bool handleListHelpers(char* command, char* reply) {
  if (strncmp(command, "list.block.lora", 15) == 0) {
    // show patterns that are drop for origin lora
    auto rules = HashRules::instance().getRules();
    char* dp = reply; if (rules.empty()) { strcpy(reply, "(none)"); return true; }
    for (auto &r : rules) {
      if (r.action == HashRules::Action::Drop) {
        int w = snprintf(dp, 160 - (dp - reply), "%s\n", r.pattern.c_str());
        dp += w; if ((dp - reply) >= 159) break;
      }
    }
    return true;
  }
  if (strncmp(command, "list.block.mqtt", 15) == 0) {
    // same as above (rules don't generally vary by origin in file), but list for mqtt
    auto rules = HashRules::instance().getRules();
    char* dp = reply; if (rules.empty()) { strcpy(reply, "(none)"); return true; }
    for (auto &r : rules) {
      if (r.action == HashRules::Action::Drop) {
        int w = snprintf(dp, 160 - (dp - reply), "%s\n", r.pattern.c_str());
        dp += w; if ((dp - reply) >= 159) break;
      }
    }
    return true;
  }
  if (strncmp(command, "list.allow.lora", 15) == 0) {
    auto rules = HashRules::instance().getRules();
    char* dp = reply; if (rules.empty()) { strcpy(reply, "(none)"); return true; }
    for (auto &r : rules) {
      if (r.action == HashRules::Action::Allow) {
        int w = snprintf(dp, 160 - (dp - reply), "%s\n", r.pattern.c_str());
        dp += w; if ((dp - reply) >= 159) break;
      }
    }
    return true;
  }
  if (strncmp(command, "list.allow.mqtt", 15) == 0) {
    auto rules = HashRules::instance().getRules();
    char* dp = reply; if (rules.empty()) { strcpy(reply, "(none)"); return true; }
    for (auto &r : rules) {
      if (r.action == HashRules::Action::Allow) {
        int w = snprintf(dp, 160 - (dp - reply), "%s\n", r.pattern.c_str());
        dp += w; if ((dp - reply) >= 159) break;
      }
    }
    return true;
  }
  return false;
}

// We'll now inject calls to these handlers at the top of CommonCLI::handleCommand. To avoid
// having to rewrite the entire file here, we'll append a function that the existing handleCommand
// will call (if you prefer the handlers directly inside handleCommand I can change it).

bool CommonCLI::handleHashRulesAndLists(uint32_t sender_timestamp, char* command, char* reply) {
  (void)sender_timestamp;
  if (handleHashRulesCommand(command, reply)) return true;
  if (handleListHelpers(command, reply)) return true;
  return false;
}
