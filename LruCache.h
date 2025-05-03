#pragma once 

#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <thread>
#include <cmath>

#include "CacheStrategy.h"

namespace MyCache
{

// Forward declaration
template<typename Key, typename Value> class LruCache;

template<typename Key, typename Value>
class LruNode 
{
private:
    Key key_;
    Value value_;
    size_t accessCount_;  // Number of accesses
    std::weak_ptr<LruNode<Key, Value>> prev_;  // Use weak_ptr to break cyclic reference
    std::shared_ptr<LruNode<Key, Value>> next_;

public:
    LruNode(Key key, Value value)
        : key_(key)
        , value_(value)
        , accessCount_(1) 
    {}

    // Basic accessors
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    void setValue(const Value& value) { value_ = value; }
    size_t getAccessCount() const { return accessCount_; }
    void incrementAccessCount() { ++accessCount_; }

    friend class LruCache<Key, Value>;
};


template<typename Key, typename Value>
class LruCache : public CacheStrategy<Key, Value>
{
public:
    using LruNodeType = LruNode<Key, Value>;
    using NodePtr = std::shared_ptr<LruNodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    LruCache(int capacity)
        : capacity_(capacity)
    {
        initializeList();
    }

    ~LruCache() override = default;

    // Insert or update a cache entry
    void put(Key key, Value value) override
    {
        if (capacity_ <= 0)
            return;
    
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // Key exists: update value and mark as recently accessed
            updateExistingNode(it->second, value);
            return ;
        }

        addNewNode(key, value);
    }

    bool get(Key key, Value& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            moveToMostRecent(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    Value get(Key key) override
    {
        Value value{};
        // memset operates byte‑wise; using it on complex types (e.g. std::string) may corrupt the object
        get(key, value);
        return value;
    }

    // Remove a specific element
    void remove(Key key) 
    {   
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            removeNode(it->second);
            nodeMap_.erase(it);
        }
    }

private:
    void initializeList()
    {
        // Create sentinel head and tail nodes
        dummyHead_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyTail_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyHead_->next_ = dummyTail_;
        dummyTail_->prev_ = dummyHead_;
    }

    void updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        moveToMostRecent(node);
    }

    void addNewNode(const Key& key, const Value& value) 
    {
       if (nodeMap_.size() >= capacity_) 
       {
           evictLeastRecent();
       }

       NodePtr newNode = std::make_shared<LruNodeType>(key, value);
       insertNode(newNode);
       nodeMap_[key] = newNode;
    }

    // Move the node to the MRU position
    void moveToMostRecent(NodePtr node) 
    {
        removeNode(node);
        insertNode(node);
    }

    void removeNode(NodePtr node) 
    {
        if(!node->prev_.expired() && node->next_) 
        {
            auto prev = node->prev_.lock(); // Convert weak_ptr to shared_ptr
            prev->next_ = node->next_;
            node->next_->prev_ = prev;
            node->next_ = nullptr; // Sever link completely
        }
    }

    // Insert node just before the tail (MRU end)
    void insertNode(NodePtr node) 
    {
        node->next_ = dummyTail_;
        node->prev_ = dummyTail_->prev_;
        dummyTail_->prev_.lock()->next_ = node; // Convert weak_ptr to shared_ptr
        dummyTail_->prev_ = node;
    }

    // Evict the least recently used node
    void evictLeastRecent() 
    {
        NodePtr leastRecent = dummyHead_->next_;
        removeNode(leastRecent);
        nodeMap_.erase(leastRecent->getKey());
    }

private:
    int           capacity_; // Cache capacity
    NodeMap       nodeMap_;  // Maps key to node
    std::mutex    mutex_;
    NodePtr       dummyHead_; // Sentinel head
    NodePtr       dummyTail_; // Sentinel tail
};

// LRU optimization: LRU‑k variant for better filtering of ephemeral entries
template<typename Key, typename Value>
class KLruKCache : public LruCache<Key, Value>
{
public:
    KLruKCache(int capacity, int historyCapacity, int k)
        : LruCache<Key, Value>(capacity) // Call base constructor
        , historyList_(std::make_unique<LruCache<Key, size_t>>(historyCapacity))
        , k_(k)
    {}

    Value get(Key key) 
    {
        // First try to fetch from the main cache
        Value value{};
        bool inMainCache = LruCache<Key, Value>::get(key, value);

        // Retrieve and update access history
        size_t historyCount = historyList_->get(key);
        historyCount++;
        historyList_->put(key, historyCount);

        // If present in the main cache, return it
        if (inMainCache) 
        {
            return value;
        }

        // Promote to main cache once the entry has been accessed k times
        if (historyCount >= k_) 
        {
            // Check whether we stored a value earlier
            auto it = historyValueMap_.find(key);
            if (it != historyValueMap_.end()) 
            {
                Value storedValue = it->second;
                
                // Cleanup history
                historyList_->remove(key);
                historyValueMap_.erase(it);
                
                // Insert into main cache
                LruCache<Key, Value>::put(key, storedValue);
                
                return storedValue;
            }
            // No stored value available – fall through to return default
        }

        // Not found or not yet eligible for promotion
        return value;
    }

    void put(Key key, Value value) 
    {
        // Check if entry is already in the main cache
        Value existingValue{};
        bool inMainCache = LruCache<Key, Value>::get(key, existingValue);
        
        if (inMainCache) 
        {
            // Update existing record
            LruCache<Key, Value>::put(key, value);
            return;
        }
        
        // Update access history
        size_t historyCount = historyList_->get(key);
        historyCount++;
        historyList_->put(key, historyCount);
        
        // Keep the value for potential promotion
        historyValueMap_[key] = value;
        
        // Promote once the access threshold is reached
        if (historyCount >= k_) 
        {
            historyList_->remove(key);
            historyValueMap_.erase(key);
            LruCache<Key, Value>::put(key, value);
        }
    }

private:
    int                                     k_; // Access threshold to enter main cache
    std::unique_ptr<LruCache<Key, size_t>> historyList_; // Access history (value = access count)
    std::unordered_map<Key, Value>          historyValueMap_; // Values not yet promoted
};

// LRU optimization: sharding to improve performance under high concurrency
template<typename Key, typename Value>
class KHashLruCaches
{
public:
    KHashLruCaches(size_t capacity, int sliceNum)
        : capacity_(capacity)
        , sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
    {
        size_t sliceSize = std::ceil(capacity / static_cast<double>(sliceNum_)); // Capacity per shard
        for (int i = 0; i < sliceNum_; ++i)
        {
            lruSliceCaches_.emplace_back(new LruCache<Key, Value>(sliceSize)); 
        }
    }

    void put(Key key, Value value)
    {
        // Hash the key to find its shard
        size_t sliceIndex = Hash(key) % sliceNum_;
        lruSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(Key key, Value& value)
    {
        // Hash the key to find its shard
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lruSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(Key key)
    {
        Value value;
        memset(&value, 0, sizeof(value));
        get(key, value);
        return value;
    }

private:
    // Hash function for the key
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t                                              capacity_;  // Total capacity
    int                                                 sliceNum_;  // Number of shards
    std::vector<std::unique_ptr<LruCache<Key, Value>>> lruSliceCaches_; // Sharded LRU caches
};

} // namespace MyCache
