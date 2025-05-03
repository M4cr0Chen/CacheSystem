#pragma once

namespace MyCache
{

template <typename Key, typename Value>
class CacheStrategy
{
public:
    virtual ~CacheStrategy() {};

    // add cache interface
    virtual void put(Key key, Value value) = 0;

    // key is the input parameter, the accessed value is returned in the output parameter | returns true if access is successful
    virtual bool get(Key key, Value& value) = 0;
    // if key found in cache, return the value
    virtual Value get(Key key) = 0;

};

} // namespace MyCache