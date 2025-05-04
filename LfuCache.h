#pragma once

#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "CacheStrategy.h"

namespace MyCache
{

template<typename Key, typename Value> class LfuCache;

template<typename Key, typename Value>
class FreqList
{
private:
    struct Node
    {
        int freq;                       // access frequency
        Key key;
        Value value;
        std::weak_ptr<Node> pre;        // previous node (weak_ptr to break cycles)
        std::shared_ptr<Node> next;

        Node() : freq(1), next(nullptr) {}
        Node(Key key, Value value) : freq(1), key(key), value(value), next(nullptr) {}
    };

    using NodePtr = std::shared_ptr<Node>;
    int     freq_;                      // frequency represented by this list
    NodePtr head_;                      // dummy head
    NodePtr tail_;                      // dummy tail

public:
    explicit FreqList(int n) : freq_(n)
    {
        head_ = std::make_shared<Node>();
        tail_ = std::make_shared<Node>();
        head_->next = tail_;
        tail_->pre  = head_;
    }

    bool isEmpty() const { return head_->next == tail_; }

    // basic node‑management helpers
    void addNode(NodePtr node)
    {
        if (!node || !head_ || !tail_) return;

        node->pre  = tail_->pre;
        node->next = tail_;
        tail_->pre.lock()->next = node; // convert weak_ptr to shared_ptr
        tail_->pre = node;
    }

    void removeNode(NodePtr node)
    {
        if (!node || !head_ || !tail_) return;
        if (node->pre.expired() || !node->next) return;

        auto pre = node->pre.lock();    // convert weak_ptr to shared_ptr
        pre->next         = node->next;
        node->next->pre   = pre;
        node->next        = nullptr;    // explicitly sever forward link
    }

    NodePtr getFirstNode() const { return head_->next; }

    friend class LfuCache<Key, Value>;
};

//---------------------------------------------------------------------
//  LFU cache with optional average‑frequency aging                            
//---------------------------------------------------------------------

template <typename Key, typename Value>
class LfuCache : public CacheStrategy<Key, Value>
{
public:
    using Node    = typename FreqList<Key, Value>::Node;
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    LfuCache(int capacity, int maxAverageNum = 10)
        : capacity_(capacity)
        , minFreq_(INT8_MAX)
        , maxAverageNum_(maxAverageNum)
        , curAverageNum_(0)
        , curTotalNum_(0) {}

    ~LfuCache() override = default;

    //-----------------------------------------------------------------
    // put(key,value) – insert or update
    //-----------------------------------------------------------------
    void put(Key key, Value value) override
    {
        if (capacity_ == 0) return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // key exists: update value then treat as a hit
            it->second->value = value;
            getInternal(it->second, value);
            return;
        }

        putInternal(key, value);
    }

    //-----------------------------------------------------------------
    // get(key, value) – returns true on hit and copies value out
    //-----------------------------------------------------------------
    bool get(Key key, Value& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            getInternal(it->second, value);
            return true;
        }
        return false;
    }

    Value get(Key key) override
    {
        Value value{};
        get(key, value);
        return value;
    }

    // Completely clear the cache
    void purge()
    {
        nodeMap_.clear();
        freqToFreqList_.clear();
    }

private:
    //--------------------------------------------------------------
    // internal helpers
    //--------------------------------------------------------------
    void putInternal(Key key, Value value);          // insert new key
    void getInternal(NodePtr node, Value& value);    // promote on hit

    void kickOut();                                  // evict LFU node

    void removeFromFreqList(NodePtr node);           // unlink from its list
    void addToFreqList(NodePtr node);                // link into list for node->freq

    void addFreqNum();                               // ++ total / recompute average
    void decreaseFreqNum(int num);                   // -- total / recompute average
    void handleOverMaxAverageNum();                  // global ageing when avg too high
    void updateMinFreq();                            // recompute minFreq_

private:
    int capacity_;    // cache capacity
    int minFreq_;     // current minimum frequency among items
    int maxAverageNum_; // threshold for average frequency ageing
    int curAverageNum_; // current average frequency
    int curTotalNum_;   // cumulative hits across all keys

    std::mutex mutex_;
    NodeMap nodeMap_; // key → node

    // freq → linked list of items with that frequency
    std::unordered_map<int, FreqList<Key, Value>*> freqToFreqList_;
};

//------------------------------------------------------------------
//  Implementation details
//------------------------------------------------------------------

template<typename Key, typename Value>
void LfuCache<Key, Value>::getInternal(NodePtr node, Value& value)
{
    // Hit: remove from current freq‑list, increment freq, add to new list
    value = node->value;
    removeFromFreqList(node);
    node->freq++;
    addToFreqList(node);

    // If the old list (freq‑1) became empty and it was the min freq, bump minFreq_
    if (node->freq - 1 == minFreq_ && freqToFreqList_[node->freq - 1]->isEmpty())
        minFreq_++;

    // update global counters
    addFreqNum();
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::putInternal(Key key, Value value)
{
    // Evict if full
    if (nodeMap_.size() == capacity_)
        kickOut();

    // Insert new node with freq = 1
    NodePtr node = std::make_shared<Node>(key, value);
    nodeMap_[key] = node;
    addToFreqList(node);
    addFreqNum();
    minFreq_ = std::min(minFreq_, 1);
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::kickOut()
{
    // Remove first node from the list that holds minFreq_
    NodePtr node = freqToFreqList_[minFreq_]->getFirstNode();
    removeFromFreqList(node);
    nodeMap_.erase(node->key);
    decreaseFreqNum(node->freq);
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::removeFromFreqList(NodePtr node)
{
    if (!node) return;
    freqToFreqList_[node->freq]->removeNode(node);
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::addToFreqList(NodePtr node)
{
    if (!node) return;

    if (freqToFreqList_.find(node->freq) == freqToFreqList_.end())
        freqToFreqList_[node->freq] = new FreqList<Key, Value>(node->freq);

    freqToFreqList_[node->freq]->addNode(node);
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::addFreqNum()
{
    curTotalNum_++;
    curAverageNum_ = nodeMap_.empty() ? 0 : curTotalNum_ / static_cast<int>(nodeMap_.size());
    if (curAverageNum_ > maxAverageNum_)
        handleOverMaxAverageNum();
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::decreaseFreqNum(int num)
{
    curTotalNum_ -= num;
    curAverageNum_ = nodeMap_.empty() ? 0 : curTotalNum_ / static_cast<int>(nodeMap_.size());
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::handleOverMaxAverageNum()
{
    if (nodeMap_.empty()) return;

    // Age all nodes: subtract maxAverageNum_/2 from every frequency
    for (auto& pair : nodeMap_)
    {
        NodePtr node = pair.second;
        if (!node) continue;

        removeFromFreqList(node);
        node->freq -= maxAverageNum_ / 2;
        if (node->freq < 1) node->freq = 1;
        addToFreqList(node);
    }

    updateMinFreq();
}

template<typename Key, typename Value>
void LfuCache<Key, Value>::updateMinFreq()
{
    minFreq_ = INT8_MAX;
    for (const auto& pair : freqToFreqList_)
        if (pair.second && !pair.second->isEmpty())
            minFreq_ = std::min(minFreq_, pair.first);
    if (minFreq_ == INT8_MAX) minFreq_ = 1;
}

//------------------------------------------------------------------
//  Sharded LFU wrapper
//------------------------------------------------------------------
//  This implementation keeps the overall capacity the same; it simply
//  partitions it across multiple independent LFU shards to reduce lock
//  contention under heavy concurrency.
//------------------------------------------------------------------

template<typename Key, typename Value>
class HashLfuCache
{
public:
    HashLfuCache(size_t capacity, int sliceNum, int maxAverageNum = 10)
        : sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
        , capacity_(capacity)
    {
        size_t sliceSize = std::ceil(capacity_ / static_cast<double>(sliceNum_)); // capacity per shard
        for (int i = 0; i < sliceNum_; ++i)
            lfuSliceCaches_.emplace_back(new LfuCache<Key, Value>(sliceSize, maxAverageNum));
    }

    void put(Key key, Value value)
    {
        size_t sliceIndex = Hash(key) % sliceNum_;
        lfuSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(Key key, Value& value)
    {
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lfuSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(Key key)
    {
        Value value{};
        get(key, value);
        return value;
    }

    void purge()
    {
        for (auto& lfu : lfuSliceCaches_)
            lfu->purge();
    }

private:
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t capacity_;  // total capacity across all shards
    int    sliceNum_;  // number of shards
    std::vector<std::unique_ptr<LfuCache<Key, Value>>> lfuSliceCaches_; // shard container
};

} // namespace MyCache
