#pragma once

#include <windows.h>

#include <filesystem>
#include <map>
#include <string>

namespace gmod::ui {

std::map<std::string, std::string> parse_update_status(const std::string& text);

class Updates {
public:
    explicit Updates(std::filesystem::path root);
    ~Updates();
    Updates(const Updates&) = delete;
    Updates& operator=(const Updates&) = delete;

    void start(const std::string& operation);
    void cancel();
    void fail(const std::string& message);
    bool poll();
    bool busy() const { return process_ != nullptr; }
    std::string value(const std::string& key) const;

private:
    std::filesystem::path root_;
    std::filesystem::path status_path_;
    HANDLE process_ = nullptr;
    std::string last_status_;
    std::map<std::string, std::string> status_;
};

} // namespace gmod::ui
