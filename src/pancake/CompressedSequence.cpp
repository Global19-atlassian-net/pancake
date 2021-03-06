// Authors: Ivan Sovic

#include <pacbio/pancake/CompressedSequence.h>

namespace PacBio {
namespace Pancake {

CompressedSequence::CompressedSequence() = default;

CompressedSequence::CompressedSequence(const std::string& bases) { SetFromBases(bases); }

CompressedSequence::~CompressedSequence() = default;

void CompressedSequence::SetFromBases(const std::string& bases)
{
    numCompressedBases_ = CompressSequence(bases, twobit_, ranges_);
    numUncompressedBases_ = bases.size();
}

}  // namespace Pancake
}  // namespace PacBio
