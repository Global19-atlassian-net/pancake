// Author: Ivan Sovic

#ifndef PANCAKE_SEEDDB_READER_CACHED_BLOCK_H
#define PANCAKE_SEEDDB_READER_CACHED_BLOCK_H

#include <pacbio/pancake/SeedDBIndexCache.h>
#include <pacbio/pancake/SeedDBReaderCachedBlock.h>
#include <pacbio/pancake/SequenceSeedsCached.h>
#include <pacbio/util/CommonTypes.h>
#include <pacbio/util/Util.h>
#include <memory>
#include <ostream>
#include <string>

namespace PacBio {
namespace Pancake {

class SeedDBReaderCachedBlock
{
public:
    SeedDBReaderCachedBlock(std::shared_ptr<PacBio::Pancake::SeedDBIndexCache>& seedDBCache);
    SeedDBReaderCachedBlock(std::shared_ptr<PacBio::Pancake::SeedDBIndexCache>& seedDBCache,
                            const std::vector<int32_t>& blockIds);
    ~SeedDBReaderCachedBlock();

    void LoadBlock(const std::vector<int32_t>& blockIds);
    const SequenceSeedsCached& GetSeedsForSequence(int32_t seqId) const;
    const SequenceSeedsCached& GetSeedsForSequence(const std::string& seqName) const;
    const std::vector<PacBio::Pancake::SequenceSeedsCached>& records() const { return records_; }

private:
    std::shared_ptr<PacBio::Pancake::SeedDBIndexCache> indexCache_;
    std::vector<int32_t> blockIds_;
    std::vector<PacBio::Pancake::Int128t> data_;
    std::vector<PacBio::Pancake::SequenceSeedsCached> records_;

    // Info to allow random access.
    std::unordered_map<std::string, int32_t> headerToOrdinalId_;
    std::unordered_map<int32_t, int32_t> seqIdToOrdinalId_;
};

}  // namespace Pancake
}  // namespace PacBio

#endif  // PANCAKE_SEEDDB_READER_CACHED_BLOCK_H