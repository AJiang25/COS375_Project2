// Sample cache implementation with random hit return
// TODO: Modify this file to model an LRU cache as in the project description

#include "cache.h"
#include <cmath> 
// #include <random> I don't think we need this anymore?

using namespace std;

// Don't need this anymore either?
// Random generator for cache hit/miss simulation,
// static std::mt19937 generator(42);  // Fixed seed for deterministic results
// std::uniform_real_distribution<double> distribution(0.0, 1.0);

// Constructor definition
Cache::Cache(CacheConfig configParam, CacheDataType cacheType) : config(configParam) {
    // Initializations
    hits = 0;
    misses = 0;
    type = cacheType;

    // Here you can initialize other cache-specific attributes
    // For instance, if you had cache tables or other structures, initialize them here
    globalLRUCounter = 0;

    // Compute parts of address used for Tag, Index, & Offset
    numSets = (config.cacheSize / config.blockSize) / config.ways;

    // maybe use ilog2? since they're always powers of two?
    indexBits = log2(numSets);
    offsetBits = log2(config.blockSize);

    // initialize cache table, must resize to set len/width
    sets.resize(numSets, std::vector<Line>(config.ways));
    for (uint64_t set = 0; set < numSets; ++set) {
        for (uint64_t way = 0; way < config.ways; ++way) {
            sets[set][way].valid = false;
            sets[set][way].tag = 0;
            sets[set][way].lruStamp = 0;
        }
    }
}

// Access method definition
bool Cache::access(uint64_t address, CacheOperation readWrite) {
    // Decoding the Addr into Tag, Index, & Offset
    uint64_t indexMask = (1ULL << indexBits) - 1;
    uint64_t index = (address >> offsetBits) & indexMask;
    uint64_t tag = address >> (offsetBits + indexBits);

    ++globalLRUCounter;

    // Pointer to the current set
    std::vector<Line> &curSet = sets[index];

    // --------------------------------------------------------
    // Cache Hit
    // --------------------------------------------------------

    // update counter + LRU
    for (Line &line : curSet) {
        if (line.valid && line.tag == tag) {
            ++hits;
            line.lruStamp = globalLRUCounter;
            return true;
        }
    }

    // --------------------------------------------------------
    // Cache Miss
    // --------------------------------------------------------
    ++misses;

    // find invalid line
    Line *pointer = nullptr;
    for (Line &line : curSet) {
        if (!line.valid) {
            pointer = &line;
            break;
        }
    }

    // if no invalid line, use LRU
    if (!pointer) {
        pointer = &curSet[0];
        for (Line &line : curSet) {
            if (line.lruStamp < pointer->lruStamp) {
                pointer = &line;
            }
        }
    }

    // replace line w/ new block
    pointer->valid = true;
    pointer->tag = tag;
    pointer->lruStamp = globalLRUCounter;

    // --------------------------------------------------------
    // Prefetch Next Block
    // --------------------------------------------------------
    uint64_t nextAddr = address + config.blockSize;
    uint64_t nextIndexMask = (1ULL << indexBits) - 1;
    uint64_t nextIndex = (nextAddr >> offsetBits) & nextIndexMask;
    uint64_t nextTag = nextAddr >> (offsetBits + indexBits);

    std::vector<Line> &nextSet = sets[nextIndex];
    bool nextBlockFound = false;

    // Check if already in cache
    for (Line &line : nextSet) {
        if (line.valid && line.tag == nextTag) {
            nextBlockFound = true;
            break;
        }
    }

    if (!nextBlockFound) {
        // Find victim in next set
        Line *nextPointer = nullptr;
        for (Line &line : nextSet) {
            if (!line.valid) {
                nextPointer = &line;
                break;
            }
        }
        if (!nextPointer) {
             nextPointer = &nextSet[0];
             for (Line &line : nextSet) {
                 if (line.lruStamp < nextPointer->lruStamp) {
                     nextPointer = &line;
                 }
             }
        }

        // Bring in next block
        nextPointer->valid = true;
        nextPointer->tag = nextTag;
        nextPointer->lruStamp = globalLRUCounter; 
    }

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
        // Does this need to be switched to config.ways instead???
        cache_out << "Ways: " << (config.ways == 1) << std::endl;
        cache_out << "Miss Latency: " << config.missLatency << " cycles" << std::endl;
        cache_out << "---------------------" << endl;
        cache_out << "End Register Values" << endl;
        cache_out << "---------------------" << endl;
        return SUCCESS;
    } else {
        cerr << LOG_ERROR << "Could not create cache state dump file" << endl;
        return ERROR;
    }
}
