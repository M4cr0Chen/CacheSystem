#pragma once

#include "ArcCacheNode.h"
#include <unordered_map>
#include <map>
#include <mutex>

namespace MyCache 
{

template<typename Key, typename Value>
class ArcLfuCache 
{
public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr  = std::shared_ptr<NodeType>;
    using NodeMap  = std::unordered_map<Key, NodePtr>;
    using FreqMap  = std::map<size_t, std::list<NodePtr>>; // frequency → list of nodes

    /**
     * @param capacity           live LFU capacity (T2 side of ARC)
     * @param transformThreshold hits required before promotion (unused in LFU part)
     */
    explicit ArcLfuCache(size_t capacity, size_t transformThreshold)
        : capacity_(capacity)
        , ghostCapacity_(capacity)          // ghost list mirrors live capacity
        , transformThreshold_(transformThreshold)
        , minFreq_(0)
    {
        initializeLists();
    }

    // Insert or update --------------------------------------------------------
    bool put(Key key, Value value)
    {
        if (capacity_ == 0) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end())
            return updateExistingNode(it->second, value);
        return addNewNode(key, value);
    }

    // Lookup and update frequency --------------------------------------------
    bool get(Key key, Value& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end())
        {
            updateNodeFrequency(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    /** If key is present in the ghost list, remove it and return true. */
    bool checkGhost(Key key)
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end())
        {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    // ARC tuning helpers ------------------------------------------------------
    void  increaseCapacity() { ++capacity_; }
    bool  decreaseCapacity()
    {
        if (capacity_ == 0) return false;
        if (mainCache_.size() == capacity_) evictLeastFrequent();
        --capacity_;
        return true;
    }

private:
    // Initialise ghost list with dummy head/tail
    void initializeLists()
    {
        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    // Update an existing key
    bool updateExistingNode(NodePtr node, const Value& value)
    {
        node->setValue(value);
        updateNodeFrequency(node);
        return true;
    }

    // Insert fresh key; evict if cache full
    bool addNewNode(const Key& key, const Value& value)
    {
        if (mainCache_.size() >= capacity_)
            evictLeastFrequent();

        NodePtr node = std::make_shared<NodeType>(key, value);
        mainCache_[key] = node;

        // Insert into frequency‑1 bucket
        freqMap_[1].push_back(node);
        minFreq_ = 1;
        return true;
    }

    // Promote node to higher frequency bucket
    void updateNodeFrequency(NodePtr node)
    {
        size_t oldFreq = node->getAccessCount();
        node->incrementAccessCount();
        size_t newFreq = node->getAccessCount();

        // Remove from old bucket
        auto& oldList = freqMap_[oldFreq];
        oldList.remove(node);
        if (oldList.empty()) {
            freqMap_.erase(oldFreq);
            if (oldFreq == minFreq_) minFreq_ = newFreq;
        }

        // Add to new bucket
        freqMap_[newFreq].push_back(node);
    }

    // Evict LFU node and move it to ghost history
    void evictLeastFrequent()
    {
        if (freqMap_.empty()) return;
        auto& bucket = freqMap_[minFreq_];
        if (bucket.empty()) return;

        NodePtr victim = bucket.front();
        bucket.pop_front();
        if (bucket.empty()) {
            freqMap_.erase(minFreq_);
            if (!freqMap_.empty()) minFreq_ = freqMap_.begin()->first;
        }

        // History management
        if (ghostCache_.size() >= ghostCapacity_) removeOldestGhost();
        addToGhost(victim);
        mainCache_.erase(victim->getKey());
    }

    // Ghost‑list helpers ------------------------------------------------------
    void removeFromGhost(NodePtr node)
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_        = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_        = nullptr; // break forward link
        }
    }

    void addToGhost(NodePtr node)
    {
        node->next_ = ghostTail_;
        node->prev_ = ghostTail_->prev_;
        if (!ghostTail_->prev_.expired()) ghostTail_->prev_.lock()->next_ = node;
        ghostTail_->prev_ = node;
        ghostCache_[node->getKey()] = node;
    }

    void removeOldestGhost()
    {
        NodePtr oldest = ghostHead_->next_;
        if (oldest != ghostTail_) {
            removeFromGhost(oldest);
            ghostCache_.erase(oldest->getKey());
        }
    }

private:
    size_t capacity_;               // live LFU capacity
    size_t ghostCapacity_;          // history capacity
    size_t transformThreshold_;     // hits before promotion (unused here)
    size_t minFreq_;                // current minimum frequency in live cache
    std::mutex mutex_;

    NodeMap mainCache_;   // live entries
    NodeMap ghostCache_;  // history entries
    FreqMap freqMap_;     // frequency buckets

    // Ghost list sentinels
    NodePtr ghostHead_;
    NodePtr ghostTail_;
};

} // namespace MyCache
