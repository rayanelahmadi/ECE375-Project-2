#pragma once
#include <inttypes.h>
#include <iostream>
#include <vector>
#include "Utilities.h"

struct CacheConfig {
    // Cache size in bytes.
    uint64_t cacheSize;
    // Cache block size in bytes.
    uint64_t blockSize;
    // Type of cache: set associativity
    uint64_t ways;
    // Additional miss latency in cycles.
    uint64_t missLatency;
    // debug: Overload << operator to allow easy printing of CacheConfig
    friend std::ostream& operator<<(std::ostream& os, const CacheConfig& config) {
        os << "CacheConfig { " << config.cacheSize << ", " << config.blockSize << ", "
           << config.ways << ", " << config.missLatency << " }";
        return os;
    }
};

enum CacheDataType { I_CACHE = 0, D_CACHE = 1 };
enum CacheOperation { CACHE_READ = 0, CACHE_WRITE = 1 };

class Cache {
private:
    uint64_t hits, misses;
    CacheDataType type;
    // Derived geometry
    uint64_t numberOfSets;
    uint64_t blockOffsetBits;
    uint64_t setIndexBits;

    struct CacheLine {
        bool isValid;
        uint64_t tag;
        uint64_t lruIndex; // larger value means more recently used
    };

    // One vector per set; each set has 'ways' lines
    std::vector<std::vector<CacheLine>> sets;

    // Indexing used to implement true LRU
    uint64_t lruCounter = 0;

    inline uint64_t maskForBits(uint64_t bitCount) const {
        if (bitCount == 0) return 0ULL;
        return (1ULL << bitCount) - 1ULL;
    }

    inline uint64_t log2u64(uint64_t value) const {
        // Precondition per spec: value is power of two and > 0
        uint64_t bits = 0;
        while ((1ULL << bits) < value) bits++;
        return bits;
    }

    inline void computeGeometry() {
        blockOffsetBits = log2u64(config.blockSize);
        numberOfSets = config.cacheSize / (config.blockSize * config.ways);
        setIndexBits = log2u64(numberOfSets);
    }

    inline std::pair<uint64_t, uint64_t> getIndexAndTag(uint64_t address) const {
        const uint64_t indexMask = maskForBits(setIndexBits);
        const uint64_t setIndex = (address >> blockOffsetBits) & indexMask;
        const uint64_t tag = address >> (blockOffsetBits + setIndexBits);
        return {setIndex, tag};
    }

public:
    CacheConfig config;
    Cache(CacheConfig configParam, CacheDataType cacheType);

    // Access methods for reading/writing
    // @return true for hit and false for miss
    bool access(uint64_t address, CacheOperation readWrite);

    // debug: dump information as you needed
    Status dump(const std::string& base_output_name);

    uint64_t getHits() { return hits; }
    uint64_t getMisses() { return misses; }
    uint64_t getNumberOfSets() const { return numberOfSets; }
    uint64_t getBlockOffsetBits() const { return blockOffsetBits; }
    uint64_t getSetIndexBits() const { return setIndexBits; }
};
