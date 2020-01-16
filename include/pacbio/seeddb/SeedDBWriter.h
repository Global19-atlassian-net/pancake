// Author: Ivan Sovic

#ifndef PANCAKE_SEEDDB_WRITER_COMPRESSED_H
#define PANCAKE_SEEDDB_WRITER_COMPRESSED_H

#include <pacbio/seqdb/Util.h>
#include <seqdb/FastaSequenceId.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/*
    # Seed DB
    1. Metadata file: <prefix>.seeddb
    2. One or more files with seeds: <prefix>.<file_id>.seeds

    ## Metadata file:
    Text file containing the following fields:
    ```
    V <string:semantic_version>
    F <int32_t:file_id> <string:filename> <int32_t:num_seqs> <int64_t:file_size_in_bytes>
    S <int32_t:seq_id> <string:header> <int32_t:file_id> <int64_t:file_offset> <int64_t:byte_size> <int32_t:num_bases> <int32_t:num_seeds>
    B <int32_t:block_id> <int32_t:start_seq_id> <int32_t:end_seq_id> <int64_t:byte_size>
    ```

    ## Seed file:
    Binary file. Contains all bytes concatenated together, no headers, no new line chars.
*/

/// \brief      Container, describes a seeds file which accompanies the SeedDB index.
///
class SeedDBFileLine
{
public:
    int32_t fileId = 0;
    std::string filename;
    int32_t numSequences = 0;
    int64_t numBytes = 0;
};

/// \brief      Container, index information for a particular sequence's set of seeds.
///
class SeedDBSeedsLine
{
public:
    int32_t seqId = 0;
    std::string header;
    int32_t fileId = 0;
    int64_t fileOffset = 0;
    int64_t numBytes = 0;
    int32_t numBases = 0;
    int32_t numSeeds = 0;
};

class SeedDBBlockLine
{
public:
    int32_t blockId = 0;
    int32_t startSeqId = -1;
    int32_t endSeqId = -1;
    int64_t numBytes = 0;

    int32_t Span() const { return endSeqId - startSeqId; }
};

namespace PacBio {
namespace Pancake {

class SeedDBWriter
{
public:
    SeedDBWriter(const std::string& filenamePrefix, bool splitBlocks);
    ~SeedDBWriter();

    void WriteSeeds(const std::string& seqName, int32_t seqId, int32_t seqLen,
                    const std::vector<__int128>& seeds);
    void WriteSeeds(const std::vector<PacBio::Pancake::FastaSequenceId>& seqs,
                    const std::vector<std::vector<__int128>>& seeds);
    void MarkBlockEnd();
    void WriteIndex();
    void CloseFiles();

private:
    void OpenNewSeedsFile_();
    void OpenNewIndexFile_();

    const std::string version_{"0.1.0"};
    std::string filenamePrefix_;
    std::string parentFolder_;
    std::string basenamePrefix_;
    bool splitBlocks_;
    std::vector<SeedDBFileLine> fileLines_;
    std::vector<SeedDBSeedsLine> seedsLines_;
    std::vector<SeedDBBlockLine> blockLines_;
    SeedDBBlockLine currentBlock_;
    bool openNewSeedsFileUponNextWrite_;
    // Output file handlers.
    std::unique_ptr<FILE, FileDeleter> fpOutIndex_{nullptr};
    std::string outIndexFilename_;
    std::unique_ptr<FILE, FileDeleter> fpOutSeeds_{nullptr};
};

std::unique_ptr<SeedDBWriter> CreateSeedDBWriter(const std::string& filenamePrefix,
                                                 bool splitBlocks);

}  // namespace Pancake
}  // namespace PacBio

#endif  // PANCAKE_SEQDB_WRITER_H