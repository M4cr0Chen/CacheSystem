#include "LruCache.h"
#include <iostream>
#include <string>

int main()
{
    MyCache::LruCache<int, std::string> cache(128);
    cache.put(1, "one");

    std::string out;
    if (cache.get(1, out))
        std::cout << out;      // prints “one”
}
