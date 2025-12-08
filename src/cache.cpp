// LRU cache implementation
#include "cache.h"
#include <fstream>
#include <random>

using namespace std;

// Constructor definition
Cache::Cache(CacheConfig configParam, CacheDataType cacheType)
    : hits(0),
      misses(0),
      type(cacheType),
      config(configParam) {
    // Derive geometry from configuration
    computeGeometry();
    // Initialize set structures
    sets.resize(numberOfSets);
    for (auto& set : sets) {
        set.resize(config.ways);
        for (auto& line : set) {
            line.isValid = false;
            line.tag = 0;
            line.lruTimestamp = 0;
        }
    }
}

// Access method definition
bool Cache::access(uint64_t address, CacheOperation readWrite) {
    (void)readWrite; // write-through; timing identical for read/write

    auto indexAndTag = getIndexAndTag(address);
    uint64_t setIndex = indexAndTag.first;
    uint64_t tag = indexAndTag.second;
    auto& set = sets[setIndex];

    // 1) Probe for a hit
    int hitLineIndex = -1;
    for (int way = 0; way < static_cast<int>(config.ways); ++way) {
        if (set[way].isValid && set[way].tag == tag) {
            hitLineIndex = way;
            break;
        }
    }

    if (hitLineIndex >= 0) {
        hits++;
        set[hitLineIndex].lruTimestamp = ++lruClock;
        return true;
    }

    // 2) Miss path: write-allocate
    misses++;

    // Choose victim: first invalid, else true LRU
    int victimIndex = -1;
    for (int way = 0; way < static_cast<int>(config.ways); ++way) {
        if (!set[way].isValid) {
            victimIndex = way;
            break;
        }
    }
    if (victimIndex < 0) {
        uint64_t minTimestamp = set[0].lruTimestamp;
        victimIndex = 0;
        for (int way = 1; way < static_cast<int>(config.ways); ++way) {
            if (set[way].lruTimestamp < minTimestamp) {
                minTimestamp = set[way].lruTimestamp;
                victimIndex = way;
            }
        }
    }

    // Fill
    set[victimIndex].isValid = true;
    set[victimIndex].tag = tag;
    set[victimIndex].lruTimestamp = ++lruClock;
    return false;
}

// debug: dump information as you needed, here are some examples
Status Cache::dump(const std::string& base_output_name) {
    ofstream cache_out(base_output_name + "_cache_state.out");
    if (cache_out) {
        cache_out << "---------------------" << endl;
        cache_out << "Begin Register Values" << endl;
        cache_out << "---------------------" << endl;
        cache_out << "Cache Configuration:" << std::endl;
        cache_out << "Size: " << config.cacheSize << " bytes" << std::endl;
        cache_out << "Block Size: " << config.blockSize << " bytes" << std::endl;
        cache_out << "Ways: " << config.ways << std::endl;
        cache_out << "Miss Latency: " << config.missLatency << " cycles" << std::endl;
        cache_out << "Derived Geometry:" << std::endl;
        cache_out << "Sets: " << numberOfSets << std::endl;
        cache_out << "Block Offset Bits: " << blockOffsetBits << std::endl;
        cache_out << "Set Index Bits: " << setIndexBits << std::endl;
        cache_out << "Statistics:" << std::endl;
        cache_out << "Hits: " << hits << std::endl;
        cache_out << "Misses: " << misses << std::endl;
        cache_out << "---------------------" << endl;
        cache_out << "End Register Values" << endl;
        cache_out << "---------------------" << endl;
        return SUCCESS;
    } else {
        cerr << LOG_ERROR << "Could not create cache state dump file" << endl;
        return ERROR;
    }
}
