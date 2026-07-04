#include "token_store.h"
#include <random>
#include <sstream>

namespace chat::server{

static std::string makeToken(uint64_t n){
    static const char hex[]="0123456789abcdef";
    std::string s="tok_";
    s.reserve(4+16);
    uint64_t x=n;
    for (int i=15;i>=0;--i){ s.push_back(hex[(x>>(i*4))&0xF]); }
    return s;
}

std::string TokenStore::issue(const std::string& username){
    std::lock_guard<std::mutex> lk(mu_);
    ++counter_;
    std::string tok = makeToken(counter_);
    tok2user_[tok] = username;
    return tok;
}

std::string TokenStore::lookup(const std::string& token) const{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tok2user_.find(token);
    return it == tok2user_.end() ? std::string() : it->second;
}

void TokenStore::revoke(const std::string& token){
    std::lock_guard<std::mutex> lk(mu_);
    tok2user_.erase(token);
}

}