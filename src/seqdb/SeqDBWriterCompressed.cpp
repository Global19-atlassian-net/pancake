// Authors: Ivan Sovic

#include <pacbio/seqdb/CompressedSequence.h>
#include <pacbio/seqdb/SeqDBWriterCompressed.h>
#include <pacbio/seqdb/Util.h>
#include <cmath>
#include <iostream>
#include <sstream>

namespace PacBio {
namespace Pancake {

std::unique_ptr<SeqDBWriterCompressed> CreateSeqDBWriterCompressed(
    const std::string& filenamePrefix, bool useCompression, int64_t flushSize,
    int64_t fileBlockSize)
{
    return std::make_unique<SeqDBWriterCompressed>(filenamePrefix, useCompression, flushSize,
                                                   fileBlockSize);
}

SeqDBWriterCompressed::SeqDBWriterCompressed(const std::string& filenamePrefix, bool useCompression,
                                             int64_t flushSize, int64_t fileBlockSize)
    : filenamePrefix_(filenamePrefix)
    , useCompression_(useCompression)
    , flushSizeBytes_(flushSize)
    , fileBlockSize_(fileBlockSize)
{
    // Perform the actuall throwable work here, so that the constructor doesn't throw.
    SplitPath(filenamePrefix_, parentFolder_, basenamePrefix_);
    seqBuffer_.reserve(flushSizeBytes_);
    OpenNewSequenceFile_();
    OpenNewIndexFile_();
}

SeqDBWriterCompressed::~SeqDBWriterCompressed()
{
    FlushSequenceBuffer();
    WriteIndex();
}

void SeqDBWriterCompressed::AddSequence(const std::string& header, const std::string& seq)
{
    // We need access to the open file to count the number of sequences, bases and bytes.
    if (fileLines_.empty()) {
        throw std::runtime_error("There are no output sequence files open.");
    }

    // Check if we found the file boundary. If so, flush the current buffer,
    // and open a new file. This has to be done upfront, because otherwise
    // we will end up with a single empty file for the last block.
    if (fileLines_.back().numBytes >= fileBlockSize_ && fileLines_.back().numBytes > 0) {
        FlushSequenceBuffer();
        OpenNewSequenceFile_();
    }

    int32_t numBytes = 0;
    std::vector<Range> ranges;
    int64_t numUncompressedBases = 0;
    int64_t numCompressedBases = 0;

    // Add the bases (either compressed or uncompressed), and initialize the
    // byte and length values properly.
    if (useCompression_) {
        // Compress the sequence.
        PacBio::Pancake::CompressedSequence compressed = PacBio::Pancake::CompressedSequence(seq);
        const auto& bytes = compressed.GetTwobit();
        // Add the compressed bytes to the buffer.
        seqBuffer_.insert(seqBuffer_.end(), bytes.begin(), bytes.end());
        // Set the numeric values used for the index.
        numBytes = static_cast<int32_t>(bytes.size());
        ranges = compressed.GetRanges();
        numUncompressedBases = compressed.GetNumUncompressedBases();
        numCompressedBases = compressed.GetNumCompressedBases();

    } else {
        // Convert the bytes to the internal type.
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(seq.data());
        // Add the bytes to the buffer.
        seqBuffer_.insert(seqBuffer_.end(), bytes, bytes + seq.size());
        // Set the numeric values used for the index.
        numBytes = static_cast<int32_t>(seq.size());
        ranges = {Range{0, static_cast<int32_t>(seq.size())}};
        numUncompressedBases = numBytes;
        numCompressedBases = numBytes;
    }

    // Create a new index registry object.
    SeqDBSequenceLine sl;
    sl.seqId = static_cast<int32_t>(seqLines_.size());
    sl.header = header;
    sl.numBytes = numBytes;
    sl.numBases = static_cast<int32_t>(seq.size());
    sl.fileId = fileLines_.back().fileId;
    sl.fileOffset = fileLines_.back().numBytes;
    sl.ranges = ranges;
    seqLines_.emplace_back(sl);

    // Increase counts for the current file.
    fileLines_.back().numBytes += numBytes;
    ++fileLines_.back().numSequences;
    fileLines_.back().numUncompressedBases += numUncompressedBases;
    fileLines_.back().numCompressedBases += numCompressedBases;

    // Increase the global counts.
    ++totalOutSeqs_;
    totalOutBytes_ += numBytes;

    // Flush the sequences if we reached the buffer size.
    if (static_cast<int64_t>(seqBuffer_.size()) > flushSizeBytes_) {
        FlushSequenceBuffer();
    }
}

void SeqDBWriterCompressed::OpenNewIndexFile_()
{
    outIndexFilename_ = filenamePrefix_ + ".seqdb";
    fpOutIndex_ = PacBio::Pancake::OpenFile(outIndexFilename_.c_str(), "w");
}

void SeqDBWriterCompressed::OpenNewSequenceFile_()
{
    // Register a new file object.
    SeqDBFileLine fileLine;
    fileLine.fileId = static_cast<int32_t>(fileLines_.size());
    fileLine.filename = basenamePrefix_ + ".seqdb." + std::to_string(fileLines_.size()) + ".seq";
    fileLines_.emplace_back(fileLine);

    // Open the new file and return the pointer.
    fpOutSeqs_ =
        PacBio::Pancake::OpenFile(JoinPath(parentFolder_, fileLine.filename).c_str(), "wb");
}

void SeqDBWriterCompressed::FlushSequenceBuffer()
{
    WriteSequences();
    ClearSequenceBuffer();
}

bool SeqDBWriterCompressed::WriteSequences()
{
    // An output sequence file should be open at all times, starting from construction.
    if (fpOutSeqs_ == nullptr) {
        throw std::runtime_error("Cannot write sequences because a sequence file is not open.");
    }

    // Write the actual sequences.
    size_t num = fwrite(seqBuffer_.data(), sizeof(int8_t), seqBuffer_.size(), fpOutSeqs_.get());
    return num == seqBuffer_.size();
}

void SeqDBWriterCompressed::WriteIndex()
{
    // An output index file should be open at all times, starting from construction.
    if (fpOutIndex_ == nullptr) {
        throw std::runtime_error("Cannot write the index because an output file is not open.");
    }

    // Write the version and compression information.
    fprintf(fpOutIndex_.get(), "V\t%s\n", version_.c_str());
    fprintf(fpOutIndex_.get(), "C\t%d\n",
            static_cast<int32_t>(useCompression_));  // Compression is turned on.

    // Write all the files and their sizes.
    for (const auto& f : fileLines_) {
        fprintf(fpOutIndex_.get(), "F\t%d\t%s\t%d\t%lld\t%lld\n", f.fileId, f.filename.c_str(),
                f.numSequences, f.numBytes, f.numCompressedBases);
    }

    // Write the indexes of all sequences.
    for (size_t i = 0; i < seqLines_.size(); ++i) {
        fprintf(fpOutIndex_.get(), "S\t%d\t%s\t%d\t%lld\t%d\t%d", seqLines_[i].seqId,
                seqLines_[i].header.c_str(), seqLines_[i].fileId, seqLines_[i].fileOffset,
                seqLines_[i].numBytes, seqLines_[i].numBases);
        fprintf(fpOutIndex_.get(), "\t%lu", seqLines_[i].ranges.size());
        for (const auto& r : seqLines_[i].ranges) {
            fprintf(fpOutIndex_.get(), "\t%d\t%d", r.start, r.end);
        }
        fprintf(fpOutIndex_.get(), "\n");
    }
}

void SeqDBWriterCompressed::ClearSequenceBuffer() { seqBuffer_.clear(); }

void SeqDBWriterCompressed::CloseFiles()
{
    fpOutIndex_ = nullptr;
    fpOutSeqs_ = nullptr;
}

}  // namespace Pancake
}  // namespace PacBio
