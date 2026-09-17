#include "config.hpp"

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#include "nlohmann/json.hpp"
#include "util.hpp"

namespace podlogs {

using nlohmann::json;

namespace {

const std::set<std::string> kKnownFields = {"host",      "container_name", "container_id", "image", "image_name",
                                            "image_tag", "image_id",       "stream",       "pod"};

struct Reader {
  const json& j;
  std::string where;

  const json* at(const char* key) const {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
  }

  void str(const char* key, std::string& out) const {
    if (const json* v = at(key)) {
      if (!v->is_string()) throw std::runtime_error(where + "." + key + " must be a string");
      out = v->get<std::string>();
    }
  }
  void boolean(const char* key, bool& out) const {
    if (const json* v = at(key)) {
      if (!v->is_boolean()) throw std::runtime_error(where + "." + key + " must be true/false");
      out = v->get<bool>();
    }
  }
  template <class T>
  void integer(const char* key, T& out) const {
    if (const json* v = at(key)) {
      if (!v->is_number_integer()) throw std::runtime_error(where + "." + key + " must be an integer");
      out = v->get<T>();
    }
  }
  // Accepts an integer (milliseconds) or a duration string like "5s".
  void duration_ms(const char* key, int& out) const {
    if (const json* v = at(key)) {
      if (v->is_number_integer()) {
        out = v->get<int>();
      } else if (v->is_string()) {
        auto ms = util::parse_duration_ms(v->get<std::string>());
        if (!ms) throw std::runtime_error(where + "." + key + ": invalid duration '" + v->get<std::string>() + "'");
        out = static_cast<int>(*ms);
      } else {
        throw std::runtime_error(where + "." + key + " must be a duration string or integer milliseconds");
      }
    }
  }
  void size(const char* key, size_t& out) const {
    if (const json* v = at(key)) {
      if (!v->is_number_integer() || v->get<int64_t>() < 0) throw std::runtime_error(where + "." + key + " must be a non-negative integer");
      out = v->get<size_t>();
    }
  }
  void strings(const char* key, std::vector<std::string>& out) const {
    if (const json* v = at(key)) {
      if (!v->is_array()) throw std::runtime_error(where + "." + key + " must be an array of strings");
      out.clear();
      for (const auto& item : *v) {
        if (!item.is_string()) throw std::runtime_error(where + "." + key + " must be an array of strings");
        out.push_back(item.get<std::string>());
      }
    }
  }
  void string_map(const char* key, std::map<std::string, std::string>& out) const {
    if (const json* v = at(key)) {
      if (!v->is_object()) throw std::runtime_error(where + "." + key + " must be an object of strings");
      out.clear();
      for (const auto& [k, item] : v->items()) {
        if (!item.is_string()) throw std::runtime_error(where + "." + key + "." + k + " must be a string");
        out[k] = item.get<std::string>();
      }
    }
  }
  Reader sub(const char* key) const {
    const json* v = at(key);
    static const json empty = json::object();
    if (v && !v->is_object()) throw std::runtime_error(where + "." + key + " must be an object");
    return Reader{v ? *v : empty, where + "." + key};
  }
};

}  // namespace

Config load_config(const std::string& path) {
  Config cfg;
  if (!path.empty()) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    json doc;
    try {
      doc = json::parse(ss.str(), nullptr, true, /*ignore_comments=*/true);
    } catch (const json::parse_error& e) {
      throw std::runtime_error("config " + path + ": " + e.what());
    }
    if (!doc.is_object()) throw std::runtime_error("config " + path + ": top level must be an object");

    Reader root{doc, "config"};
    root.str("log_level", cfg.log_level);
    root.str("hostname", cfg.hostname);
    root.size("queue_capacity", cfg.queue_capacity);
    root.str("metrics_listen", cfg.metrics_listen);

    Reader p = root.sub("podman");
    p.str("socket", cfg.podman.socket);
    p.str("api_prefix", cfg.podman.api_prefix);
    p.duration_ms("timeout", cfg.podman.timeout_ms);
    p.duration_ms("resync_interval", cfg.podman.resync_interval_ms);
    p.strings("include", cfg.podman.include);
    p.strings("exclude", cfg.podman.exclude);
    p.boolean("skip_infra", cfg.podman.skip_infra);
    p.str("initial_lookback", cfg.podman.initial_lookback);

    Reader l = root.sub("loki");
    l.str("url", cfg.loki.url);
    l.duration_ms("timeout", cfg.loki.timeout_ms);
    l.duration_ms("batch_wait", cfg.loki.batch_wait_ms);
    l.size("batch_bytes", cfg.loki.batch_bytes);
    l.size("batch_entries", cfg.loki.batch_entries);
    l.integer("max_retries", cfg.loki.max_retries);
    l.duration_ms("backoff_max", cfg.loki.backoff_max_ms);
    l.str("tenant", cfg.loki.tenant);
    l.str("basic_auth_user", cfg.loki.basic_auth_user);
    l.str("basic_auth_pass", cfg.loki.basic_auth_pass);

    Reader c = root.sub("checkpoint");
    c.str("path", cfg.checkpoint.path);
    c.duration_ms("interval", cfg.checkpoint.interval_ms);

    Reader pr = root.sub("parse");
    pr.strings("level_keys", cfg.parse.level_keys);
    pr.strings("logger_keys", cfg.parse.logger_keys);
    pr.str("numeric_levels", cfg.parse.numeric_levels);
    pr.boolean("detect_text_levels", cfg.parse.detect_text_levels);
    pr.size("text_scan_bytes", cfg.parse.text_scan_bytes);
    Reader ml = pr.sub("multiline");
    ml.boolean("enabled", cfg.parse.multiline.enabled);
    ml.duration_ms("flush_timeout", cfg.parse.multiline.flush_timeout_ms);
    ml.integer("max_lines", cfg.parse.multiline.max_lines);
    ml.size("max_bytes", cfg.parse.multiline.max_bytes);
    std::vector<std::string> extra;
    ml.strings("extra_patterns", extra);
    for (auto& e : extra) cfg.parse.multiline.continuation_patterns.push_back(e);
    ml.strings("patterns", cfg.parse.multiline.continuation_patterns);

    Reader lb = root.sub("labels");
    lb.string_map("static", cfg.labels.static_labels);
    lb.strings("from_container", cfg.labels.from_container);
    lb.strings("as_metadata", cfg.labels.as_metadata);
    lb.string_map("container_labels", cfg.labels.container_labels);
    lb.boolean("level_as_label", cfg.labels.level_as_label);
  }
  apply_env_overrides(cfg);
  validate_config(cfg);
  return cfg;
}

void apply_env_overrides(Config& cfg) {
  cfg.podman.socket = util::getenv_or("PODLOGS_PODMAN_SOCKET", cfg.podman.socket);
  cfg.loki.url = util::getenv_or("PODLOGS_LOKI_URL", cfg.loki.url);
  cfg.loki.tenant = util::getenv_or("PODLOGS_LOKI_TENANT", cfg.loki.tenant);
  const std::string auth = util::getenv_or("PODLOGS_LOKI_BASIC_AUTH", "");
  if (!auth.empty()) {
    const size_t colon = auth.find(':');
    cfg.loki.basic_auth_user = auth.substr(0, colon);
    cfg.loki.basic_auth_pass = colon == std::string::npos ? "" : auth.substr(colon + 1);
  }
  cfg.checkpoint.path = util::getenv_or("PODLOGS_CHECKPOINT_PATH", cfg.checkpoint.path);
  cfg.log_level = util::getenv_or("PODLOGS_LOG_LEVEL", cfg.log_level);
  cfg.metrics_listen = util::getenv_or("PODLOGS_METRICS_LISTEN", cfg.metrics_listen);
  cfg.hostname = util::getenv_or("PODLOGS_HOSTNAME", cfg.hostname);
  cfg.podman.initial_lookback = util::getenv_or("PODLOGS_INITIAL_LOOKBACK", cfg.podman.initial_lookback);
}

void validate_config(const Config& cfg) {
  auto check_fields = [](const std::vector<std::string>& fields, const char* what) {
    for (const auto& f : fields) {
      if (!kKnownFields.count(f)) {
        std::string valid;
        for (const auto& k : kKnownFields) valid += (valid.empty() ? "" : ", ") + k;
        throw std::runtime_error(std::string("labels.") + what + ": unknown field '" + f + "' (valid: " + valid + ")");
      }
    }
  };
  check_fields(cfg.labels.from_container, "from_container");
  check_fields(cfg.labels.as_metadata, "as_metadata");
  for (const auto& [k, v] : cfg.labels.static_labels) {
    if (util::sanitize_label_name(k) != k) throw std::runtime_error("labels.static: '" + k + "' is not a valid label name");
    (void)v;
  }
  if (cfg.podman.initial_lookback != "all" && !util::parse_duration_ms(cfg.podman.initial_lookback)) {
    throw std::runtime_error("podman.initial_lookback must be \"all\" or a duration such as \"1h\"");
  }
  if (!http::parse_endpoint(cfg.loki.url) || http::parse_endpoint(cfg.loki.url)->is_unix()) {
    throw std::runtime_error("loki.url must be http://host[:port][/prefix] (TLS is not supported; put a reverse proxy in front if needed)");
  }
  if (cfg.parse.numeric_levels != "pino" && cfg.parse.numeric_levels != "python") {
    throw std::runtime_error("parse.numeric_levels must be \"pino\" or \"python\"");
  }
  auto check_regexes = [](const std::vector<std::string>& res, const char* what) {
    for (const auto& r : res) {
      try {
        std::regex re(r, std::regex::ECMAScript);
      } catch (const std::regex_error& e) {
        throw std::runtime_error(std::string(what) + ": invalid regex '" + r + "': " + e.what());
      }
    }
  };
  check_regexes(cfg.podman.include, "podman.include");
  check_regexes(cfg.podman.exclude, "podman.exclude");
  check_regexes(cfg.parse.multiline.continuation_patterns, "parse.multiline.patterns");
  if (cfg.queue_capacity < 100) throw std::runtime_error("queue_capacity must be at least 100");
  if (cfg.loki.batch_entries == 0 || cfg.loki.batch_bytes == 0) throw std::runtime_error("loki.batch_entries/batch_bytes must be > 0");
}

std::string dump_config(const Config& cfg) {
  json j = {
      {"log_level", cfg.log_level},
      {"hostname", cfg.hostname},
      {"queue_capacity", cfg.queue_capacity},
      {"metrics_listen", cfg.metrics_listen},
      {"podman",
       {{"socket", cfg.podman.socket},
        {"api_prefix", cfg.podman.api_prefix},
        {"timeout", cfg.podman.timeout_ms},
        {"resync_interval", cfg.podman.resync_interval_ms},
        {"include", cfg.podman.include},
        {"exclude", cfg.podman.exclude},
        {"skip_infra", cfg.podman.skip_infra},
        {"initial_lookback", cfg.podman.initial_lookback}}},
      {"loki",
       {{"url", cfg.loki.url},
        {"timeout", cfg.loki.timeout_ms},
        {"batch_wait", cfg.loki.batch_wait_ms},
        {"batch_bytes", cfg.loki.batch_bytes},
        {"batch_entries", cfg.loki.batch_entries},
        {"max_retries", cfg.loki.max_retries},
        {"backoff_max", cfg.loki.backoff_max_ms},
        {"tenant", cfg.loki.tenant},
        {"basic_auth_user", cfg.loki.basic_auth_user},
        {"basic_auth_pass", cfg.loki.basic_auth_pass.empty() ? "" : "********"}}},
      {"checkpoint", {{"path", cfg.checkpoint.path}, {"interval", cfg.checkpoint.interval_ms}}},
      {"parse",
       {{"level_keys", cfg.parse.level_keys},
        {"logger_keys", cfg.parse.logger_keys},
        {"numeric_levels", cfg.parse.numeric_levels},
        {"detect_text_levels", cfg.parse.detect_text_levels},
        {"text_scan_bytes", cfg.parse.text_scan_bytes},
        {"multiline",
         {{"enabled", cfg.parse.multiline.enabled},
          {"flush_timeout", cfg.parse.multiline.flush_timeout_ms},
          {"max_lines", cfg.parse.multiline.max_lines},
          {"max_bytes", cfg.parse.multiline.max_bytes},
          {"patterns", cfg.parse.multiline.continuation_patterns}}}}},
      {"labels",
       {{"static", cfg.labels.static_labels},
        {"from_container", cfg.labels.from_container},
        {"as_metadata", cfg.labels.as_metadata},
        {"container_labels", cfg.labels.container_labels},
        {"level_as_label", cfg.labels.level_as_label}}},
  };
  return j.dump(2);
}

}  // namespace podlogs
