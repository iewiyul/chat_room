#pragma once

#include <string>
#include <unordered_map>
#include <mutex>

namespace chat::server{

class TokenStore{
public:
    // 返回 token
    std::string issue(const std::string& username);
    // 返回 "" 表示无效
    std::string lookup(const std::string& token) const;
    void revoke(const std::string& token);

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> tok2user_;
    // token 自增 id
    uint64_t counter_ = 0;
};

}