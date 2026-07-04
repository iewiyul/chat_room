#include "../../src/server/room.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>

using namespace chat::server;

static Message mk(int64_t t, const std::string& from="u", const std::string& c="x"){
    return Message{from, "lobby", c, t};
}

int main(){
    // 用例 1: messages 增/查 + since 过滤
    {
        Room r;
        r.addMessage(mk(100, "alice", "hi"));
        r.addMessage(mk(200, "bob",   "yo"));
        r.addMessage(mk(300, "alice", "sup"));

        assert(r.messageCount() == 3);
        auto all = r.recentMessages(0);
        assert(all.size() == 3);
        assert(all[0].from == "alice" && all[0].content == "hi");
        assert(all[2].from == "alice" && all[2].content == "sup");

        // since=150 拿后两条
        auto after = r.recentMessages(150);
        assert(after.size() == 2);
        assert(after[0].from == "bob");
        assert(after[1].from == "alice");
    }

    // 用例 2: 上限（MAX_HISTORY=200）
    {
        Room r;
        for (int i = 0; i < 250; ++i) r.addMessage(mk(i));
        assert(r.messageCount() == 200);
        auto all = r.recentMessages(0);
        assert(all.size() == 200);
        // 最老的是 i=50（前面 50 条 pop 掉了）
        assert(all[0].time == 50);
        assert(all.back().time == 249);
    }

    // 用例 3: 用户增删查
    {
        Room r;
        assert(!r.hasUser("alice"));
        r.addUser("alice");
        r.addUser("bob");
        assert(r.hasUser("alice"));
        assert(r.hasUser("bob"));
        auto us = r.users();
        assert(us.size() == 2);

        r.removeUser("alice");
        assert(!r.hasUser("alice"));
        assert(r.hasUser("bob"));
    }

    // 用例 4: 同名 addUser 不报错（unordered_set 自然去重）
    {
        Room r;
        r.addUser("a");
        r.addUser("a");
        assert(r.users().size() == 1);
    }

    // 用例 5: 多线程读写不爆（10 个线程并发 add 100 条消息 + add/removeUser）
    {
        Room r;
        std::vector<std::thread> ths;
        std::atomic<int> ready{0};
        for (int t = 0; t < 10; ++t){
            ths.emplace_back([&]{
                ready++;
                while (ready.load() < 10) {}     // 同时起跑
                for (int i = 0; i < 100; ++i){
                    r.addMessage(mk(t * 1000 + i, "u"+std::to_string(t)));
                    if (i % 3 == 0) r.addUser("u"+std::to_string(t));
                    if (i % 5 == 0) r.removeUser("u"+std::to_string(t));
                }
            });
        }
        for (auto& th : ths) th.join();
        // 1000 条入队，被 cap 到 200
        assert(r.messageCount() == 200);
        // users 不一定 = 10，因为 add/remove 竞争，但至少 0
        assert(r.users().size() <= 10);
    }

    std::cout << "test_room: ALL PASS\n";
    return 0;
}
