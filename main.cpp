#include "LruCache.h"
#include "LfuCache.h"
#include <iostream>
#include <string>

int main()
{
    MyCache::LruCache<int, std::string> cache(128);
    MyCache::LfuCache<int, std::string> lfuCache(128, 10);
    cache.put(1, "one");
    lfuCache.put(1, "one");

    std::string out;
    if (cache.get(1, out))
        std::cout << out;      // prints “one”

    std::string out2;
    if (lfuCache.get(1, out2))
        std::cout << out2;     // prints “one”
}
