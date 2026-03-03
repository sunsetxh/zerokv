// Skip List Index for Storage Engine
#pragma once

#include <memory>
#include <vector>
#include <random>
#include <string>

namespace zerokv {

template<typename Key, typename Value>
class SkipList {
public:
    struct Node {
        Key key;
        Value value;
        std::vector<Node*> forward;

        Node(const Key& k, const Value& v, int level)
            : key(k), value(v), forward(level + 1, nullptr) {}
    };

    SkipList(int max_level = 16) : max_level_(max_level), probability_(0.5f) {
        head_ = new Node(Key(), Value(), max_level_);
    }

    ~SkipList() {
        Node* current = head_;
        while (current) {
            Node* next = current->forward[0];
            delete current;
            current = next;
        }
    }

    void insert(const Key& key, const Value& value);
    bool search(const Key& key, Value* value);
    bool erase(const Key& key);

private:
    int random_level();
    Node* head_;
    int max_level_;
    float probability_;
    std::mt19937 rng_;
};

} // namespace zerokv
