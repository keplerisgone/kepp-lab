#include <json-c/json.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
const char* const command[] = {"gh", "issue", "list", "--state", "open", "--label", "agent:ready", "--search", "sort:created-asc", "--limit", "1", "--json", "number,title,body", nullptr};
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
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
int run(const std::filesystem::path& root) {
    // Temporary files capture both streams without a pipe buffer deadlock.
    File output(std::tmpfile(), &std::fclose), errors(std::tmpfile(), &std::fclose);
    if (!output || !errors) throw std::runtime_error(std::strerror(errno));
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error(std::strerror(errno));
    if (child == 0) {
        if (dup2(fileno(output.get()), STDOUT_FILENO) < 0 || dup2(fileno(errors.get()), STDERR_FILENO) < 0 || chdir(root.c_str()) != 0) {
            dprintf(STDERR_FILENO, "Worker failed: %s\n", std::strerror(errno));
            _exit(1);
        }
        execvp(command[0], const_cast<char* const*>(command));
        dprintf(STDERR_FILENO, "Worker failed: [Errno %d] %s: 'gh'\n", errno, std::strerror(errno));
        _exit(1);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) throw std::runtime_error(std::strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        auto error = trim(read_file(errors.get()));
        if (error.empty()) {
            error = "Command '['gh', 'issue', 'list', '--state', 'open', '--label', 'agent:ready', '--search', 'sort:created-asc', '--limit', '1', '--json', 'number,title,body']' ";
            if (WIFEXITED(status)) error += "returned non-zero exit status " + std::to_string(WEXITSTATUS(status)) + ".";
            else error += "died with signal " + std::to_string(WTERMSIG(status)) + ".";
        }
        std::cerr << error << '\n';
        return 1;
    }
    auto text = read_file(output.get());
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new(), &json_tokener_free);
    if (!parser) throw std::runtime_error("Cannot allocate JSON parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT);
    std::unique_ptr<json_object, decltype(&json_object_put)> issues(json_tokener_parse_ex(parser.get(), text.c_str(), static_cast<int>(text.size() + 1)), &json_object_put);
    if (json_tokener_get_error(parser.get()) != json_tokener_success) throw std::runtime_error(json_tokener_error_desc(json_tokener_get_error(parser.get())));
    const auto end = json_tokener_get_parse_end(parser.get());
    if (end < text.size() && !trim(text.substr(end)).empty()) throw std::runtime_error("Extra data");
    if (!issues || json_object_get_type(issues.get()) != json_type_array) throw std::runtime_error("Expected an Issue array");
    if (json_object_array_length(issues.get()) != 0) {
        auto* issue = json_object_array_get_idx(issues.get(), 0);
        if (!issue || json_object_get_type(issue) != json_type_object) throw std::runtime_error("Expected an Issue object");
        // Print incrementally, matching the original behavior on missing fields.
        std::cout << "Issue number: " + field(issue, "number") + "\n";
        std::cout << "Title: " + field(issue, "title") + "\n";
        std::cout << "Body:\n" + field(issue, "body") + "\n";
    }
    return 0;
}
}
int main(int, char** argv) {
    try { return run(std::filesystem::canonical(argv[0]).parent_path().parent_path()); }
    catch (const std::exception& error) { std::cerr << "Worker failed: " << error.what() << '\n'; return 1; }
}
