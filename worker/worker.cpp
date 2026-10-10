#include <json-c/json.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;

struct CommandResult {
    int exit_code;
    std::string output;
    std::string errors;
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

CommandResult run_command(const std::vector<std::string>& arguments,
                          const std::filesystem::path& directory,
                          const std::optional<std::string>& input = std::nullopt) {
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
        if (input_file) {
            if (dup2(fileno(input_file.get()), STDIN_FILENO) < 0) {
                dprintf(STDERR_FILENO, "Worker failed: %s\n", std::strerror(errno));
                _exit(1);
            }
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
        _exit(1);
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error(std::strerror(errno));
    }
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return {exit_code, read_file(output.get()), read_file(errors.get())};
}

std::string field(json_object* issue, const char* name) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(issue, name, &value)) throw std::runtime_error(std::string("'") + name + "'");
    if (!value) return "None";
    switch (json_object_get_type(value)) {
    case json_type_string: return {json_object_get_string(value), static_cast<size_t>(json_object_get_string_len(value))};
    case json_type_boolean: return json_object_get_boolean(value) ? "True" : "False";
    default: return json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
    }
}

void print_streams(const CommandResult& result) {
    if (!result.output.empty()) std::cout << result.output << (result.output.back() == '\n' ? "" : "\n");
    if (!result.errors.empty()) std::cerr << result.errors << (result.errors.back() == '\n' ? "" : "\n");
}

int run(const std::filesystem::path& root) {
    const std::vector<std::string> issue_command = {
        "gh", "issue", "list", "--repo", "keplerisgone/kepp-lab", "--state", "open",
        "--label", "agent:ready", "--search", "sort:created-asc", "--limit", "1",
        "--json", "number,title,body"
    };
    const auto issue_result = run_command(issue_command, root);
    if (issue_result.exit_code != 0) {
        print_streams(issue_result);
        if (trim(issue_result.errors).empty())
            std::cerr << "gh issue list exited with code " << issue_result.exit_code << '\n';
        return 1;
    }

    const auto& text = issue_result.output;
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new(), &json_tokener_free);
    if (!parser) throw std::runtime_error("Cannot allocate JSON parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT);
    std::unique_ptr<json_object, decltype(&json_object_put)> issues(
        json_tokener_parse_ex(parser.get(), text.c_str(), static_cast<int>(text.size() + 1)), &json_object_put);
    if (json_tokener_get_error(parser.get()) != json_tokener_success)
        throw std::runtime_error(json_tokener_error_desc(json_tokener_get_error(parser.get())));
    const auto end = json_tokener_get_parse_end(parser.get());
    if (end < text.size() && !trim(text.substr(end)).empty()) throw std::runtime_error("Extra data");
    if (!issues || json_object_get_type(issues.get()) != json_type_array)
        throw std::runtime_error("Expected an Issue array");
    if (json_object_array_length(issues.get()) == 0) return 0;

    auto* issue = json_object_array_get_idx(issues.get(), 0);
    if (!issue || json_object_get_type(issue) != json_type_object)
        throw std::runtime_error("Expected an Issue object");
    const auto number = field(issue, "number");
    const auto title = field(issue, "title");
    const auto body = field(issue, "body");
    std::cout << "Issue number: " << number << "\nTitle: " << title << "\nBody:\n" << body << '\n';

    const auto branch = "agent/issue-" + number;
    const auto worktree = root.parent_path() / (root.filename().string() + "-issue-" + number);
    std::cout << "Worktree: " << worktree << "\nBranch: " << branch << '\n';
    const auto worktree_result = run_command(
        {"git", "worktree", "add", "-b", branch, worktree.string()}, root);
    print_streams(worktree_result);
    if (worktree_result.exit_code != 0) {
        std::cerr << "git worktree add exited with code " << worktree_result.exit_code << '\n';
        return 1;
    }

    const std::string prompt =
        "Implement the following approved GitHub Issue in this worktree.\n"
        "Make only the smallest necessary changes and run relevant tests.\n"
        "Do not commit, push, create a pull request, or modify another worktree.\n\n"
        "Issue #" + number + "\nTitle: " + title + "\n\nBody:\n" + body + "\n";
    const auto codex_result = run_command(
        {"codex", "exec", "--sandbox", "workspace-write", "--cd", worktree.string(), "-"},
        worktree, prompt);
    print_streams(codex_result);
    std::cout << "Codex exit code: " << codex_result.exit_code << '\n';

    const auto status_result = run_command({"git", "status", "--short"}, worktree);
    std::cout << "Git changes:\n";
    print_streams(status_result);
    if (status_result.exit_code != 0) {
        std::cerr << "git status exited with code " << status_result.exit_code << '\n';
        return 1;
    }
    return codex_result.exit_code == 0 ? 0 : 1;
}
}

int main(int, char** argv) {
    try { return run(std::filesystem::canonical(argv[0]).parent_path().parent_path()); }
    catch (const std::exception& error) { std::cerr << "Worker failed: " << error.what() << '\n'; return 1; }
}
