#include <iostream>
#include <string>
#include <chrono>
#include <vector>
#include <iomanip>
#include <random>
#include <algorithm>

#include "CacheStrategy.h"
#include "LfuCache.h"
#include "LruCache.h"
#include "ArcCache/ArcCache.h"

//---------------------------------------------------------------------
//  Simple wall‑clock timer helper
//---------------------------------------------------------------------
class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}
    
    double elapsedMs() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count();
    }

private:
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

//---------------------------------------------------------------------
//  Utility: print the aggregated hit‑rate results for a single test case
//---------------------------------------------------------------------
void printResults(const std::string& testName,
                  int                 capacity,
                  const std::vector<int>& get_ops,
                  const std::vector<int>& hits)
{
    std::cout << "=== " << testName << " : summary ===\n";
    std::cout << "Cache capacity: " << capacity << "\n";

    // Names inferred from vector size (LRU, LFU, ARC always present)
    std::vector<std::string> names;
    if (hits.size() == 3)      names = {"LRU", "LFU", "ARC"};
    else if (hits.size() == 4) names = {"LRU", "LFU", "ARC", "LRU‑K"};
    else if (hits.size() == 5) names = {"LRU", "LFU", "ARC", "LRU‑K", "LFU‑Aging"};

    for (size_t i = 0; i < hits.size(); ++i) {
        double hitRate = 100.0 * hits[i] / get_ops[i];
        std::cout << (i < names.size() ? names[i] : "Algo" + std::to_string(i+1))
                  << " – hit‑rate: " << std::fixed << std::setprecision(2)
                  << hitRate << "% "
                  << "(" << hits[i] << "/" << get_ops[i] << ")\n";
    }
    std::cout << "\n";
}

//---------------------------------------------------------------------
//  Scenario 1: Hot‑spot workload – small set of hot keys mixed with a large
//              cold population.
//---------------------------------------------------------------------
void testHotDataAccess() {
    std::cout << "\n=== Scenario 1: hot‑data access ===\n";

    const int CAPACITY    = 20;      // cache size
    const int OPS         = 500'000; // total operations
    const int HOT_KEYS    = 20;      // number of hot keys
    const int COLD_KEYS   = 5'000;   // cold key universe

    // Instantiate caches
    MyCache::LruCache<int,std::string>   lru(CAPACITY);
    MyCache::LfuCache<int,std::string>   lfu(CAPACITY);
    MyCache::ArcCache<int,std::string>   arc(CAPACITY);
    // LRU‑K: promote after 2 hits, history capacity = hot + cold universe
    MyCache::LruKCache<int,std::string>  lruk(CAPACITY, HOT_KEYS + COLD_KEYS, 2);
    // LFU with ageing (maxAverageNum = 20 000) to prevent frequency explosion
    MyCache::LfuCache<int,std::string>   lfuAging(CAPACITY, 20'000);

    std::vector<MyCache::CacheStrategy<int,std::string>*> caches =
        {&lru, &lfu, &arc, &lruk, &lfuAging};

    std::vector<int> hits(5,0);
    std::vector<int> get_ops(5,0);

    std::random_device rd;
    std::mt19937       gen(rd());

    // Run identical operation stream on each cache
    for (size_t ci = 0; ci < caches.size(); ++ci) {
        // 1) warm‑up: insert all hot keys once
        for (int k = 0; k < HOT_KEYS; ++k)
            caches[ci]->put(k, "value" + std::to_string(k));

        // 2) mixed workload: 70 % reads, 30 % writes by design
        for (int op = 0; op < OPS; ++op) {
            bool isPut = (gen() % 100) < 30; // 30 % writes
            int  key   = (gen() % 100 < 70)
                            ? gen() % HOT_KEYS                       // 70 % goes to hot set
                            : HOT_KEYS + (gen() % COLD_KEYS);       // 30 % to cold universe

            if (isPut) {
                std::string val = "value" + std::to_string(key) + "_v" + std::to_string(op % 100);
                caches[ci]->put(key, val);
            } else {
                std::string out;
                ++get_ops[ci];
                if (caches[ci]->get(key, out)) ++hits[ci];
            }
        }
    }

    printResults("Hot‑data access", CAPACITY, get_ops, hits);
}

//---------------------------------------------------------------------
//  Scenario 2: Cyclic scan – sequential window with occasional jumps.
//---------------------------------------------------------------------
void testLoopPattern() {
    std::cout << "\n=== Scenario 2: cyclic scan ===\n";

    const int CAPACITY  = 50;      // cache size
    const int LOOP      = 500;     // size of the scanning window
    const int OPS       = 200'000; // total operations

    MyCache::LruCache<int,std::string>   lru(CAPACITY);
    MyCache::LfuCache<int,std::string>   lfu(CAPACITY);
    MyCache::ArcCache<int,std::string>   arc(CAPACITY);
    MyCache::LruKCache<int,std::string>  lruk(CAPACITY, LOOP*2, 2);
    MyCache::LfuCache<int,std::string>   lfuAging(CAPACITY, 3'000);

    std::vector<MyCache::CacheStrategy<int,std::string>*> caches =
        {&lru, &lfu, &arc, &lruk, &lfuAging};

    std::vector<int> hits(5,0), get_ops(5,0);

    std::random_device rd; std::mt19937 gen(rd());

    for (size_t ci = 0; ci < caches.size(); ++ci) {
        // Warm‑up: load first 20 % of LOOP into cache
        for (int k = 0; k < LOOP/5; ++k)
            caches[ci]->put(k, "loop" + std::to_string(k));

        int scanPos = 0; // pointer for sequential scan

        for (int op = 0; op < OPS; ++op) {
            bool isPut = (gen() % 100) < 20; // 20 % writes

            int key;
            if (op % 100 < 60) {        // 60 % sequential scan
                key = scanPos;
                scanPos = (scanPos + 1) % LOOP;
            } else if (op % 100 < 90) { // 30 % random within window
                key = gen() % LOOP;
            } else {                    // 10 % outside window
                key = LOOP + (gen() % LOOP);
            }

            if (isPut) {
                std::string val = "loop" + std::to_string(key) + "_v" + std::to_string(op % 100);
                caches[ci]->put(key, val);
            } else {
                std::string out;
                ++get_ops[ci];
                if (caches[ci]->get(key, out)) ++hits[ci];
            }
        }
    }

    printResults("Cyclic scan", CAPACITY, get_ops, hits);
}

//---------------------------------------------------------------------
//  Scenario 3: Work‑load phase shifts – five distinct phases with very
//              different access patterns.
//---------------------------------------------------------------------
void testWorkloadShift() {
    std::cout << "\n=== Scenario 3: workload shift ===\n";

    const int CAPACITY   = 30;       // cache size
    const int OPS        = 80'000;   // total operations
    const int PHASE_LEN  = OPS / 5;  // length per phase

    MyCache::LruCache<int,std::string>   lru(CAPACITY);
    MyCache::LfuCache<int,std::string>   lfu(CAPACITY);
    MyCache::ArcCache<int,std::string>   arc(CAPACITY);
    MyCache::LruKCache<int,std::string>  lruk(CAPACITY, 500, 2);
    MyCache::LfuCache<int,std::string>   lfuAging(CAPACITY, 10'000);

    std::vector<MyCache::CacheStrategy<int,std::string>*> caches =
        {&lru, &lfu, &arc, &lruk, &lfuAging};

    std::vector<int> hits(5,0), get_ops(5,0);
    std::random_device rd; std::mt19937 gen(rd());

    for (size_t ci = 0; ci < caches.size(); ++ci) {
        // light warm‑up
        for (int k = 0; k < 30; ++k)
            caches[ci]->put(k, "init" + std::to_string(k));

        for (int op = 0; op < OPS; ++op) {
            int phase = op / PHASE_LEN;

            // Write ratio per phase (tuned for realism)
            int putProb;
            switch (phase) {
                case 0: putProb = 15; break; // hot‑spot phase
                case 1: putProb = 30; break; // wide random phase
                case 2: putProb = 10; break; // sequential scan
                case 3: putProb = 25; break; // localised random
                case 4: putProb = 20; break; // blended
                default: putProb = 20;
            }
            bool isPut = (gen() % 100) < putProb;

            // Key generation strategy varies by phase
            int key;
            if (phase == 0) {
                key = gen() % 5; // 5‑key hot set
            } else if (phase == 1) {
                key = gen() % 400; // broad range
            } else if (phase == 2) {
                key = (op - PHASE_LEN*2) % 100; // sequential 0‑99
            } else if (phase == 3) {
                int region = (op / 800) % 5; // 5 local regions
                key = region*15 + (gen()%15);
            } else {
                int r = gen() % 100;
                if (r < 40)      key = gen() % 5;          // 40 % hot set
                else if (r < 70) key = 5 + (gen() % 45);   // 30 % mid range
                else             key = 50 + (gen() % 350); // 30 % wide range
            }

            if (isPut) {
                std::string val = "value" + std::to_string(key) + "_p" + std::to_string(phase);
                caches[ci]->put(key, val);
            } else {
                std::string out;
                ++get_ops[ci];
                if (caches[ci]->get(key, out)) ++hits[ci];
            }
        }
    }

    printResults("Workload shift", CAPACITY, get_ops, hits);
}

int main() {
    testHotDataAccess();
    testLoopPattern();
    testWorkloadShift();
    return 0;
}
