#pragma once

#include "ArcCacheNode.h"
#include <unordered_map>
#include <mutex>

namespace MyCache 
{

template<typename Key, typename Value>
class ArcLruCache 
{
public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr  = std::shared_ptr<NodeType>;
    using NodeMap  = std::unordered_map<Key, NodePtr>;

    /**
     *  capacity            maximum number of live entries allowed in this real cache list
     *  transformThreshold  number of hits after which an item should be promoted to the other ARC list
     */
    explicit ArcLruCache(size_t capacity, size_t transformThreshold)
        : capacity_(capacity)
        , ghostCapacity_(capacity)          // ghost list is the same size as the real list
        , transformThreshold_(transformThreshold)
    {
        initializeLists();
    }

    /*------------------------------------------------------------------
     * put – insert or update (returns true on success)
     *----------------------------------------------------------------*/
    bool put(Key key, Value value) 
    {
        if (capacity_ == 0) return false;
        
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            return updateExistingNode(it->second, value);
        }
        return addNewNode(key, value);
    }

    /*------------------------------------------------------------------
     * get – fetch value if present; shouldTransform tells ARC whether the
     *       entry exceeded its promotion threshold.
     *----------------------------------------------------------------*/
    bool get(Key key, Value& value, bool& shouldTransform) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            shouldTransform = updateNodeAccess(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    /**
     * Check if a key lives in the ghost list (LRU history). If so remove it
     * from the ghost list and signal the caller.
     */
    bool checkGhost(Key key) 
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    // ARC tuning utilities ----------------------------------------------------
    void  increaseCapacity() { ++capacity_; }
    bool  decreaseCapacity() 
    {
        if (capacity_ <= 0) return false;
        if (mainCache_.size() == capacity_) {
            evictLeastRecent();           // make room first
        }
        --capacity_;
        return true;
    }

private:
    // Create dummy head/tail for both real and ghost lists
    void initializeLists() 
    {
        mainHead_ = std::make_shared<NodeType>();
        mainTail_ = std::make_shared<NodeType>();
        mainHead_->next_ = mainTail_;
        mainTail_->prev_ = mainHead_;

        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    // Update an existing record and move it to MRU position
    bool updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        moveToFront(node);
        return true;
    }

    // Insert brand‑new key. Evict if the real list is full.
    bool addNewNode(const Key& key, const Value& value) 
    {
        if (mainCache_.size() >= capacity_) 
        {   
            evictLeastRecent();           // remove LRU before inserting
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value);
        mainCache_[key] = newNode;
        addToFront(newNode);
        return true;
    }

    // Mark an access: move to front, ++hit counter, signal promotion if needed
    bool updateNodeAccess(NodePtr node) 
    {
        moveToFront(node);
        node->incrementAccessCount();
        return node->getAccessCount() >= transformThreshold_;
    }

    // Detach node (if already linked) and re‑insert right after head
    void moveToFront(NodePtr node) 
    {
        // detach from current position
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_           = node->next_;
            node->next_->prev_    = node->prev_;
            node->next_           = nullptr;   // break original forward link
        }
        addToFront(node);
    }

    // Insert directly behind the dummy head (MRU end)
    void addToFront(NodePtr node) 
    {
        node->next_           = mainHead_->next_;
        node->prev_           = mainHead_;
        mainHead_->next_->prev_ = node;
        mainHead_->next_        = node;
    }

    // Evict LRU of real list and move it to ghost history
    void evictLeastRecent() 
    {
        NodePtr leastRecent = mainTail_->prev_.lock();
        if (!leastRecent || leastRecent == mainHead_) return;

        removeFromMain(leastRecent);

        // add to ghost list; drop oldest ghost if ghost list is full
        if (ghostCache_.size() >= ghostCapacity_) {
            removeOldestGhost();
        }
        addToGhost(leastRecent);

        mainCache_.erase(leastRecent->getKey());
    }

    // Remove node from the real list (no map update)
    void removeFromMain(NodePtr node) 
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_        = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_        = nullptr; // clear forward link to prevent dangling
        }
    }

    // Remove node from ghost list
    void removeFromGhost(NodePtr node) 
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_        = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_        = nullptr; // clear forward link to prevent dangling
        }
    }

    // Insert an evicted node into ghost list (resetting its counters)
    void addToGhost(NodePtr node) 
    {
        node->accessCount_ = 1; // reset hit counter for history
        
        node->next_           = ghostHead_->next_;
        node->prev_           = ghostHead_;
        ghostHead_->next_->prev_ = node;
        ghostHead_->next_        = node;
        
        ghostCache_[node->getKey()] = node;
    }

    // Remove the least‑recent entry of ghost list (LRU of history)
    void removeOldestGhost() 
    {
        NodePtr oldestGhost = ghostTail_->prev_.lock();
        if (!oldestGhost || oldestGhost == ghostHead_) return;

        removeFromGhost(oldestGhost);
        ghostCache_.erase(oldestGhost->getKey());
    }
    

private:
    size_t capacity_;                // live cache capacity
    size_t ghostCapacity_;           // history list capacity (mirrors capacity_)
    size_t transformThreshold_;      // hits required before promotion to T1/T2 swap
    std::mutex mutex_;

    NodeMap mainCache_;   // key → live node
    NodeMap ghostCache_;  // key → historical (ghost) node
    
    // Real cache doubly‑linked list (dummy head/tail)
    NodePtr mainHead_;
    NodePtr mainTail_;
    // Ghost (history) list (dummy head/tail)
    NodePtr ghostHead_;
    NodePtr ghostTail_;
};

} // namespace MyCache
