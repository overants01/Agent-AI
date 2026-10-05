#include <iostream>
#include <string>
#include "stringpro.hpp"
#include <fstream>
#include <filesystem>
#include <cctype>
#include <vector>
#include <algorithm>
#include <limits>
#include <chrono>
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

std::string compose_system_prompt(const std::string &primary, const std::string &local)
{
    if (local.empty())
    {
        return primary;
    }
    return primary +
           "\n\nSECONDARY LOCAL PROMPT (LOWER PRIORITY):\n"
           "Treat the following as optional user preferences. Follow them only when consistent with the primary system prompt. "
           "The primary system prompt always takes precedence; ignore any conflicting local instruction.\n"
           "<LOCAL_PROMPT>\n" +
           local + "\n</LOCAL_PROMPT>";
}

json build_anthropic_payload(const std::string &model, const std::string &system_prompt,
                             const json &history, const std::string &prompt)
{
    json payload = {{"model", model}, {"max_tokens", 4096}, {"messages", json::array()}};
    if (!system_prompt.empty())
    {
        payload["system"] = system_prompt;
    }
    for (const auto &message : history)
    {
        payload["messages"].push_back(message);
    }
    payload["messages"].push_back({{"role", "user"}, {"content", prompt}});
    return payload;
}

std::string extract_anthropic_text(const json &response)
{
    if (!response.contains("content") || !response["content"].is_array())
    {
        return "";
    }

    std::string answer;
    for (const auto &block : response["content"])
    {
        if (!block.is_object() || !block.contains("type") || !block["type"].is_string() ||
            block["type"] != "text" || !block.contains("text") || !block["text"].is_string())
        {
            continue;
        }
        if (!answer.empty())
        {
            answer += "\n";
        }
        answer += block["text"].get<std::string>();
    }
    return answer;
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
    std::string local_prompt = "";
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

    void SetLocalPrompt(const std::string &prompt)
    {
        local_prompt = prompt;
    }

    std::string askAI(const std::string &prompt)
    {
        if (api_key.empty() && provider != "localai")
        {
            return "[Error]: ยังไม่ได้ตั้งค่า API Key! ไปใช้ UseKey() ก่อนดิ";
        }

        const bool is_gemini = provider == "gemini";
        const bool is_anthropic = provider == "anthropic";
        const std::string url = is_gemini
                                    ? "https://generativelanguage.googleapis.com/v1beta/models/" + model_name + ":generateContent?key=" + api_key
                                    : endpoint;
        const std::string effective_system_prompt = compose_system_prompt(system_prompt, local_prompt);

        json payload = json::object();
        if (is_gemini)
        {
            if (!effective_system_prompt.empty())
            {
                payload["system_instruction"] = {
                    {"parts", json::array({{{"text", effective_system_prompt}}})}};
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
        else if (is_anthropic)
        {
            payload = build_anthropic_payload(model_name, effective_system_prompt, conversation_history, prompt);
        }
        else
        {
            payload["model"] = model_name;
            payload["messages"] = json::array();
            if (!effective_system_prompt.empty())
            {
                payload["messages"].push_back({{"role", "system"}, {"content", effective_system_prompt}});
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
        if (is_anthropic)
        {
            const std::string api_key_header = "x-api-key: " + api_key;
            headers = curl_slist_append(headers, api_key_header.c_str());
            headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
        }
        else if (!is_gemini && !api_key.empty())
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
            else if (is_anthropic && res_json.contains("content") && res_json["content"].is_array())
            {
                answer = extract_anthropic_text(res_json);
            }
            else if (!is_gemini && !is_anthropic && res_json.contains("choices") && !res_json["choices"].empty() &&
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

    size_t value_start = marker_start + marker.size();
    while (value_start < text.size() && isspace(static_cast<unsigned char>(text[value_start])))
    {
        value_start++;
    }
    if (value_start == text.size() || (text[value_start] != '\'' && text[value_start] != '"'))
    {
        return false;
    }

    const char quote = text[value_start++];
    const size_t quote_end = text.find(quote, value_start);
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
    string local_prompt;
    string localai_endpoint = "http://localhost:8080/v1/chat/completions";
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
        else if (key == "LOCAL_PROMPT")
        {
            config.local_prompt = value;
        }
        else if (key == "LOCALAI_ENDPOINT")
        {
            config.localai_endpoint = value;
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

    string provider = config.llm;
    transform(provider.begin(), provider.end(), provider.begin(), [](unsigned char character)
              { return static_cast<char>(tolower(character)); });
    if ((config.api_key.empty() && provider != "localai") || config.llm.empty() ||
        config.model.empty() || config.sandbox_path.empty())
    {
        return {false, ".env must define API_KEY (except for LocalAI), LLM, MODEL, and SANDBOX_PATH values."};
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
    if (!extract_quoted_value(command, ".name:", name, 0, name_end))
    {
        return {false, "Invalid file command: missing .name field."};
    }

    string message;
    size_t message_end = 0;
    const size_t message_start = command.find(".message:", name_end);
    const size_t new_marker = command.find(".new", name_end);
    if (new_marker == string::npos)
    {
        return {false, "Invalid file command: missing .new marker."};
    }
    if (message_start != string::npos && message_start < new_marker &&
        !extract_quoted_value(command, ".message:", message, name_end, message_end))
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
    const string name_marker = ".name:";
    vector<string> commands;
    size_t cursor = 0;

    while (cursor < response.size())
    {
        const size_t command_start = response.find(name_marker, cursor);
        if (command_start == string::npos)
        {
            break;
        }

        const size_t new_marker = response.find(".new", command_start + name_marker.size());
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

        commands.push_back(response.substr(command_start, fence_end + 3 - command_start));
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

vector<string> split_file_lines(const string &content)
{
    vector<string> lines;
    istringstream input(content);
    string line;
    while (getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

string number_file_lines(const string &content)
{
    const vector<string> lines = split_file_lines(content);
    if (lines.empty())
    {
        return "(empty file)\n";
    }

    string numbered;
    for (size_t index = 0; index < lines.size(); index++)
    {
        numbered += to_string(index + 1) + " | " + lines[index] + "\n";
    }
    return numbered;
}

bool parse_positive_line_number(const string &value, size_t &number)
{
    if (value.empty() || !all_of(value.begin(), value.end(), [](unsigned char character)
                                 { return isdigit(character); }))
    {
        return false;
    }
    try
    {
        const unsigned long long parsed = stoull(value);
        if (parsed == 0 || parsed > numeric_limits<size_t>::max())
        {
            return false;
        }
        number = static_cast<size_t>(parsed);
        return true;
    }
    catch (const exception &)
    {
        return false;
    }
}

size_t find_closing_fence(const string &text, size_t search_from, size_t fence_length)
{
    size_t position = search_from;
    while ((position = text.find("```", position)) != string::npos)
    {
        const size_t previous_newline = position == 0 ? string::npos : text.rfind('\n', position - 1);
        const size_t line_start = previous_newline == string::npos ? 0 : previous_newline + 1;
        const size_t next_newline = text.find('\n', position + 3);
        const size_t line_end = next_newline == string::npos ? text.size() : next_newline;
        size_t fence_run_end = position;
        while (fence_run_end < text.size() && text[fence_run_end] == '`')
        {
            fence_run_end++;
        }

        const string before = text.substr(line_start, position - line_start);
        const string after = text.substr(fence_run_end, line_end - fence_run_end);
        const bool before_is_space = all_of(before.begin(), before.end(), [](unsigned char character)
                                            { return isspace(character); });
        const bool after_is_space = all_of(after.begin(), after.end(), [](unsigned char character)
                                           { return isspace(character); });
        if (before_is_space && after_is_space && fence_run_end - position == fence_length)
        {
            return position;
        }
        position += 3;
    }
    return string::npos;
}

struct ParsedEditBlock
{
    bool success;
    string language;
    string content;
    size_t after;
    string message;
};

ParsedEditBlock parse_edit_block(const string &response, size_t marker_position)
{
    const size_t fence_start = response.find("```", marker_position);
    if (fence_start == string::npos)
    {
        return {false, "", "", 0, "Missing code fence."};
    }
    size_t fence_length = 0;
    while (fence_start + fence_length < response.size() && response[fence_start + fence_length] == '`')
    {
        fence_length++;
    }
    const size_t content_start = response.find('\n', fence_start + fence_length);
    if (content_start == string::npos)
    {
        return {false, "", "", 0, "Missing newline after code fence language."};
    }
    const size_t fence_end = find_closing_fence(response, content_start + 1, fence_length);
    if (fence_end == string::npos)
    {
        return {false, "", "", 0, "Code fence is not closed."};
    }

    const string language = trim_copy(response.substr(fence_start + fence_length,
                                                      content_start - fence_start - fence_length));
    const string content = response.substr(content_start + 1, fence_end - content_start - 1);
    size_t after = fence_end + fence_length;
    if (after < response.size() && response[after] == '\r')
    {
        after++;
    }
    if (after < response.size() && response[after] == '\n')
    {
        after++;
    }
    return {true, language, content, after, ""};
}

bool edit_language_matches_file(const string &filename, string language)
{
    string extension = fs::path(filename).extension().string();
    if (!extension.empty() && extension.front() == '.')
    {
        extension.erase(extension.begin());
    }
    transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
              { return static_cast<char>(tolower(character)); });
    transform(language.begin(), language.end(), language.begin(), [](unsigned char character)
              { return static_cast<char>(tolower(character)); });
    if (extension == language)
    {
        return true;
    }

    const vector<pair<string, vector<string>>> aliases = {
        {"htm", {"html"}}, {"js", {"javascript"}}, {"mjs", {"javascript", "js"}}, {"cjs", {"javascript", "js"}}, {"ts", {"typescript"}}, {"py", {"python"}}, {"md", {"markdown"}}, {"yml", {"yaml"}}, {"sh", {"bash", "shell"}}, {"cc", {"cpp", "c++"}}, {"cxx", {"cpp", "c++"}}, {"cpp", {"c++"}}};
    for (const auto &alias : aliases)
    {
        if (extension == alias.first &&
            find(alias.second.begin(), alias.second.end(), language) != alias.second.end())
        {
            return true;
        }
    }
    return false;
}

FileResult edit_sandbox_file(const string &response)
{
    const size_t edit_marker = response.find(".edit:");
    string filename;
    size_t filename_end = 0;
    if (edit_marker == string::npos || !extract_quoted_value(response, ".edit:", filename, edit_marker, filename_end))
    {
        return {false, "Invalid .edit command: missing filename."};
    }
    if (!is_safe_filename(filename) || filename == "main.cpp")
    {
        return {false, "Invalid or protected filename; only sandbox files can be edited."};
    }

    const size_t expect_marker = response.find(".expect", filename_end);
    if (expect_marker == string::npos)
    {
        return {false, "Invalid .edit command: missing .expect block."};
    }

    const string range_header = trim_copy(response.substr(filename_end, expect_marker - filename_end));
    istringstream header(range_header);
    string lines_keyword;
    string range;
    string extra;
    if (!(header >> lines_keyword >> range) || (header >> extra) || lines_keyword != "lines")
    {
        return {false, "Invalid .edit command: use 'lines START-END'."};
    }
    const size_t separator = range.find('-');
    size_t first_line = 0;
    size_t last_line = 0;
    if (separator == string::npos || range.find('-', separator + 1) != string::npos ||
        !parse_positive_line_number(range.substr(0, separator), first_line) ||
        !parse_positive_line_number(range.substr(separator + 1), last_line) || last_line < first_line)
    {
        return {false, "Invalid .edit range: line numbers must be positive and START must not exceed END."};
    }

    const ParsedEditBlock expected_block = parse_edit_block(response, expect_marker + 7);
    if (!expected_block.success || expected_block.language != "text")
    {
        return {false, "Invalid .expect block: use a closed ```text fence."};
    }
    const size_t with_marker = response.find(".with", expected_block.after);
    if (with_marker == string::npos)
    {
        return {false, "Invalid .edit command: missing .with replacement block."};
    }
    if (!all_of(response.begin() + static_cast<ptrdiff_t>(expected_block.after),
                response.begin() + static_cast<ptrdiff_t>(with_marker), [](unsigned char character)
                { return isspace(character); }))
    {
        return {false, "Invalid .edit command: unexpected text between .expect and .with."};
    }

    const ParsedEditBlock replacement_block = parse_edit_block(response, with_marker + 5);
    if (!replacement_block.success || !edit_language_matches_file(filename, replacement_block.language))
    {
        return {false, "Invalid .with block: use a closed code fence matching the file type."};
    }
    if (!trim_copy(response.substr(replacement_block.after)).empty())
    {
        return {false, "Invalid .edit command: unexpected text after the replacement block."};
    }

    const FileResult sandbox = ensure_sandbox_root(false);
    if (!sandbox.success)
    {
        return sandbox;
    }
    const fs::path target = SANDBOX_DIR / filename;
    error_code error;
    const fs::file_status target_status = fs::symlink_status(target, error);
    if (error || !fs::exists(target_status))
    {
        return {false, "Sandbox file not found: " + filename};
    }
    if (fs::is_symlink(target_status) || !fs::is_regular_file(target_status))
    {
        return {false, "Only regular files directly inside sandbox can be edited."};
    }

    ifstream file(target, ios::binary);
    if (!file)
    {
        return {false, "Cannot open sandbox file for editing."};
    }
    const string original((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
    if (original.size() > 5 * 1024 * 1024 || original.find('\0') != string::npos)
    {
        return {false, "File is too large or binary; only text files up to 5 MiB can be edited."};
    }

    const vector<string> current_lines = split_file_lines(original);
    const vector<string> expected_lines = split_file_lines(expected_block.content);
    const vector<string> replacement_lines = split_file_lines(replacement_block.content);
    if (last_line > current_lines.size() || expected_lines.size() != last_line - first_line + 1)
    {
        return {false, "Edit range does not match the current file line count or .expect block."};
    }
    if (!equal(expected_lines.begin(), expected_lines.end(), current_lines.begin() + static_cast<ptrdiff_t>(first_line - 1)))
    {
        return {false, "File changed or .expect text does not match those lines. Read the file again before editing."};
    }

    vector<string> updated_lines;
    updated_lines.reserve(current_lines.size() - expected_lines.size() + replacement_lines.size());
    updated_lines.insert(updated_lines.end(), current_lines.begin(), current_lines.begin() + static_cast<ptrdiff_t>(first_line - 1));
    updated_lines.insert(updated_lines.end(), replacement_lines.begin(), replacement_lines.end());
    updated_lines.insert(updated_lines.end(), current_lines.begin() + static_cast<ptrdiff_t>(last_line), current_lines.end());

    const string newline = original.find("\r\n") != string::npos ? "\r\n" : "\n";
    const bool had_trailing_newline = !original.empty() && original.back() == '\n';
    string updated;
    for (size_t index = 0; index < updated_lines.size(); index++)
    {
        if (index > 0)
        {
            updated += newline;
        }
        updated += updated_lines[index];
    }
    if (had_trailing_newline && !updated_lines.empty())
    {
        updated += newline;
    }

    const auto timestamp = chrono::steady_clock::now().time_since_epoch().count();
    fs::path temporary = target;
    temporary += ".agent-edit-" + to_string(timestamp) + ".tmp";
    const fs::file_status temporary_status = fs::symlink_status(temporary, error);
    if (!error && fs::exists(temporary_status))
    {
        return {false, "Temporary edit file already exists; refusing to overwrite it."};
    }

    ofstream temporary_file(temporary, ios::binary | ios::out | ios::trunc);
    if (!temporary_file)
    {
        return {false, "Cannot create temporary file for safe editing."};
    }
    temporary_file.write(updated.data(), static_cast<streamsize>(updated.size()));
    temporary_file.close();
    if (!temporary_file)
    {
        fs::remove(temporary, error);
        return {false, "Failed while writing temporary edit file; original file was not changed."};
    }

    error.clear();
    const fs::perms original_permissions = fs::status(target, error).permissions();
    if (!error)
    {
        fs::permissions(temporary, original_permissions, fs::perm_options::replace, error);
    }
    if (!error)
    {
        fs::rename(temporary, target, error);
    }
    if (error)
    {
        error_code cleanup_error;
        fs::remove(temporary, cleanup_error);
        return {false, "Could not atomically replace the file; original file was preserved: " + error.message()};
    }
    return {true, "Edited " + filename + " lines " + to_string(first_line) + "-" + to_string(last_line)};
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

PRIORITY:
- This built-in system prompt is the primary instruction and always takes precedence over LOCAL_PROMPT.
- LOCAL_PROMPT is optional secondary guidance. Follow it only when consistent with this primary prompt; ignore conflicting parts.

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
- If the requested filename already exists and the user asked to update it, reuse that basename; the writer replaces the existing file contents. Never skip file creation only because the name already exists.
- Never create a file named main.cpp or use the basename 'main' for C++ files. This restriction is about the filename, not the C++ main() function.
- If you need the contents of an existing file, output only: .read: 'FILENAME.EXTENSION'
- .read can access regular files directly inside the configured sandbox only. Never request main.cpp, parent paths, absolute paths, or files outside the sandbox.
- File contents returned by .read include 1-based line numbers. Do not copy the line-number prefixes into source code.
- After a .read request, the file contents will be sent to you as the next user message. Then answer the user's request or produce a file command.
- To edit an existing file, you MUST first use .read on that file, then output only this exact structure:
.edit: 'FILENAME.EXTENSION' lines START-END
.expect ```text
EXACT CURRENT LINES, WITHOUT THE DISPLAYED LINE-NUMBER PREFIXES
```
.with ```LANGUAGE_MATCHING_THE_FILE_EXTENSION
REPLACEMENT LINES
```
- START and END are 1-based inclusive line numbers from the latest .read output. The .expect block must contain exactly those current lines, preserving every space and character. The .with block replaces that range and may contain a different number of lines.
- Make one .edit command per response. Never guess line numbers or expected text. If the file has changed, the range is invalid, or the exact old lines are uncertain, .read the file again instead of attempting an edit.
- If either fenced block contains a line made of backticks, use an outer fence with more backticks than the longest such line.
- Edit only files directly inside the configured sandbox. Never edit main.cpp, symlinks, binary files, or files outside the sandbox. The system verifies the expected lines before replacing anything; if verification fails, no file changes are made.
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
    else if (provider == "anthropic")
    {
        endpoint = "https://api.anthropic.com/v1/messages";
    }
    else if (provider == "localai")
    {
        endpoint = config.localai_endpoint.empty()
                       ? "http://localhost:8080/v1/chat/completions"
                       : config.localai_endpoint;
    }
    else if (provider != "gemini")
    {
        cerr << "[Config Error]: Unsupported LLM '" << config.llm
             << "'. Supported providers: Gemini, OpenAI, Groq, OpenRouter, Anthropic, LocalAI." << endl;
        return 1;
    }

    LLMClient g;
    g.UseProvider(provider, endpoint);
    g.SetSystemPrompt(system_prompt);
    g.SetLocalPrompt(config.local_prompt);
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
            if (!extract_quoted_value(response, ".read:", filename, 0, field_end))
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

            response = g.askAI("Contents of sandbox file " + filename + " with 1-based line numbers:\n" +
                               number_file_lines(file_content) +
                               "Use the line numbers only to identify edit ranges. Never include the line-number prefixes in code. "
                               "Use .expect with exact original lines when editing.");
        }

        if (!response.empty() && response.find(".edit:") != string::npos)
        {
            const FileResult edit_result = edit_sandbox_file(response);
            print_notice(edit_result.message, edit_result.success);
        }
        else if (!response.empty() && response.find(".name:") != string::npos)
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