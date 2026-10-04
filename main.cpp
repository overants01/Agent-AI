#include <iostream>
#include <string>
#include "stringpro.hpp"
#include <fstream>
#include <filesystem>
#include <cctype>
#include <vector>
#include <algorithm>
#include <limits>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
using namespace std;

using json = nlohmann::json;
namespace fs = std::filesystem;
fs::path SANDBOX_DIR = "sandbox";
fs::path CONFIG_ENV_PATH = ".env";

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *output)
{
    size_t total_size = size * nmemb;
    output->append((char *)contents, total_size);
    return total_size;
}

void remember_exchange(json &history, size_t max_exchanges, const std::string &prompt,
                       const std::string &answer)
{
    if (max_exchanges == 0)
    {
        return;
    }

    history.push_back({{"role", "user"}, {"content", prompt}});
    history.push_back({{"role", "assistant"}, {"content", answer}});

    const size_t stored_exchanges = history.size() / 2;
    if (stored_exchanges > max_exchanges)
    {
        const size_t entries_to_remove = (stored_exchanges - max_exchanges) * 2;
        history.erase(history.begin(), history.begin() + static_cast<json::difference_type>(entries_to_remove));
    }
}

class LLMClient
{
private:
    std::string provider;
    std::string api_key;
    std::string model_name;
    std::string endpoint;
    std::string system_prompt = "";
    json conversation_history = json::array();
    size_t message_history_limit = 0;

public:
    void UseProvider(const std::string &name, const std::string &api_endpoint)
    {
        provider = name;
        endpoint = api_endpoint;
    }

    void UseKey(const std::string &key)
    {
        api_key = key;
    }

    void UseModel(const std::string &model)
    {
        model_name = model;
    }

    void SetMessageHistoryLimit(size_t exchanges)
    {
        message_history_limit = exchanges;
    }

    void SetSystemPrompt(const std::string &sys_prompt)
    {
        system_prompt = sys_prompt;
    }

    std::string askAI(const std::string &prompt)
    {
        if (api_key.empty())
        {
            return "[Error]: ยังไม่ได้ตั้งค่า API Key! ไปใช้ UseKey() ก่อนดิ";
        }

        const bool is_gemini = provider == "gemini";
        const std::string url = is_gemini
                                    ? "https://generativelanguage.googleapis.com/v1beta/models/" + model_name + ":generateContent?key=" + api_key
                                    : endpoint;

        json payload = json::object();
        if (is_gemini)
        {
            if (!system_prompt.empty())
            {
                payload["system_instruction"] = {
                    {"parts", json::array({{{"text", system_prompt}}})}};
            }

            payload["contents"] = json::array();
            for (const auto &message : conversation_history)
            {
                const std::string role = message["role"] == "assistant" ? "model" : "user";
                payload["contents"].push_back({{"role", role},
                                               {"parts", json::array({{{"text", message["content"]}}})}});
            }
            payload["contents"].push_back({{"role", "user"},
                                           {"parts", json::array({{{"text", prompt}}})}});
        }
        else
        {
            payload["model"] = model_name;
            payload["messages"] = json::array();
            if (!system_prompt.empty())
            {
                payload["messages"].push_back({{"role", "system"}, {"content", system_prompt}});
            }
            for (const auto &message : conversation_history)
            {
                payload["messages"].push_back(message);
            }
            payload["messages"].push_back({{"role", "user"}, {"content", prompt}});
        }

        std::string request_body = payload.dump();
        std::string response_string;

        CURL *curl = curl_easy_init();
        if (!curl)
        {
            return "[Error]: cURL Initialization Failed";
        }

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!is_gemini)
        {
            const std::string authorization = "Authorization: Bearer " + api_key;
            headers = curl_slist_append(headers, authorization.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            return std::string("[cURL Error]: ") + curl_easy_strerror(res);
        }

        try
        {
            json res_json = json::parse(response_string);

            std::string answer;
            if (is_gemini && res_json.contains("candidates") && !res_json["candidates"].empty() &&
                res_json["candidates"][0].contains("content") &&
                res_json["candidates"][0]["content"].contains("parts") &&
                !res_json["candidates"][0]["content"]["parts"].empty())
            {
                answer = res_json["candidates"][0]["content"]["parts"][0]["text"].get<std::string>();
            }
            else if (!is_gemini && res_json.contains("choices") && !res_json["choices"].empty() &&
                     res_json["choices"][0].contains("message") &&
                     res_json["choices"][0]["message"].contains("content") &&
                     res_json["choices"][0]["message"]["content"].is_string())
            {
                answer = res_json["choices"][0]["message"]["content"].get<std::string>();
            }

            if (!answer.empty())
            {
                remember_exchange(conversation_history, message_history_limit, prompt, answer);
                return answer;
            }

            if (res_json.contains("error"))
            {
                const auto &error = res_json["error"];
                const std::string detail = error.is_object() && error.contains("message") &&
                                                   error["message"].is_string()
                                               ? error["message"].get<std::string>()
                                               : error.dump();
                return "[API Error]: " + detail;
            }
            return "[Error]: Provider response did not contain a text answer. Response: " + response_string;
        }
        catch (const std::exception &e)
        {
            return std::string("[JSON Parse Error]: ") + e.what() + "\nRaw: " + response_string;
        }
    }
};

bool extract_quoted_value(const string &text, const string &marker, string &value,
                          size_t search_from, size_t &field_end)
{
    const size_t marker_start = text.find(marker, search_from);
    if (marker_start == string::npos)
    {
        return false;
    }

    const size_t value_start = marker_start + marker.size();
    const size_t quote_end = text.find('\'', value_start);
    if (quote_end == string::npos)
    {
        return false;
    }

    value = text.substr(value_start, quote_end - value_start);
    field_end = quote_end + 1;
    return true;
}

bool is_safe_filename(const string &filename)
{
    const fs::path path(filename);
    return !filename.empty() && filename != "." && filename != ".." &&
           !path.is_absolute() && path.filename() == path &&
           filename.find('/') == string::npos && filename.find('\\') == string::npos;
}

struct FileResult
{
    bool success;
    string message;
};

struct AppConfig
{
    string api_key;
    string llm;
    string model;
    string sandbox_path = "sandbox";
    size_t message_history = 0;
};

string trim_copy(string value)
{
    const auto first = find_if_not(value.begin(), value.end(), [](unsigned char character)
                                   { return isspace(character); });
    const auto last = find_if_not(value.rbegin(), value.rend(), [](unsigned char character)
                                  { return isspace(character); })
                          .base();
    if (first >= last)
    {
        return "";
    }
    return string(first, last);
}

FileResult load_env_config(const fs::path &path, AppConfig &config)
{
    ifstream file(path);
    if (!file)
    {
        return {false, "Cannot open .env. Create it from .env.example and set your API key."};
    }

    string line;
    while (getline(file, line))
    {
        line = trim_copy(line);
        if (line.empty() || line.front() == '#')
        {
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == string::npos)
        {
            continue;
        }

        const string key = trim_copy(line.substr(0, equals));
        string value = trim_copy(line.substr(equals + 1));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }

        if (key == "API_KEY")
        {
            config.api_key = value;
        }
        else if (key == "LLM")
        {
            config.llm = value;
        }
        else if (key == "MODEL")
        {
            config.model = value;
        }
        else if (key == "SANDBOX_PATH")
        {
            config.sandbox_path = value;
        }
        else if (key == "MESSAGE_HISTORY")
        {
            if (value.empty() || !all_of(value.begin(), value.end(), [](unsigned char character)
                                         { return isdigit(character); }))
            {
                return {false, "MESSAGE_HISTORY must be a non-negative whole number."};
            }
            try
            {
                const unsigned long long parsed = stoull(value);
                if (parsed > numeric_limits<size_t>::max())
                {
                    return {false, "MESSAGE_HISTORY is too large."};
                }
                config.message_history = static_cast<size_t>(parsed);
            }
            catch (const exception &)
            {
                return {false, "MESSAGE_HISTORY is too large."};
            }
        }
    }

    if (config.api_key.empty() || config.llm.empty() || config.model.empty() || config.sandbox_path.empty())
    {
        return {false, ".env must define non-empty API_KEY, LLM, MODEL, and SANDBOX_PATH values."};
    }
    return {true, ""};
}

FileResult ensure_sandbox_root(bool create_if_missing)
{
    error_code error;
    if (create_if_missing)
    {
        fs::create_directories(SANDBOX_DIR, error);
        if (error)
        {
            return {false, "Cannot create sandbox directory: " + error.message()};
        }
    }

    error.clear();
    const fs::file_status status = fs::symlink_status(SANDBOX_DIR, error);
    if (error)
    {
        return {false, "Sandbox directory is unavailable: " + error.message()};
    }
    if (fs::is_symlink(status) || !fs::is_directory(status))
    {
        return {false, "Sandbox path must be a real directory."};
    }
    return {true, ""};
}

FileResult write_file_command(const string &command)
{
    string name;
    size_t name_end = 0;
    if (!extract_quoted_value(command, ".name: '", name, 0, name_end))
    {
        return {false, "Invalid file command: missing .name field."};
    }

    string message;
    size_t message_end = 0;
    const size_t message_start = command.find(".message: '", name_end);
    const size_t new_marker = command.find(".new", name_end);
    if (new_marker == string::npos)
    {
        return {false, "Invalid file command: missing .new marker."};
    }
    if (message_start != string::npos && message_start < new_marker &&
        !extract_quoted_value(command, ".message: '", message, name_end, message_end))
    {
        return {false, "Invalid file command: malformed .message field."};
    }

    const size_t fence_start = command.find("```", new_marker);
    if (fence_start == string::npos)
    {
        return {false, "Invalid file command: missing code fence."};
    }

    const size_t language_start = fence_start + 3;
    const size_t content_start = command.find('\n', language_start);
    if (content_start == string::npos)
    {
        return {false, "Invalid file command: use a real newline after the language."};
    }

    string extension = command.substr(language_start, content_start - language_start);
    while (!extension.empty() && isspace(static_cast<unsigned char>(extension.back())))
    {
        extension.pop_back();
    }
    while (!extension.empty() && isspace(static_cast<unsigned char>(extension.front())))
    {
        extension.erase(extension.begin());
    }

    const size_t fence_end = command.rfind("```");
    if (fence_end == string::npos || fence_end < content_start + 1 || extension.empty() || name.empty())
    {
        return {false, "Invalid file command: incomplete code block."};
    }
    for (unsigned char character : extension)
    {
        if (!isalnum(character) && character != '_')
        {
            return {false, "Invalid file command: invalid file extension."};
        }
    }

    const string extension_suffix = "." + extension;
    while (name.size() >= extension_suffix.size())
    {
        const size_t suffix_start = name.size() - extension_suffix.size();
        bool has_extension = true;
        for (size_t index = 0; index < extension_suffix.size(); index++)
        {
            const unsigned char name_character = static_cast<unsigned char>(name[suffix_start + index]);
            const unsigned char extension_character = static_cast<unsigned char>(extension_suffix[index]);
            if (tolower(name_character) != tolower(extension_character))
            {
                has_extension = false;
                break;
            }
        }
        if (!has_extension)
        {
            break;
        }
        name.erase(suffix_start);
    }
    if (name.empty())
    {
        return {false, "Invalid filename: provide a basename before the extension."};
    }

    const string filename = name + extension_suffix;
    if (!is_safe_filename(filename))
    {
        return {false, "Invalid filename: only files directly inside sandbox are allowed."};
    }
    if (filename == "main.cpp")
    {
        return {false, "Refusing to create protected filename main.cpp."};
    }

    const FileResult sandbox = ensure_sandbox_root(true);
    if (!sandbox.success)
    {
        return sandbox;
    }

    const fs::path target = SANDBOX_DIR / filename;
    error_code error;
    error.clear();
    const fs::file_status status = fs::symlink_status(target, error);
    if (!error && fs::is_symlink(status))
    {
        return {false, "Refusing to write through a sandbox symlink."};
    }
    if (error && error != errc::no_such_file_or_directory)
    {
        return {false, "Cannot inspect sandbox file: " + error.message()};
    }
    if (!error && fs::is_directory(status))
    {
        return {false, "Cannot write a file over a directory."};
    }

    ofstream file(target, ios::binary | ios::trunc);
    if (!file)
    {
        return {false, "Cannot open sandbox file for writing."};
    }
    file.write(command.data() + content_start + 1,
               static_cast<streamsize>(fence_end - content_start - 1));
    file.close();
    if (!file)
    {
        return {false, "Failed while writing sandbox file."};
    }

    return {true, message.empty() ? "Created " + filename : message};
}

vector<FileResult> write_file_commands(const string &response)
{
    const string name_marker = ".name: '";
    vector<string> commands;
    size_t cursor = 0;

    while (cursor < response.size())
    {
        while (cursor < response.size() && isspace(static_cast<unsigned char>(response[cursor])))
        {
            cursor++;
        }
        if (cursor == response.size())
        {
            break;
        }
        if (response.compare(cursor, name_marker.size(), name_marker) != 0)
        {
            return {{false, "Invalid multi-file response: expected another .name command."}};
        }

        const size_t new_marker = response.find(".new", cursor + name_marker.size());
        const size_t fence_start = response.find("```", new_marker);
        const size_t content_start = fence_start == string::npos
                                         ? string::npos
                                         : response.find('\n', fence_start + 3);
        const size_t fence_end = content_start == string::npos
                                     ? string::npos
                                     : response.find("```", content_start + 1);
        if (new_marker == string::npos || fence_start == string::npos ||
            content_start == string::npos || fence_end == string::npos)
        {
            return {{false, "Invalid multi-file response: incomplete file command."}};
        }

        commands.push_back(response.substr(cursor, fence_end + 3 - cursor));
        cursor = fence_end + 3;
    }

    if (commands.empty())
    {
        return {{false, "Invalid multi-file response: no file commands found."}};
    }

    vector<FileResult> results;
    results.reserve(commands.size());
    for (const string &command : commands)
    {
        results.push_back(write_file_command(command));
    }
    return results;
}

FileResult read_sandbox_file(const string &filename, string &content)
{
    if (!is_safe_filename(filename) || filename == "main.cpp")
    {
        return {false, "Invalid or protected filename; only sandbox files can be read."};
    }

    const FileResult sandbox = ensure_sandbox_root(false);
    if (!sandbox.success)
    {
        return sandbox;
    }

    error_code error;
    const fs::path target = SANDBOX_DIR / filename;
    const fs::file_status status = fs::symlink_status(target, error);
    if (error || !fs::exists(status))
    {
        return {false, "Sandbox file not found: " + filename};
    }
    if (fs::is_symlink(status) || !fs::is_regular_file(status))
    {
        return {false, "Only regular files directly inside sandbox can be read."};
    }

    ifstream file(target, ios::binary);
    if (!file)
    {
        return {false, "Cannot open sandbox file for reading."};
    }
    content.assign(istreambuf_iterator<char>(file), istreambuf_iterator<char>());
    return {true, "Read " + filename};
}

FileResult remove_sandbox_file(const string &filename)
{
    if (!is_safe_filename(filename) || filename == "main.cpp")
    {
        return {false, "Invalid or protected filename; only sandbox files can be deleted."};
    }

    const FileResult sandbox = ensure_sandbox_root(false);
    if (!sandbox.success)
    {
        return sandbox;
    }

    const fs::path target = SANDBOX_DIR / filename;
    error_code error;
    const fs::file_status status = fs::symlink_status(target, error);
    if (error || !fs::exists(status))
    {
        return {false, "Sandbox file not found: " + filename};
    }
    if (fs::is_symlink(status) || !fs::is_regular_file(status))
    {
        return {false, "Only regular files directly inside sandbox can be deleted."};
    }

    if (!fs::remove(target, error) || error)
    {
        return {false, "Failed to delete sandbox file: " + filename};
    }
    return {true, "Deleted " + filename};
}

vector<FileResult> remove_sandbox_files(const string &response)
{
    const string marker = ".rm: '";
    vector<string> filenames;
    size_t cursor = 0;

    while (cursor < response.size())
    {
        while (cursor < response.size() && isspace(static_cast<unsigned char>(response[cursor])))
        {
            cursor++;
        }
        if (cursor == response.size())
        {
            break;
        }
        if (response.compare(cursor, marker.size(), marker) != 0)
        {
            return {{false, "Invalid .rm batch: expected another .rm command."}};
        }

        string filename;
        size_t field_end = 0;
        if (!extract_quoted_value(response, marker, filename, cursor, field_end))
        {
            return {{false, "Invalid .rm batch: malformed filename."}};
        }
        filenames.push_back(filename);
        cursor = field_end;
    }

    if (filenames.empty())
    {
        return {{false, "Invalid .rm batch: no filenames found."}};
    }

    vector<FileResult> results;
    results.reserve(filenames.size());
    for (const string &filename : filenames)
    {
        results.push_back(remove_sandbox_file(filename));
    }
    return results;
}

FileResult list_sandbox_files(string &listing)
{
    const FileResult sandbox = ensure_sandbox_root(true);
    if (!sandbox.success)
    {
        return sandbox;
    }

    vector<string> filenames;
    error_code error;
    for (fs::directory_iterator entry(SANDBOX_DIR, error), end; entry != end && !error; entry.increment(error))
    {
        const fs::file_status status = entry->symlink_status(error);
        if (!error && fs::is_regular_file(status))
        {
            filenames.push_back(entry->path().filename().string());
        }
        error.clear();
    }
    if (error)
    {
        return {false, "Cannot list sandbox files: " + error.message()};
    }

    sort(filenames.begin(), filenames.end());
    if (filenames.empty())
    {
        listing = "Sandbox is empty.";
        return {true, listing};
    }

    listing = "Files in sandbox:\n";
    for (const string &filename : filenames)
    {
        listing += "- " + filename + "\n";
    }
    return {true, listing};
}

bool is_ls_command(const string &response)
{
    return response == ".ls" || start_with(response, ".ls\n") || start_with(response, ".ls\r");
}

bool is_rm_command(const string &response)
{
    return start_with(response, ".rm:");
}

bool terminal_has_color()
{
    return isatty(STDOUT_FILENO) && getenv("NO_COLOR") == nullptr;
}

size_t terminal_width()
{
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col >= 40)
    {
        return size.ws_col;
    }
    return 80;
}

string trim_left_copy(const string &value)
{
    const auto first = find_if_not(value.begin(), value.end(), [](unsigned char character)
                                   { return isspace(character); });
    return string(first, value.end());
}

void print_wrapped_line(const string &line, size_t width)
{
    if (line.empty())
    {
        cout << "  \n";
        return;
    }

    size_t leading = 0;
    while (leading < line.size() && (line[leading] == ' ' || line[leading] == '\t'))
    {
        leading++;
    }
    string prefix = "  " + line.substr(0, leading);
    string continuation = prefix;
    size_t content_start = leading;
    if (content_start + 2 <= line.size() &&
        (line.compare(content_start, 2, "- ") == 0 || line.compare(content_start, 2, "* ") == 0))
    {
        prefix += line.substr(content_start, 2);
        continuation += "  ";
        content_start += 2;
    }
    else
    {
        size_t marker_end = content_start;
        while (marker_end < line.size() && isdigit(static_cast<unsigned char>(line[marker_end])))
        {
            marker_end++;
        }
        if (marker_end > content_start && marker_end + 1 < line.size() && line[marker_end] == '.' && line[marker_end + 1] == ' ')
        {
            prefix += line.substr(content_start, marker_end + 2 - content_start);
            continuation += string(marker_end + 2 - content_start, ' ');
            content_start = marker_end + 2;
        }
    }

    istringstream words(line.substr(content_start));
    string word;
    string current = prefix;
    size_t current_width = current.size();
    const size_t max_width = max(width, current_width + 8);
    while (words >> word)
    {
        if (current_width > current.size() && current_width + 1 + word.size() > max_width)
        {
            cout << current << '\n';
            current = continuation;
            current_width = current.size();
        }
        else if (current_width > current.size())
        {
            current += ' ';
            current_width++;
        }
        current += word;
        current_width += word.size();
    }
    cout << current << '\n';
}

void print_code_line(const string &line, size_t width, bool use_color)
{
    const string first_prefix = "  ";
    const string next_prefix = "    ";
    size_t start = 0;
    bool first_line = true;
    do
    {
        const string &prefix = first_line ? first_prefix : next_prefix;
        const size_t max_bytes = max<size_t>(1, width - min(width - 1, prefix.size()));
        size_t end = min(line.size(), start + max_bytes);
        while (end < line.size() && end > start && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80)
        {
            end--;
        }
        if (end == start && start < line.size())
        {
            end = min(line.size(), start + max_bytes);
            while (end < line.size() && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80)
            {
                end++;
            }
        }

        if (use_color)
        {
            cout << "\033[2;37m";
        }
        cout << prefix << line.substr(start, end - start);
        if (use_color)
        {
            cout << "\033[0m";
        }
        cout << '\n';
        start = end;
        first_line = false;
    } while (start < line.size());
}

void print_chat_response(const string &response)
{
    const bool use_color = terminal_has_color();
    const size_t width = terminal_width() > 4 ? terminal_width() - 2 : 78;
    cout << '\n';
    if (use_color)
    {
        cout << "\033[1;36m";
    }
    cout << "AI";
    if (use_color)
    {
        cout << "\033[0m";
    }
    cout << '\n';

    istringstream lines(response);
    string line;
    bool in_code_block = false;
    while (getline(lines, line))
    {
        const string trimmed = trim_left_copy(line);
        if (start_with(trimmed, "```"))
        {
            in_code_block = !in_code_block;
            if (use_color)
            {
                cout << "\033[1;35m";
            }
            cout << "  " << trimmed;
            if (use_color)
            {
                cout << "\033[0m";
            }
            cout << '\n';
        }
        else if (in_code_block)
        {
            print_code_line(line, width, use_color);
        }
        else if (!trimmed.empty() && trimmed.front() == '#')
        {
            if (use_color)
            {
                cout << "\033[1;33m";
            }
            print_wrapped_line(line, width);
            if (use_color)
            {
                cout << "\033[0m";
            }
        }
        else
        {
            print_wrapped_line(line, width);
        }
    }
    cout << '\n';
}

void print_notice(const string &message, bool success)
{
    if (terminal_has_color())
    {
        cout << (success ? "\033[1;32m" : "\033[1;31m");
    }
    cout << (success ? "[OK] " : "[!] ") << message;
    if (terminal_has_color())
    {
        cout << "\033[0m";
    }
    cout << '\n';
}

int main()
{
    string cmd = "";
    string system_prompt = R"(You are an AI Agent with two distinct modes:

MODE 1: GENERAL CONVERSATION
- Use for greetings, questions, or general chats. Respond naturally.

MODE 2: FILE CREATION AND READING
- When creating one file, output only this command format:
.name: 'BASE_FILENAME' .message: 'MESSAGE_FOR_USER' .new ```EXTENSION
CODE_CONTENT
```
- CRITICAL: .name must never contain a file extension. Put the extension only after the opening code fence. For example, use .name: 'index' with ```html, never .name: 'index.html'.
- When a task requires multiple files, output one complete command block per file, concatenated directly with no explanation or extra text between blocks.
- Give each file its own basename, extension, and .message. Do not put multiple files' code in one block.
- The .message text is displayed to the user only after the file is successfully written.
- Files can only be created directly inside the directory configured by SANDBOX_PATH. Use a basename only, with no path and no extension in .name.
- Never create a file named main.cpp or use the basename 'main' for C++ files. This restriction is about the filename, not the C++ main() function.
- If you need the contents of an existing file, output only: .read: 'FILENAME.EXTENSION'
- .read can access regular files directly inside the configured sandbox only. Never request main.cpp, parent paths, absolute paths, or files outside the sandbox.
- After a .read request, the file contents will be sent to you as the next user message. Then answer the user's request or produce a file command.
- If the user explicitly asks to delete one or more files, output only one .rm command per file on separate lines.
- Each .rm command uses this format: .rm: 'FILENAME.EXTENSION'
- .rm can delete regular files directly inside the configured sandbox only. Never request main.cpp, parent paths, absolute paths, directories, or files outside the sandbox.
- To see which files are available, output only: .ls
- .ls returns the names of regular files directly inside the sandbox. It does not include subdirectories or follow symbolic links.
- After .ls, the file listing will be sent to you as the next user message. Use a listed filename with .read if you need its contents.
- Always use valid syntax for the target language. In C++, use double quotes for strings and single quotes only for single characters.
- Use real line breaks inside code fences and preserve source-code escape sequences such as "\\n".
- Do not add explanations outside the command when creating or reading a file.

EXACT EXAMPLES:

User: สวัสดีครับ
Assistant: สวัสดีครับ มีอะไรให้ช่วยไหมครับ?

User: นายเขียนโค้ดง่ายๆให้ดูหน่อยสิ
Assistant: .name: 'hello' .message: 'สร้างไฟล์ hello.py เรียบร้อยแล้ว' .new ```py
print('Hello World')
```

User: เขียน C++ พิมพ์ Hello World
Assistant: .name: 'hello_cpp' .message: 'สร้างไฟล์ hello_cpp.cpp เรียบร้อยแล้ว' .new ```cpp
#include <iostream>
int main() {
    std::cout << "Hello World" << std::endl;
    return 0;
}
```)";
    AppConfig config;
    CONFIG_ENV_PATH = fs::absolute(".env").lexically_normal();
    const FileResult config_result = load_env_config(CONFIG_ENV_PATH, config);
    if (!config_result.success)
    {
        cerr << "[Config Error]: " << config_result.message << endl;
        return 1;
    }

    fs::path sandbox_path(config.sandbox_path);
    if (!sandbox_path.is_absolute())
    {
        sandbox_path = CONFIG_ENV_PATH.parent_path() / sandbox_path;
    }
    SANDBOX_DIR = sandbox_path.lexically_normal();

    string provider = config.llm;
    transform(provider.begin(), provider.end(), provider.begin(), [](unsigned char character)
              { return static_cast<char>(tolower(character)); });
    string endpoint;
    if (provider == "openai")
    {
        endpoint = "https://api.openai.com/v1/chat/completions";
    }
    else if (provider == "groq")
    {
        endpoint = "https://api.groq.com/openai/v1/chat/completions";
    }
    else if (provider == "openrouter")
    {
        endpoint = "https://openrouter.ai/api/v1/chat/completions";
    }
    else if (provider != "gemini")
    {
        cerr << "[Config Error]: Unsupported LLM '" << config.llm
             << "'. Supported providers: Gemini, OpenAI, Groq, OpenRouter." << endl;
        return 1;
    }

    LLMClient g;
    g.UseProvider(provider, endpoint);
    g.SetSystemPrompt(system_prompt);
    g.UseModel(config.model);
    g.UseKey(config.api_key);
    g.SetMessageHistoryLimit(config.message_history);

    while (true)
    {
        cout << '\n';
        if (terminal_has_color())
        {
            cout << "\033[1;34m";
        }
        cout << "You > ";
        if (terminal_has_color())
        {
            cout << "\033[0m";
        }
        cout.flush();
        getline(cin, cmd);
        if (cmd == "e" || cmd == "-e")
        {
            return 0;
        }
        string response = g.askAI(cmd);
        int tool_requests = 0;
        while (start_with(response, ".read:") || is_ls_command(response))
        {
            if (++tool_requests > 5)
            {
                print_notice("Stopped after too many consecutive sandbox requests.", false);
                response.clear();
                break;
            }

            if (is_ls_command(response))
            {
                string listing;
                const FileResult list_result = list_sandbox_files(listing);
                if (!list_result.success)
                {
                    print_notice(list_result.message, false);
                    response.clear();
                    break;
                }
                response = g.askAI("Current sandbox file listing:\n" + listing +
                                   "Use only these listed sandbox files when deciding what to read.");
                continue;
            }

            string filename;
            size_t field_end = 0;
            if (!extract_quoted_value(response, ".read: '", filename, 0, field_end))
            {
                print_notice("Invalid .read command.", false);
                response.clear();
                break;
            }

            string file_content;
            const FileResult read_result = read_sandbox_file(filename, file_content);
            if (!read_result.success)
            {
                print_notice(read_result.message, false);
                response.clear();
                break;
            }

            response = g.askAI("Contents of sandbox file " + filename + ":\n" +
                               file_content + "\nUse this file content to answer the user's request.");
        }

        if (!response.empty() && start_with(response, ".name:"))
        {
            const vector<FileResult> write_results = write_file_commands(response);
            for (const FileResult &write_result : write_results)
            {
                print_notice(write_result.message, write_result.success);
            }
        }
        else if (!response.empty() && is_rm_command(response))
        {
            const vector<FileResult> remove_results = remove_sandbox_files(response);
            for (const FileResult &remove_result : remove_results)
            {
                print_notice(remove_result.message, remove_result.success);
            }
        }
        else if (!response.empty())
        {
            print_chat_response(response);
        }
        cmd = "";
    }

    return 0;
}