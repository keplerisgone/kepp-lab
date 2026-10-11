#include <json-c/json.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;

struct CommandResult {
    int exit_code;
    std::string output;
    std::string errors;
    bool timed_out = false;
    bool signaled = false;
    int signal = 0;
};

struct Options {
    std::string repository;
    std::optional<std::filesystem::path> path;
    std::optional<std::filesystem::path> scan_root;
    std::set<std::string> trusted_users;
    std::set<std::string> trusted_owners;
    int codex_timeout_seconds = 3600;
    bool resume = false;
};

struct ApprovedIssue {
    std::string number;
    std::string title;
    std::string body;
    std::string author;
    std::string editor;
    std::string labeler;
};

struct Verification {
    std::filesystem::path directory;
    std::vector<std::string> command;
    std::string display;
};

struct JobState {
    std::string stage;
    std::string repository;
    std::string issue;
    std::string title;
    std::string body;
    std::string branch;
    std::string worktree;
    std::string source_root;
    std::string base_commit;
    std::string base_branch;
    std::string fingerprint;
    std::string commit;
    std::vector<std::string> paths;
};

class ExecutionLock {
  public:
    explicit ExecutionLock(const std::string& repository) {
        auto name = repository;
        for (auto& character : name) if (character == '/') character = '-';
        path_ = std::filesystem::temp_directory_path() /
                ("kepp-worker-" + std::to_string(getuid()) + "-" + name + ".lock");
        descriptor_ = open(path_.c_str(), O_CREAT | O_RDWR, 0600);
        if (descriptor_ < 0) throw std::runtime_error("Cannot open execution lock: " + std::string(std::strerror(errno)));
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const auto message = errno == EWOULDBLOCK
                ? "Another worker is already running for " + repository
                : "Cannot acquire execution lock: " + std::string(std::strerror(errno));
            close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error(message);
        }
    }
    ~ExecutionLock() {
        if (descriptor_ >= 0) {
            flock(descriptor_, LOCK_UN);
            close(descriptor_);
        }
    }
    ExecutionLock(const ExecutionLock&) = delete;
    ExecutionLock& operator=(const ExecutionLock&) = delete;
  private:
    int descriptor_ = -1;
    std::filesystem::path path_;
};

class ScanExecutionLock {
  public:
    explicit ScanExecutionLock(const std::filesystem::path& scan_root) {
        const auto identity = std::to_string(std::hash<std::string>{}(scan_root.string()));
        path_ = std::filesystem::temp_directory_path() /
                ("kepp-worker-" + std::to_string(getuid()) + "-scan-" + identity + ".lock");
        descriptor_ = open(path_.c_str(), O_CREAT | O_RDWR, 0600);
        if (descriptor_ < 0)
            throw std::runtime_error("Cannot open scan lock: " + std::string(std::strerror(errno)));
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK) {
                close(descriptor_);
                descriptor_ = -1;
                return;
            }
            const auto message = std::string("Cannot acquire scan lock: ") + std::strerror(errno);
            close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error(message);
        }
        acquired_ = true;
    }
    ~ScanExecutionLock() {
        if (descriptor_ >= 0) {
            flock(descriptor_, LOCK_UN);
            close(descriptor_);
        }
    }
    bool acquired() const { return acquired_; }
    ScanExecutionLock(const ScanExecutionLock&) = delete;
    ScanExecutionLock& operator=(const ScanExecutionLock&) = delete;
  private:
    int descriptor_ = -1;
    bool acquired_ = false;
    std::filesystem::path path_;
};

std::string read_file(FILE* file) {
    if (std::fseek(file, 0, SEEK_SET) != 0) throw std::runtime_error(std::strerror(errno));
    std::string result;
    char buffer[4096];
    size_t size;
    while ((size = std::fread(buffer, 1, sizeof buffer, file)) != 0) result.append(buffer, size);
    if (std::ferror(file)) throw std::runtime_error(std::strerror(errno));
    return result;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\n\r\f\v") - first + 1);
}

std::string lowercase(std::string value) {
    for (auto& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

CommandResult run_command(const std::vector<std::string>& arguments,
                          const std::filesystem::path& directory,
                          const std::optional<std::string>& input = std::nullopt,
                          const std::optional<int>& timeout_seconds = std::nullopt) {
    if (arguments.empty()) throw std::runtime_error("Empty command");
    File output(std::tmpfile(), &std::fclose), errors(std::tmpfile(), &std::fclose);
    if (!output || !errors) throw std::runtime_error(std::strerror(errno));
    File input_file(nullptr, &std::fclose);
    if (input) {
        input_file.reset(std::tmpfile());
        if (!input_file) throw std::runtime_error(std::strerror(errno));
        if (std::fwrite(input->data(), 1, input->size(), input_file.get()) != input->size() ||
            std::fseek(input_file.get(), 0, SEEK_SET) != 0)
            throw std::runtime_error(std::strerror(errno));
    }
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error(std::strerror(errno));
    if (child == 0) {
        setpgid(0, 0);
        if (input_file && dup2(fileno(input_file.get()), STDIN_FILENO) < 0) {
            dprintf(STDERR_FILENO, "Worker failed: %s\n", std::strerror(errno));
            _exit(1);
        }
        if (dup2(fileno(output.get()), STDOUT_FILENO) < 0 ||
            dup2(fileno(errors.get()), STDERR_FILENO) < 0 || chdir(directory.c_str()) != 0) {
            dprintf(STDERR_FILENO, "Worker failed: %s\n", std::strerror(errno));
            _exit(1);
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        dprintf(STDERR_FILENO, "Worker failed: [Errno %d] %s: '%s'\n",
                errno, std::strerror(errno), arguments[0].c_str());
        _exit(127);
    }
    setpgid(child, child);

    int status = 0;
    bool timed_out = false;
    const auto deadline = timeout_seconds
        ? std::chrono::steady_clock::now() + std::chrono::seconds(*timeout_seconds)
        : std::chrono::steady_clock::time_point::max();
    while (true) {
        const auto waited = waitpid(child, &status, timeout_seconds ? WNOHANG : 0);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) throw std::runtime_error(std::strerror(errno));
        if (waited < 0) continue;
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            kill(-child, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (waitpid(child, &status, WNOHANG) == 0) {
                kill(-child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const bool signaled = WIFSIGNALED(status);
    const int signal_number = signaled ? WTERMSIG(status) : 0;
    const int exit_code = timed_out ? 124 : (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + signal_number);
    return {exit_code, read_file(output.get()), read_file(errors.get()), timed_out, signaled, signal_number};
}

void print_streams(const CommandResult& result) {
    if (!result.output.empty()) std::cout << result.output << (result.output.back() == '\n' ? "" : "\n");
    if (!result.errors.empty()) std::cerr << result.errors << (result.errors.back() == '\n' ? "" : "\n");
}

int command_failed(const std::string& stage, const CommandResult& result) {
    print_streams(result);
    if (result.timed_out) std::cerr << stage << " timed out\n";
    else if (result.signaled) std::cerr << stage << " terminated by signal " << result.signal << '\n';
    else std::cerr << stage << " exited with code " << result.exit_code << '\n';
    return 1;
}

void log_stage(const std::string& stage) { std::cout << "[worker] " << stage << '\n'; }

std::string repository_from_remote(std::string remote) {
    remote = trim(std::move(remote));
    const std::vector<std::string> prefixes = {
        "git@github.com:", "ssh://git@github.com/", "https://github.com/", "http://github.com/"
    };
    bool matched = false;
    for (const auto& prefix : prefixes) {
        if (remote.rfind(prefix, 0) == 0) {
            remote.erase(0, prefix.size());
            matched = true;
            break;
        }
    }
    if (!matched) throw std::runtime_error("Unsupported origin URL: " + remote);
    if (remote.size() > 4 && remote.compare(remote.size() - 4, 4, ".git") == 0) remote.resize(remote.size() - 4);
    while (!remote.empty() && remote.back() == '/') remote.pop_back();
    const auto separator = remote.find('/');
    if (separator == std::string::npos || separator == 0 || separator + 1 == remote.size() ||
        remote.find('/', separator + 1) != std::string::npos)
        throw std::runtime_error("Invalid GitHub origin repository: " + remote);
    return remote;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--repo" && index + 1 < argc) options.repository = argv[++index];
        else if (argument == "--path" && index + 1 < argc) options.path = argv[++index];
        else if (argument == "--scan-root" && index + 1 < argc) options.scan_root = argv[++index];
        else if (argument == "--trusted-user" && index + 1 < argc) {
            const std::string login = argv[++index];
            if (login.empty() || login.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") !=
                                     std::string::npos)
                throw std::runtime_error("Invalid --trusted-user login");
            options.trusted_users.insert(lowercase(login));
        } else if (argument == "--trusted-owner" && index + 1 < argc) {
            const std::string owner = argv[++index];
            if (owner.empty() || owner.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") !=
                                     std::string::npos)
                throw std::runtime_error("Invalid --trusted-owner login");
            options.trusted_owners.insert(lowercase(owner));
        }
        else if (argument == "--timeout" && index + 1 < argc) {
            try { options.codex_timeout_seconds = std::stoi(argv[++index]); }
            catch (...) { throw std::runtime_error("Invalid --timeout value"); }
            if (options.codex_timeout_seconds <= 0) throw std::runtime_error("--timeout must be positive");
        } else if (argument == "--resume") options.resume = true;
        else throw std::runtime_error("Usage: worker [--repo owner/name] [--path PATH | --scan-root PATH] "
                                      "[--trusted-owner LOGIN] [--trusted-user LOGIN] "
                                      "[--timeout SECONDS] [--resume]");
    }
    if (options.path && options.scan_root) throw std::runtime_error("--path and --scan-root are mutually exclusive");
    if (options.scan_root && options.trusted_owners.empty())
        throw std::runtime_error("--scan-root requires at least one --trusted-owner");
    if (options.scan_root && !options.repository.empty())
        throw std::runtime_error("--repo cannot be combined with --scan-root");
    return options;
}

std::filesystem::path repository_path(const Options& options) {
    const auto candidate = std::filesystem::canonical(options.path.value_or(std::filesystem::current_path()));
    const auto result = run_command({"git", "rev-parse", "--show-toplevel"}, candidate);
    if (result.exit_code != 0) {
        print_streams(result);
        throw std::runtime_error("Path is not inside a non-bare Git worktree: " + candidate.string());
    }
    const auto root_text = trim(result.output);
    if (root_text.empty()) throw std::runtime_error("Git returned an empty repository root");
    return std::filesystem::canonical(root_text);
}

bool path_is_within(const std::filesystem::path& path, const std::filesystem::path& parent) {
    const auto relative = path.lexically_relative(parent);
    if (relative.empty()) return path == parent;
    return !relative.is_absolute() && *relative.begin() != "..";
}

std::vector<std::filesystem::path> discover_repositories(const Options& options) {
    const auto scan_root = std::filesystem::canonical(*options.scan_root);
    if (!std::filesystem::is_directory(scan_root))
        throw std::runtime_error("Scan root is not a directory: " + scan_root.string());

    std::vector<std::filesystem::path> repositories;
    std::set<std::string> common_directories;
    const auto inspect = [&](const std::filesystem::path& candidate) {
        try {
            const auto top_result = run_command({"git", "rev-parse", "--show-toplevel"}, candidate);
            const auto git_dir_result = run_command({"git", "rev-parse", "--absolute-git-dir"}, candidate);
            const auto common_result = run_command(
                {"git", "rev-parse", "--path-format=absolute", "--git-common-dir"}, candidate);
            if (top_result.exit_code != 0 || git_dir_result.exit_code != 0 || common_result.exit_code != 0) {
                std::cout << "[scan] " << candidate << ": skipped invalid or bare repository\n";
                return;
            }
            const auto top = std::filesystem::canonical(trim(top_result.output));
            const auto git_directory = std::filesystem::canonical(trim(git_dir_result.output));
            const auto common_directory = std::filesystem::canonical(trim(common_result.output));
            if (!path_is_within(top, scan_root)) {
                std::cout << "[scan] " << candidate << ": skipped root outside scan boundary\n";
                return;
            }
            if (git_directory != common_directory) {
                std::cout << "[scan] " << top << ": skipped linked worktree\n";
                return;
            }
            const auto remote_result = run_command({"git", "remote", "get-url", "origin"}, top);
            if (remote_result.exit_code != 0) {
                std::cout << "[scan] " << top << ": skipped missing origin\n";
                return;
            }
            const auto repository = repository_from_remote(remote_result.output);
            const auto separator = repository.find('/');
            const auto owner = lowercase(repository.substr(0, separator));
            if (!options.trusted_owners.count(owner)) {
                std::cout << "[scan] " << top << ": skipped untrusted owner @" << owner << '\n';
                return;
            }
            if (!common_directories.insert(common_directory.string()).second) {
                std::cout << "[scan] " << top << ": skipped duplicate Git common directory\n";
                return;
            }
            repositories.push_back(top);
            std::cout << "[scan] " << top << ": accepted " << repository << '\n';
        } catch (const std::exception& error) {
            std::cout << "[scan] " << candidate << ": skipped: " << error.what() << '\n';
        }
    };

    if (std::filesystem::exists(scan_root / ".git")) {
        inspect(scan_root);
        std::sort(repositories.begin(), repositories.end());
        return repositories;
    }
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        scan_root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }
        const auto entry = *iterator;
        const auto status = entry.symlink_status(error);
        if (!error && std::filesystem::is_symlink(status)) {
            if (entry.is_directory(error)) iterator.disable_recursion_pending();
        } else if (!error && entry.is_directory(error) && std::filesystem::exists(entry.path() / ".git")) {
            inspect(entry.path());
            iterator.disable_recursion_pending();
        }
        iterator.increment(error);
    }
    std::sort(repositories.begin(), repositories.end());
    return repositories;
}

std::string field(json_object* object, const char* name) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value)) throw std::runtime_error(std::string("Missing JSON field: ") + name);
    if (!value) return "";
    switch (json_object_get_type(value)) {
    case json_type_string: return {json_object_get_string(value), static_cast<size_t>(json_object_get_string_len(value))};
    case json_type_boolean: return json_object_get_boolean(value) ? "true" : "false";
    default: return json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
    }
}

std::unique_ptr<json_object, decltype(&json_object_put)> parse_json(const std::string& text) {
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new(), &json_tokener_free);
    if (!parser) throw std::runtime_error("Cannot allocate JSON parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT);
    std::unique_ptr<json_object, decltype(&json_object_put)> value(
        json_tokener_parse_ex(parser.get(), text.c_str(), static_cast<int>(text.size() + 1)), &json_object_put);
    if (json_tokener_get_error(parser.get()) != json_tokener_success)
        throw std::runtime_error(json_tokener_error_desc(json_tokener_get_error(parser.get())));
    const auto end = json_tokener_get_parse_end(parser.get());
    if (end < text.size() && !trim(text.substr(end)).empty()) throw std::runtime_error("Extra JSON data");
    return value;
}

json_object* object_member(json_object* object, const char* name) {
    json_object* value = nullptr;
    if (!object || json_object_get_type(object) != json_type_object ||
        !json_object_object_get_ex(object, name, &value) || !value)
        throw std::runtime_error(std::string("Missing GitHub response field: ") + name);
    return value;
}

std::string optional_login(json_object* object) {
    if (!object || json_object_get_type(object) == json_type_null) return {};
    return lowercase(field(object, "login"));
}

std::optional<ApprovedIssue> approved_issue(const std::string& repository,
                                             const std::string& number,
                                             const std::set<std::string>& trusted_users,
                                             const std::filesystem::path& directory,
                                             bool explain_rejection = true) {
    const auto separator = repository.find('/');
    const auto owner = repository.substr(0, separator);
    const auto name = repository.substr(separator + 1);
    static const std::string query =
        "query($owner:String!,$name:String!,$number:Int!){repository(owner:$owner,name:$name){"
        "issue(number:$number){number title body author{login} editor{login} labels(first:100){nodes{name}}"
        "timelineItems(last:100,itemTypes:[LABELED_EVENT]){nodes{... on LabeledEvent{actor{login} label{name}}}}}}}";
    const auto result = run_command({"gh", "api", "graphql", "-f", "query=" + query,
        "-F", "owner=" + owner, "-F", "name=" + name, "-F", "number=" + number}, directory);
    if (result.exit_code != 0) throw std::runtime_error("GitHub approval lookup failed for issue #" + number +
                                                       ": " + trim(result.errors));
    auto response = parse_json(result.output);
    auto* data = object_member(response.get(), "data");
    auto* repository_object = object_member(data, "repository");
    auto* issue = object_member(repository_object, "issue");

    ApprovedIssue approved;
    approved.number = field(issue, "number");
    approved.title = field(issue, "title");
    approved.body = field(issue, "body");
    json_object* author = nullptr;
    json_object* editor = nullptr;
    json_object_object_get_ex(issue, "author", &author);
    json_object_object_get_ex(issue, "editor", &editor);
    approved.author = optional_login(author);
    approved.editor = optional_login(editor);

    bool has_ready_label = false;
    auto* labels = object_member(object_member(issue, "labels"), "nodes");
    for (size_t index = 0; index < json_object_array_length(labels); ++index) {
        if (field(json_object_array_get_idx(labels, index), "name") == "agent:ready") has_ready_label = true;
    }
    auto* events = object_member(object_member(issue, "timelineItems"), "nodes");
    for (size_t index = 0; index < json_object_array_length(events); ++index) {
        auto* event = json_object_array_get_idx(events, index);
        if (!event || json_object_get_type(event) == json_type_null) continue;
        auto* label = object_member(event, "label");
        if (field(label, "name") != "agent:ready") continue;
        json_object* actor = nullptr;
        json_object_object_get_ex(event, "actor", &actor);
        approved.labeler = optional_login(actor);
    }

    std::string reason;
    if (!has_ready_label) reason = "agent:ready label is absent";
    else if (!trusted_users.count(approved.author)) reason = "author @" + approved.author + " is not trusted";
    else if (!approved.editor.empty() && !trusted_users.count(approved.editor))
        reason = "last editor @" + approved.editor + " is not trusted";
    else if (!trusted_users.count(approved.labeler))
        reason = "latest agent:ready labeler @" + approved.labeler + " is not trusted";
    if (!reason.empty()) {
        if (explain_rejection) std::cout << "[worker] skipping issue #" << number << ": " << reason << '\n';
        return std::nullopt;
    }
    return approved;
}

bool has_pull_request(const CommandResult& result) {
    auto pull_requests = parse_json(result.output);
    if (!pull_requests || json_object_get_type(pull_requests.get()) != json_type_array)
        throw std::runtime_error("Expected a pull request array");
    return json_object_array_length(pull_requests.get()) != 0;
}

std::filesystem::path state_path_for(const std::filesystem::path& worktree) {
    return worktree.parent_path() / (worktree.filename().string() + ".worker-state.json");
}

std::string required_string(json_object* object, const char* name) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value) || !value ||
        json_object_get_type(value) != json_type_string)
        throw std::runtime_error(std::string("Invalid worker state field: ") + name);
    return {json_object_get_string(value), static_cast<size_t>(json_object_get_string_len(value))};
}

JobState read_state(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Resume state is missing: " + path.string());
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto value = parse_json(contents);
    if (!value || json_object_get_type(value.get()) != json_type_object)
        throw std::runtime_error("Invalid worker state: " + path.string());
    JobState state;
    state.stage = required_string(value.get(), "stage");
    state.repository = required_string(value.get(), "repository");
    state.issue = required_string(value.get(), "issue");
    state.title = required_string(value.get(), "title");
    state.body = required_string(value.get(), "body");
    state.branch = required_string(value.get(), "branch");
    state.worktree = required_string(value.get(), "worktree");
    state.source_root = required_string(value.get(), "source_root");
    state.base_commit = required_string(value.get(), "base_commit");
    state.base_branch = required_string(value.get(), "base_branch");
    state.fingerprint = required_string(value.get(), "fingerprint");
    state.commit = required_string(value.get(), "commit");
    json_object* paths = nullptr;
    if (!json_object_object_get_ex(value.get(), "paths", &paths) || !paths ||
        json_object_get_type(paths) != json_type_array)
        throw std::runtime_error("Invalid worker state field: paths");
    for (size_t index = 0; index < json_object_array_length(paths); ++index) {
        auto* item = json_object_array_get_idx(paths, index);
        if (!item || json_object_get_type(item) != json_type_string)
            throw std::runtime_error("Invalid worker state path");
        state.paths.emplace_back(json_object_get_string(item), json_object_get_string_len(item));
    }
    return state;
}

void write_state(const std::filesystem::path& path, const JobState& state) {
    std::unique_ptr<json_object, decltype(&json_object_put)> value(json_object_new_object(), &json_object_put);
    if (!value) throw std::runtime_error("Cannot allocate worker state");
    const auto add = [&](const char* name, const std::string& field_value) {
        json_object_object_add(value.get(), name,
            json_object_new_string_len(field_value.data(), static_cast<int>(field_value.size())));
    };
    add("stage", state.stage);
    add("repository", state.repository);
    add("issue", state.issue);
    add("title", state.title);
    add("body", state.body);
    add("branch", state.branch);
    add("worktree", state.worktree);
    add("source_root", state.source_root);
    add("base_commit", state.base_commit);
    add("base_branch", state.base_branch);
    add("fingerprint", state.fingerprint);
    add("commit", state.commit);
    auto* paths = json_object_new_array();
    for (const auto& item : state.paths)
        json_object_array_add(paths, json_object_new_string_len(item.data(), static_cast<int>(item.size())));
    json_object_object_add(value.get(), "paths", paths);
    const auto temporary = path.string() + ".tmp." + std::to_string(getpid());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot write worker state: " + temporary);
        output << json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PRETTY) << '\n';
        if (!output) throw std::runtime_error("Cannot write worker state: " + temporary);
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("Cannot replace worker state: " + error.message());
    }
}

struct WorktreeRecord {
    std::filesystem::path path;
    std::string branch;
};

std::optional<WorktreeRecord> registered_worktree(const std::string& text,
                                                   const std::filesystem::path& expected) {
    std::istringstream input(text);
    std::string line;
    std::optional<WorktreeRecord> current;
    while (std::getline(input, line)) {
        if (line.rfind("worktree ", 0) == 0) {
            if (current && current->path == expected) return current;
            current = WorktreeRecord{line.substr(9), ""};
        } else if (current && line.rfind("branch refs/heads/", 0) == 0) {
            current->branch = line.substr(18);
        } else if (line.empty() && current) {
            if (current->path == expected) return current;
            current.reset();
        }
    }
    if (current && current->path == expected) return current;
    return std::nullopt;
}

std::vector<std::string> changed_paths(const std::string& status) {
    std::vector<std::string> paths;
    size_t position = 0;
    while (position < status.size()) {
        const auto end = status.find('\0', position);
        if (end == std::string::npos) throw std::runtime_error("Invalid git status output");
        const auto entry = status.substr(position, end - position);
        position = end + 1;
        if (entry.size() < 4 || entry[2] != ' ') throw std::runtime_error("Invalid git status entry");
        const bool rename = entry[0] == 'R' || entry[0] == 'C' || entry[1] == 'R' || entry[1] == 'C';
        paths.push_back(entry.substr(3));
        if (rename) {
            const auto old_end = status.find('\0', position);
            if (old_end == std::string::npos) throw std::runtime_error("Invalid renamed path");
            paths.push_back(status.substr(position, old_end - position));
            position = old_end + 1;
        }
    }
    return paths;
}

std::string change_fingerprint(const std::filesystem::path& worktree,
                               const std::string& status,
                               const std::vector<std::string>& paths) {
    uint64_t hash = 1469598103934665603ULL;
    auto consume = [&](const char* data, size_t size) {
        for (size_t index = 0; index < size; ++index) {
            hash ^= static_cast<unsigned char>(data[index]);
            hash *= 1099511628211ULL;
        }
    };
    consume(status.data(), status.size());
    for (const auto& name : paths) {
        consume(name.data(), name.size());
        const auto path = worktree / name;
        std::error_code error;
        const auto file_status = std::filesystem::symlink_status(path, error);
        if (!error && std::filesystem::is_symlink(file_status)) {
            const auto target = std::filesystem::read_symlink(path, error).generic_string();
            if (error) throw std::runtime_error("Cannot fingerprint changed symlink: " + name);
            consume(target.data(), target.size());
        } else if (!error && std::filesystem::is_regular_file(file_status)) {
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot fingerprint changed path: " + name);
            char buffer[8192];
            while (input.read(buffer, sizeof buffer) || input.gcount() != 0)
                consume(buffer, static_cast<size_t>(input.gcount()));
        } else {
            static const char missing[] = "<missing>";
            consume(missing, sizeof missing - 1);
        }
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

bool sensitive_path(const std::string& value) {
    const std::filesystem::path path(value);
    if (path.empty() || path.is_absolute()) return true;
    for (const auto& component_path : path) {
        const auto component = component_path.string();
        if (component == ".." || component == ".git" || component == "id_rsa" || component == "id_ed25519" ||
            component == "credentials" || component == "secrets" || component == ".env" ||
            component.rfind(".env.", 0) == 0) return true;
    }
    const auto extension = path.extension().string();
    return extension == ".pem" || extension == ".key" || extension == ".p12" || extension == ".pfx";
}

bool validate_paths(const std::filesystem::path& worktree, const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        const auto normalized = std::filesystem::path(path).lexically_normal();
        if (sensitive_path(path) || normalized.empty() || *normalized.begin() == "..") {
            std::cerr << "Change validation rejected path: " << path << '\n';
            return false;
        }
        std::error_code error;
        const auto status = std::filesystem::symlink_status(worktree / normalized, error);
        if (!error && std::filesystem::is_symlink(status)) {
            const auto target = std::filesystem::read_symlink(worktree / normalized, error);
            if (error || target.is_absolute() ||
                (normalized.parent_path() / target).lexically_normal().string().rfind("..", 0) == 0) {
                std::cerr << "Change validation rejected escaping symlink: " << path << '\n';
                return false;
            }
        }
    }
    return true;
}

bool makefile_has_test_target(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("test:", 0) == 0 || line.rfind("test :", 0) == 0) return true;
    }
    return false;
}

std::optional<Verification> verification_at(const std::filesystem::path& directory) {
    if (std::filesystem::exists(directory / "Makefile") && makefile_has_test_target(directory / "Makefile"))
        return Verification{directory, {"make", "test"}, "make test (" + directory.filename().string() + "/)"};
    if (std::filesystem::exists(directory / "Cargo.toml"))
        return Verification{directory, {"cargo", "test"}, "cargo test (" + directory.filename().string() + "/)"};
    if (std::filesystem::exists(directory / "go.mod"))
        return Verification{directory, {"go", "test", "./..."}, "go test ./... (" + directory.filename().string() + "/)"};
    if (std::filesystem::exists(directory / "pom.xml"))
        return Verification{directory, {"mvn", "test"}, "mvn test (" + directory.filename().string() + "/)"};
    if (std::filesystem::exists(directory / "gradlew"))
        return Verification{directory, {"./gradlew", "test"}, "./gradlew test (" + directory.filename().string() + "/)"};
    const auto package = directory / "package.json";
    if (std::filesystem::exists(package)) {
        std::ifstream input(package, std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        try {
            auto value = parse_json(contents);
            json_object* scripts = nullptr;
            json_object* test = nullptr;
            if (value && json_object_get_type(value.get()) == json_type_object &&
                json_object_object_get_ex(value.get(), "scripts", &scripts) && scripts &&
                json_object_get_type(scripts) == json_type_object &&
                json_object_object_get_ex(scripts, "test", &test) && test &&
                json_object_get_type(test) == json_type_string)
                return Verification{directory, {"npm", "test"}, "npm test (" + directory.filename().string() + "/)"};
        } catch (const std::exception&) {
            // Invalid project metadata is not a safe basis for an inferred command.
        }
    }
    return std::nullopt;
}

std::vector<Verification> verifications_for(const std::filesystem::path& worktree,
                                             const std::vector<std::string>& paths) {
    std::map<std::string, Verification> selected;
    for (const auto& changed : paths) {
        auto directory = (worktree / changed).parent_path();
        while (true) {
            if (const auto verification = verification_at(directory)) {
                selected.emplace(directory.string(), *verification);
                break;
            }
            if (directory == worktree) break;
            const auto parent = directory.parent_path();
            if (parent == directory || directory.string().rfind(worktree.string(), 0) != 0) break;
            directory = parent;
        }
    }
    std::vector<Verification> result;
    for (auto& item : selected) result.push_back(std::move(item.second));
    return result;
}

std::string repository_slug(std::string repository) {
    std::string result;
    for (const unsigned char character : repository) {
        const bool safe = std::isalnum(character) || character == '-' || character == '_';
        if (safe) result.push_back(static_cast<char>(character));
        else if (result.empty() || result.back() != '-') result.push_back('-');
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    if (result.empty()) throw std::runtime_error("Repository name has no safe slug");
    if (result.size() > 100) result.resize(100);
    return result;
}

int run(const Options& options) {
    const auto root = repository_path(options);
    log_stage("validating repository");
    const auto remote_result = run_command({"git", "remote", "get-url", "origin"}, root);
    if (remote_result.exit_code != 0) return command_failed("Repository validation: git remote get-url origin", remote_result);
    const auto actual_repository = repository_from_remote(remote_result.output);
    if (!options.repository.empty() && actual_repository != options.repository) {
        std::cerr << "Repository validation failed: requested " << options.repository << " but origin is " << actual_repository << '\n';
        return 1;
    }
    const auto& repository = actual_repository;
    std::cout << "Repository root: " << root << "\nRepository: " << repository << '\n';
    ExecutionLock lock(repository + "-" + std::to_string(std::hash<std::string>{}(root.string())));

    log_stage("checking GitHub authentication");
    const auto auth_result = run_command({"gh", "auth", "status"}, root);
    if (auth_result.exit_code != 0) return command_failed("GitHub authentication", auth_result);

    auto trusted_users = options.trusted_users;
    if (trusted_users.empty()) {
        const auto viewer_result = run_command({"gh", "api", "user", "--jq", ".login"}, root);
        if (viewer_result.exit_code != 0) return command_failed("Authenticated user lookup", viewer_result);
        const auto viewer = trim(viewer_result.output);
        if (viewer.empty()) throw std::runtime_error("Authenticated GitHub login is empty");
        trusted_users.insert(lowercase(viewer));
    }
    std::cout << "Trusted GitHub users:";
    for (const auto& user : trusted_users) std::cout << " @" << user;
    std::cout << '\n';

    log_stage("selecting oldest trusted issue");
    const auto issue_result = run_command({"gh", "issue", "list", "--repo", repository, "--state", "open",
        "--label", "agent:ready", "--search", "sort:created-asc", "--limit", "100", "--json", "number"}, root);
    if (issue_result.exit_code != 0) return command_failed("Issue lookup: gh issue list", issue_result);
    auto issues = parse_json(issue_result.output);
    if (!issues || json_object_get_type(issues.get()) != json_type_array) throw std::runtime_error("Expected an Issue array");
    std::optional<ApprovedIssue> selected_issue;
    for (size_t index = 0; index < json_object_array_length(issues.get()); ++index) {
        auto* candidate = json_object_array_get_idx(issues.get(), index);
        if (!candidate || json_object_get_type(candidate) != json_type_object)
            throw std::runtime_error("Expected an Issue object");
        selected_issue = approved_issue(repository, field(candidate, "number"), trusted_users, root);
        if (selected_issue) break;
    }
    if (!selected_issue) {
        std::cout << "No trusted approved issues; nothing to do.\n";
        return 0;
    }
    const auto number = selected_issue->number;
    const auto title = selected_issue->title;
    const auto body = selected_issue->body;
    std::cout << "Issue number: " << number << "\nTitle: " << title << "\nBody:\n" << body << '\n';
    const auto approval_still_valid = [&](const std::string& stage) {
        const auto current = approved_issue(repository, number, trusted_users, root, false);
        if (!current || current->title != title || current->body != body || current->author != selected_issue->author) {
            std::cerr << "Trusted Issue approval changed before " << stage << "; refusing to continue.\n";
            return false;
        }
        return true;
    };

    const auto slug = repository_slug(repository);
    const auto branch = "agent/" + slug + "-issue-" + number;
    const auto worktree = root.parent_path() / (root.filename().string() + "-" + slug + "-issue-" + number);
    const auto state_path = state_path_for(worktree);
    const std::vector<std::string> pr_list_command = {"gh", "pr", "list", "--repo", repository,
        "--state", "all", "--head", branch, "--limit", "1", "--json", "number,url"};

    log_stage("checking existing pull request and workspace state");
    const auto worktree_list_result = run_command({"git", "worktree", "list", "--porcelain"}, root);
    if (worktree_list_result.exit_code != 0) return command_failed("Existing worktree check: git worktree list", worktree_list_result);
    const std::vector<std::string> status_command = {"git", "status", "--porcelain=v1", "-z", "--untracked-files=all"};
    const auto record = registered_worktree(worktree_list_result.output, worktree);
    JobState state;

    if (options.resume) {
        if (!std::filesystem::exists(worktree) || !record) {
            std::cerr << "No registered existing worktree is available to resume: " << worktree << '\n';
            return 1;
        }
        if (record->branch != branch) {
            std::cerr << "Existing worktree branch mismatch; preserving path: " << worktree << '\n';
            return 1;
        }
        state = read_state(state_path);
        if (state.repository != repository || state.issue != number || state.branch != branch ||
            state.worktree != worktree.string() || state.source_root != root.string()) {
            std::cerr << "Resume state does not identify this repository, Issue, branch, and worktree; preserving files.\n";
            return 1;
        }
        if (state.title != title || state.body != body) {
            std::cerr << "Issue content changed since the saved work; preserving files and refusing resume.\n";
            return 1;
        }
        const auto worktree_remote = run_command({"git", "remote", "get-url", "origin"}, worktree);
        if (worktree_remote.exit_code != 0)
            return command_failed("Worktree repository validation: git remote get-url origin", worktree_remote);
        if (repository_from_remote(worktree_remote.output) != repository) {
            std::cerr << "Existing worktree repository mismatch; preserving files.\n";
            return 1;
        }
        const auto head_result = run_command({"git", "rev-parse", "HEAD"}, worktree);
        if (head_result.exit_code != 0) return command_failed("Resume HEAD lookup: git rev-parse HEAD", head_result);
        const auto head = trim(head_result.output);
        if ((state.stage == "IMPLEMENTED" || state.stage == "VERIFYING" || state.stage == "VERIFIED") &&
            head != state.base_commit) {
            std::cerr << "Work basis changed since the saved run; preserving files and refusing resume.\n";
            return 1;
        }
        if ((state.stage == "COMMITTED" || state.stage == "PUSHED" || state.stage == "PR_CREATED") &&
            (state.commit.empty() || head != state.commit)) {
            std::cerr << "Saved commit does not match the existing worktree; preserving files and refusing resume.\n";
            return 1;
        }
        if (state.stage == "NEW" || state.stage == "CODEX_RUNNING" || state.stage == "BLOCKED") {
            std::cerr << "Codex completion is not proven; preserving existing changes. Explicit recovery is required.\n";
            return 1;
        }
        std::cout << "Resuming saved job at state " << state.stage << ".\n";
    } else {
        if (record || std::filesystem::exists(worktree) || std::filesystem::exists(state_path)) {
            std::cerr << "Existing worktree conflict; preserving it. Rerun with --resume after inspection: " << worktree << '\n';
            return 1;
        }
        const auto initial_pr_result = run_command(pr_list_command, root);
        if (initial_pr_result.exit_code != 0) return command_failed("Existing PR lookup: gh pr list", initial_pr_result);
        if (has_pull_request(initial_pr_result)) {
            std::cout << "A pull request already exists for " << branch << "; skipping work.\n";
            return 0;
        }
        const auto branch_result = run_command({"git", "show-ref", "--verify", "--quiet", "refs/heads/" + branch}, root);
        if (branch_result.exit_code == 0) {
            std::cerr << "Existing branch conflict; preserving branch: " << branch << '\n';
            return 1;
        }
        if (branch_result.exit_code != 1) return command_failed("Existing branch check: git show-ref", branch_result);
        const auto remote_branch_result = run_command(
            {"git", "ls-remote", "--exit-code", "--heads", "origin", branch}, root);
        if (remote_branch_result.exit_code == 0) {
            std::cerr << "Existing remote branch conflict; preserving branch: " << branch << '\n';
            return 1;
        }
        if (remote_branch_result.exit_code != 2)
            return command_failed("Existing remote branch check: git ls-remote", remote_branch_result);
        std::cout << "Worktree: " << worktree << "\nBranch: " << branch << '\n';
        log_stage("creating isolated worktree");
        const auto worktree_result = run_command({"git", "worktree", "add", "-b", branch, worktree.string()}, root);
        if (worktree_result.exit_code != 0) return command_failed("Worktree creation: git worktree add", worktree_result);
        print_streams(worktree_result);
        const auto worktree_root_result = run_command({"git", "rev-parse", "--show-toplevel"}, worktree);
        if (worktree_root_result.exit_code != 0)
            return command_failed("Worktree root validation: git rev-parse --show-toplevel", worktree_root_result);
        if (std::filesystem::canonical(trim(worktree_root_result.output)) != std::filesystem::canonical(worktree)) {
            std::cerr << "Created worktree root does not match the intended path; preserving it.\n";
            return 1;
        }
        const auto original_head_result = run_command({"git", "rev-parse", "HEAD"}, worktree);
        if (original_head_result.exit_code != 0) return command_failed("Baseline HEAD lookup: git rev-parse HEAD", original_head_result);
        const auto base_result = run_command({"gh", "repo", "view", repository, "--json", "defaultBranchRef",
            "--jq", ".defaultBranchRef.name"}, worktree);
        if (base_result.exit_code != 0) return command_failed("Default branch lookup: gh repo view", base_result);
        const auto base = trim(base_result.output);
        if (base.empty()) throw std::runtime_error("Default branch is empty");
        state = {"NEW", repository, number, title, body, branch, worktree.string(), root.string(),
                 trim(original_head_result.output), base, "", "", {}};
        write_state(state_path, state);
        if (!approval_still_valid("Codex execution")) return 1;
        state.stage = "CODEX_RUNNING";
        write_state(state_path, state);
        const std::string prompt = "Implement the following approved GitHub Issue in this worktree.\n"
            "Treat the worktree root as the repository root. Inspect the repository-wide structure and applicable "
            "AGENTS.md instructions before deciding where to work.\n"
            "You may inspect the entire repository, but make only the smallest necessary changes and run relevant tests.\n"
            "Do not commit, push, create a pull request, or modify another worktree.\n\n"
            "Repository: " + repository + "\nRepository root: " + worktree.string() +
            "\nIssue #" + number + "\nTitle: " + title + "\n\nBody:\n" + body + "\n";
        log_stage("running Codex");
        const auto codex_result = run_command({"codex", "exec", "-m", "gpt-5.5", "--sandbox", "workspace-write",
            "--cd", worktree.string(), "-"}, worktree, prompt, options.codex_timeout_seconds);
        print_streams(codex_result);
        std::cout << "Codex exit code: " << codex_result.exit_code << '\n';
        if (codex_result.timed_out || codex_result.signaled || codex_result.exit_code != 0) {
            state.stage = "BLOCKED";
            write_state(state_path, state);
            if (codex_result.timed_out)
                std::cerr << "Codex execution timed out after " << options.codex_timeout_seconds << " seconds\n";
            else if (codex_result.signaled)
                std::cerr << "Codex execution terminated by signal " << codex_result.signal << '\n';
            else return command_failed("Codex execution", codex_result);
            return 1;
        }
        const auto head_result = run_command({"git", "rev-parse", "HEAD"}, worktree);
        if (head_result.exit_code != 0) return command_failed("Post-Codex HEAD lookup: git rev-parse HEAD", head_result);
        if (trim(head_result.output) != state.base_commit) {
            state.stage = "BLOCKED";
            write_state(state_path, state);
            std::cerr << "Codex changed HEAD; preserving worktree and refusing automatic submission.\n";
            return 1;
        }
        auto status_result = run_command(status_command, worktree);
        if (status_result.exit_code != 0) return command_failed("Change inspection: git status", status_result);
        state.paths = changed_paths(status_result.output);
        if (state.paths.empty()) {
            state.stage = "BLOCKED";
            write_state(state_path, state);
            std::cerr << "No Git changes; commit, push, and pull request are blocked.\n";
            return 1;
        }
        if (!validate_paths(worktree, state.paths)) return 1;
        state.fingerprint = change_fingerprint(worktree, status_result.output, state.paths);
        state.stage = "IMPLEMENTED";
        write_state(state_path, state);
    }

    bool pushed_commit_validated = false;
    if (state.stage == "PUSHED" || state.stage == "PR_CREATED") {
        const auto remote_head_result = run_command(
            {"git", "ls-remote", "--exit-code", "--heads", "origin", branch}, worktree);
        if (remote_head_result.exit_code != 0)
            return command_failed("Pushed branch validation: git ls-remote", remote_head_result);
        std::istringstream remote_output(remote_head_result.output);
        std::string remote_head;
        remote_output >> remote_head;
        if (remote_head != state.commit) {
            std::cerr << "Remote branch does not match the saved commit; refusing PR recovery.\n";
            return 1;
        }
        pushed_commit_validated = true;
    }

    const auto initial_pr_result = run_command(pr_list_command, worktree);
    if (initial_pr_result.exit_code != 0) return command_failed("Existing PR lookup: gh pr list", initial_pr_result);
    if (has_pull_request(initial_pr_result)) {
        state.stage = "PR_CREATED";
        write_state(state_path, state);
        std::cout << "A pull request already exists for " << branch << "; skipping creation.\n";
        return 0;
    }

    auto verifications = verifications_for(worktree, state.paths);
    if (state.stage == "IMPLEMENTED" || state.stage == "VERIFYING") {
        auto status_result = run_command(status_command, worktree);
        if (status_result.exit_code != 0) return command_failed("Resume change inspection: git status", status_result);
        const auto paths = changed_paths(status_result.output);
        if (paths.empty() || !validate_paths(worktree, paths) ||
            change_fingerprint(worktree, status_result.output, paths) != state.fingerprint) {
            std::cerr << "Saved implementation no longer matches the existing changes; preserving files.\n";
            return 1;
        }
        if (options.resume) log_stage("resuming at verification");
        if (!approval_still_valid("verification")) return 1;
        state.stage = "VERIFYING";
        write_state(state_path, state);
        verifications = verifications_for(worktree, paths);
        if (!verifications.empty()) {
            for (const auto& verification : verifications) {
                if (!std::filesystem::exists(verification.directory)) {
                    std::cerr << "Verification directory does not exist: " << verification.directory << '\n';
                    return 1;
                }
                log_stage("running detected verification: " + verification.display);
                const auto verification_result = run_command(verification.command, verification.directory);
                print_streams(verification_result);
                if (verification_result.exit_code != 0)
                    return command_failed("Verification: " + verification.display, verification_result);
            }
        } else {
            log_stage("no safe automated verification command detected");
        }
        status_result = run_command(status_command, worktree);
        if (status_result.exit_code != 0) return command_failed("Post-verification change inspection: git status", status_result);
        state.paths = changed_paths(status_result.output);
        if (state.paths.empty() || !validate_paths(worktree, state.paths)) {
            std::cerr << "No valid Git changes after verification; automatic submission is blocked.\n";
            return 1;
        }
        state.fingerprint = change_fingerprint(worktree, status_result.output, state.paths);
        state.stage = "VERIFIED";
        write_state(state_path, state);
    }

    if (state.stage == "VERIFIED") {
        if (!approval_still_valid("commit")) return 1;
        log_stage("staging and validating changes");
        std::vector<std::string> add_command = {"git", "add", "--"};
        add_command.insert(add_command.end(), state.paths.begin(), state.paths.end());
        const auto add_result = run_command(add_command, worktree);
        if (add_result.exit_code != 0) return command_failed("Staging: git add", add_result);
        const auto check_result = run_command({"git", "diff", "--cached", "--check"}, worktree);
        if (check_result.exit_code != 0) return command_failed("Validation: git diff --cached --check", check_result);
        const auto staged_result = run_command({"git", "diff", "--cached", "--quiet"}, worktree);
        if (staged_result.exit_code == 0) {
            std::cerr << "No staged Git changes; automatic submission is blocked.\n";
            return 1;
        }
        if (staged_result.exit_code != 1) return command_failed("Staged change check: git diff --cached --quiet", staged_result);
        log_stage("committing verified changes");
        const auto commit_result = run_command({"git", "commit", "-m", "feat: implement issue #" + number}, worktree);
        if (commit_result.exit_code != 0) return command_failed("Git commit", commit_result);
        const auto committed_head = run_command({"git", "rev-parse", "HEAD"}, worktree);
        if (committed_head.exit_code != 0) return command_failed("Committed HEAD lookup: git rev-parse HEAD", committed_head);
        state.commit = trim(committed_head.output);
        state.stage = "COMMITTED";
        write_state(state_path, state);
    }

    if (state.stage == "COMMITTED") {
        if (!approval_still_valid("push")) return 1;
        log_stage("pushing work branch");
        const auto push_result = run_command({"git", "push", "--set-upstream", "origin", branch}, worktree);
        if (push_result.exit_code != 0) return command_failed("Git push", push_result);
        state.stage = "PUSHED";
        write_state(state_path, state);
    } else if (state.stage == "PUSHED" && !pushed_commit_validated) {
        const auto remote_head_result = run_command(
            {"git", "ls-remote", "--exit-code", "--heads", "origin", branch}, worktree);
        if (remote_head_result.exit_code != 0)
            return command_failed("Pushed branch validation: git ls-remote", remote_head_result);
        std::istringstream remote_output(remote_head_result.output);
        std::string remote_head;
        remote_output >> remote_head;
        if (remote_head != state.commit) {
            std::cerr << "Remote branch does not match the saved commit; refusing PR recovery.\n";
            return 1;
        }
    }

    if (state.stage != "PUSHED") {
        std::cerr << "Saved job state cannot be advanced safely: " << state.stage << '\n';
        return 1;
    }
    if (!approval_still_valid("pull request creation")) return 1;
    log_stage("rechecking and creating draft pull request");
    const auto existing_pr_result = run_command(pr_list_command, worktree);
    if (existing_pr_result.exit_code != 0) return command_failed("Pre-create PR lookup: gh pr list", existing_pr_result);
    if (has_pull_request(existing_pr_result)) {
        state.stage = "PR_CREATED";
        write_state(state_path, state);
        std::cout << "A pull request already exists for " << branch << "; skipping creation.\n";
        return 0;
    }
    std::string changed_summary;
    for (const auto& path : state.paths) changed_summary += "- `" + path + "`\n";
    std::string automated_verification;
    for (const auto& verification : verifications)
        automated_verification += "- PASS: `" + verification.display + "`\n";
    if (automated_verification.empty())
        automated_verification = "- SKIPPED: no safe automated command detected\n";
    const std::string pr_body = "Closes #" + number + "\n\n## Summary\n"
        "Implemented the approved issue. Changed paths:\n" + changed_summary + "\n"
        "## Verification\n" + automated_verification + "- PASS: `git diff --cached --check`\n\n"
        "## Manual verification\n- Acceptance criteria not covered by the approved automated command require manual review.\n";
    const auto pr_result = run_command({"gh", "pr", "create", "--repo", repository, "--draft", "--base", state.base_branch,
        "--head", branch, "--title", title, "--body", pr_body}, worktree);
    if (pr_result.exit_code != 0) return command_failed("Draft PR creation: gh pr create", pr_result);
    print_streams(pr_result);
    state.stage = "PR_CREATED";
    write_state(state_path, state);
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        if (!options.scan_root) return run(options);
        const auto canonical_scan_root = std::filesystem::canonical(*options.scan_root);
        ScanExecutionLock scan_lock(canonical_scan_root);
        if (!scan_lock.acquired()) {
            std::cout << "[worker] another scan is already running for "
                      << canonical_scan_root << "; skipping\n";
            return 0;
        }
        log_stage("discovering repositories");
        const auto repositories = discover_repositories(options);
        size_t succeeded = 0;
        size_t failed = 0;
        for (const auto& repository : repositories) {
            std::cout << "\n[worker] processing repository: " << repository << '\n';
            auto repository_options = options;
            repository_options.scan_root.reset();
            repository_options.path = repository;
            try {
                if (run(repository_options) == 0) ++succeeded;
                else ++failed;
            } catch (const std::exception& error) {
                ++failed;
                std::cerr << "Worker failed for " << repository << ": " << error.what() << '\n';
            }
        }
        std::cout << "\nWorker summary:\n"
                  << "  discovered: " << repositories.size() << '\n'
                  << "  succeeded:  " << succeeded << '\n'
                  << "  failed:     " << failed << '\n';
        return failed == 0 ? 0 : 1;
    }
    catch (const std::exception& error) { std::cerr << "Worker failed: " << error.what() << '\n'; return 1; }
}
