// Authors: Ivan Sovic

#include <lib/kxsort/kxsort.h>
#include <pacbio/overlaphifi/SeedIndex.h>
#include <pacbio/seeddb/Seed.h>
#include <pacbio/util/TicToc.h>
#include <pbcopper/logging/Logging.h>
#include <iostream>
#include <sstream>

namespace PacBio {
namespace Pancake {

SeedIndex::SeedIndex(std::shared_ptr<PacBio::Pancake::SeedDBIndexCache>& seedDBCache,
                     std::vector<PacBio::Pancake::SeedDB::SeedRaw>&& seeds)
    : seedDBCache_(seedDBCache), seeds_(std::move(seeds))
{
#ifdef SEED_INDEX_USING_DENSEHASH
    hash_.set_empty_key(
        SEED_INDEX_EMPTY_HASH_KEY);  // Densehash requires this to be defined on top.
#endif

    BuildHash_();
}

SeedIndex::~SeedIndex() = default;

void SeedIndex::BuildHash_()
{
    // Stop early.
    if (seeds_.empty()) {
        return;
    }

    // Sort by key first.
    TicToc ttSort;
    kx::radix_sort(seeds_.begin(), seeds_.end());
    ttSort.Stop();

    PBLOG_INFO << "Sorted the seeds in " << ttSort.GetSecs() << " sec / " << ttSort.GetCpuSecs()
               << " CPU sec";

    // Clear the storage for the hash.
    hash_.clear();

#ifdef SEED_INDEX_USING_DENSEHASH
    hash_.resize(seeds_.size());
#elif defined SEED_INDEX_USING_SPARSEHASH
    hash_.resize(seeds_.size());
#elif defined SEED_INDEX_USING_UNORDERED_MAP
    hash_.reserve(seeds_.size());
#endif

    // Fill out the hash table.
    int64_t start = 0;
    int64_t end = 0;
    uint64_t prevKey = PacBio::Pancake::SeedDB::Seed::DecodeKey(seeds_[0]);
    for (size_t i = 0; i < seeds_.size(); ++i) {
        uint64_t key = PacBio::Pancake::SeedDB::Seed::DecodeKey(seeds_[i]);
        if (key == prevKey) {
            ++end;
        } else {
            hash_[prevKey] = std::make_pair(start, end);
            start = i;
            end = i + 1;
        }
        prevKey = key;
    }
    if (end > start) {
        hash_[prevKey] = std::make_pair(start, end);
    }
}

void SeedIndex::ComputeFrequencyStats(double percentileCutoff, int64_t& retFreqMax,
                                      double& retFreqAvg, double& retFreqMedian,
                                      int64_t& retFreqCutoff) const
{
    retFreqMax = 0;
    retFreqAvg = 0.0;
    retFreqMedian = 0.0;
    retFreqCutoff = 0;

    // Sanity check.
    if (percentileCutoff < 0.0 || percentileCutoff > 1.0) {
        std::ostringstream oss;
        oss << "Invalid percentileCutoff value, should be in range [0.0, 1.0] but provided value = "
            << percentileCutoff;
        throw std::runtime_error(oss.str());
    }

    // Empty input.
    if (hash_.empty()) {
        return;
    }

    // Collect all frequencies as a vector.
    std::vector<int64_t> freqs;
    double sumFreqs = 0.0;
    int64_t numValidKeys = 0;
    freqs.reserve(hash_.size());
    for (auto it = hash_.begin(); it != hash_.end(); ++it) {
        int64_t start = std::get<0>(it->second);
        int64_t end = std::get<1>(it->second);
        int64_t span = end - start;
        if (span == 0) {
            continue;
        }
        freqs.emplace_back(span);
        sumFreqs += static_cast<double>(span);
        ++numValidKeys;
    }

    // Sanity check that there actually are enough valid keys in the hash.
    if (numValidKeys <= 0) {
        throw std::runtime_error("Invalid number of valid keys! numValidKeys = " +
                                 std::to_string(numValidKeys));
    }

    // Sort the vector for percentile calculation.
    kx::radix_sort(freqs.begin(), freqs.end());

    // Find the percentile cutoff ID.
    double numKeys = numValidKeys;
    int64_t cutoffId = floor(numKeys * (1.0 - percentileCutoff));

    // Return values.
    retFreqMax = freqs.back();
    retFreqCutoff = freqs[cutoffId];
    retFreqAvg = sumFreqs / static_cast<double>(numKeys);
    retFreqMedian = (static_cast<double>(freqs[numValidKeys / 2]) +
                     static_cast<double>(freqs[(numValidKeys - 1) / 2])) /
                    2.0;
}

int64_t SeedIndex::GetSeeds(uint64_t key,
                            std::vector<PacBio::Pancake::SeedDB::SeedRaw>& seeds) const
{
    seeds.clear();
    auto it = hash_.find(key);
    if (it == hash_.end()) {
        return 0;
    }
    int64_t start = std::get<0>(it->second);
    int64_t end = std::get<1>(it->second);
    seeds.insert(seeds.end(), seeds_.begin() + start, seeds_.begin() + end);
    return (end - start);
}

bool SeedIndex::CollectHits(const std::vector<PacBio::Pancake::SeedDB::SeedRaw>& querySeeds,
                            std::vector<SeedHit>& hits, int64_t freqCutoff) const
{
    hits.clear();

    const int32_t kmerSize = seedDBCache_->seedParams.KmerSize;

    for (const auto& querySeed : querySeeds) {
        auto decodedQuery = PacBio::Pancake::SeedDB::Seed(querySeed);
        auto it = hash_.find(decodedQuery.key);
        if (it != hash_.end()) {
            int64_t start = std::get<0>(it->second);
            int64_t end = std::get<1>(it->second);
            // Skip very frequent seeds.
            if (freqCutoff > 0 && (end - start) > freqCutoff) {
                continue;
            }
            for (int64_t i = start; i < end; ++i) {
                auto decodedTarget = PacBio::Pancake::SeedDB::Seed(seeds_[i]);
                bool isRev = false;
                int32_t targetPos = decodedTarget.pos;

                if (decodedQuery.IsRev() != decodedTarget.IsRev()) {
                    isRev = true;
                    // targetPos = seq->len() - (decodedTarget.pos + 1);
                    const auto& sl = seedDBCache_->GetSeedsLine(decodedTarget.seqID);
                    targetPos = sl.numBases - (decodedTarget.pos + kmerSize);
                    // TODO: This will be off if the homopolymer compression is used.
                    // In that case, the seed span is not the same as t he kmerSize.
                }

                SeedHit hit{decodedTarget.seqID, isRev, targetPos, 0, decodedQuery.pos};
                hits.emplace_back(hit);
            }
        }
    }

    return hits.size() > 0;
}

}  // namespace Pancake
}  // namespace PacBio