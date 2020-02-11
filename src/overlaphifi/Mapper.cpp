// Authors: Ivan Sovic

#include <lib/kxsort/kxsort.h>
#include <pacbio/alignment/SesDistanceBanded.h>
#include <pacbio/overlaphifi/Mapper.h>
#include <pacbio/overlaphifi/OverlapWriter.h>
#include <pacbio/seqdb/Util.h>
#include <pacbio/util/TicToc.h>
#include <pbcopper/logging/Logging.h>
#include <algorithm>
#include <iostream>
#include <sstream>

namespace PacBio {
namespace Pancake {

// #define PANCAKE_DEBUG

MapperResult Mapper::Map(const PacBio::Pancake::SeqDBReaderCached& targetSeqs,
                         const PacBio::Pancake::SeedIndex& index,
                         const PacBio::Pancake::FastaSequenceId& querySeq,
                         const PacBio::Pancake::SequenceSeeds& querySeeds, int64_t freqCutoff) const
{
#ifdef PANCAKE_DEBUG
    PBLOG_INFO << "Mapping query ID = " << querySeq.Id() << ", header = " << querySeq.Name();
#endif

    if (static_cast<int64_t>(querySeq.Bases().size()) < settings_.MinQueryLen) {
        return {};
    }

    TicToc ttCollectHits;
    std::vector<SeedHit> hits;
    index.CollectHits(querySeeds.Seeds(), hits, freqCutoff);
    ttCollectHits.Stop();

    TicToc ttSortHits;
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return PackSeedHitWithDiagonalTo128_(a) < PackSeedHitWithDiagonalTo128_(b);
    });
    ttSortHits.Stop();

    TicToc ttChain;
    auto overlaps = FormDiagonalAnchors_(hits, querySeq, index.GetCache(), settings_.ChainBandwidth,
                                         settings_.MinNumSeeds, settings_.MinChainSpan, true,
                                         settings_.SkipSymmetricOverlaps);
    ttChain.Stop();

    // Filter out multiple hits per query-target pair (e.g. tandem repeats) by
    // taking only the longest overlap chain.
    TicToc ttFilterTandem;
    if (settings_.OneHitPerTarget) {
        overlaps = FilterTandemOverlaps_(overlaps);
    }
    ttFilterTandem.Stop();

    TicToc ttAlign;
    overlaps = AlignOverlaps_(targetSeqs, querySeq, overlaps, settings_.AlignmentBandwidth,
                              settings_.AlignmentMaxD);
    ttAlign.Stop();

    TicToc ttFilter;
    overlaps =
        FilterOverlaps_(overlaps, settings_.MinNumSeeds, settings_.MinIdentity,
                        settings_.MinMappedLength, settings_.MinQueryLen, settings_.MinTargetLen);
    ttFilter.Stop();

#ifdef PANCAKE_DEBUG
    for (const auto& ovl : overlaps) {
        OverlapWriter::PrintOverlapAsM4(stdout, ovl, querySeq.Name(),
                                        targetSeqs.GetSequence(ovl->Bid).Name(), false);
    }

    PBLOG_INFO << "Num anchors: " << overlaps.size();
    PBLOG_INFO << "Collected " << hits.size() << " hits.";
    PBLOG_INFO << "Time - collecting hits: " << ttCollectHits.GetMillisecs() << " ms / "
               << ttCollectHits.GetCpuMillisecs() << " CPU ms";
    PBLOG_INFO << "Time - sorting: " << ttSortHits.GetMillisecs() << " ms / "
               << ttSortHits.GetCpuMillisecs() << " CPU ms";
    PBLOG_INFO << "Time - chaining: " << ttChain.GetMillisecs() << " ms / "
               << ttChain.GetCpuMillisecs() << " CPU ms";
    PBLOG_INFO << "Time - tandem filter: " << ttFilterTandem.GetMillisecs() << " ms / "
               << ttFilterTandem.GetCpuMillisecs() << " CPU ms";
    PBLOG_INFO << "Time - alignment: " << ttAlign.GetMillisecs() << " ms / "
               << ttAlign.GetCpuMillisecs() << " CPU ms";
    PBLOG_INFO << "Time - filter: " << ttFilter.GetMillisecs() << " ms / "
               << ttFilter.GetCpuMillisecs() << " CPU ms";
    DebugWriteSeedHits_("temp/debug/mapper-0-seed_hits.csv", hits, 30, querySeq.Name(),
                        querySeq.Bases().size(), "target", 0);
#endif

    MapperResult result;
    std::swap(result.overlaps, overlaps);
    return result;
}

OverlapPtr Mapper::MakeOverlap_(const std::vector<SeedHit>& sortedHits,
                                const PacBio::Pancake::FastaSequenceId& querySeq,
                                const std::shared_ptr<PacBio::Pancake::SeedDBIndexCache> indexCache,
                                int32_t beginId, int32_t endId, int32_t minTargetPosId,
                                int32_t maxTargetPosId)
{

    const auto& beginHit = sortedHits[minTargetPosId];
    const auto& endHit = sortedHits[maxTargetPosId];

    const int32_t targetId = beginHit.targetId;
    const int32_t numSeeds = endId - beginId;

    if (endHit.targetId != beginHit.targetId) {
        std::ostringstream oss;
        oss << "The targetId of the first and last seed does not match, in MakeOverlap. "
               "beginHit.targetId "
            << beginHit.targetId << ", endHit.targetId = " << endHit.targetId;
        throw std::runtime_error(oss.str());
    }

    const float score = numSeeds;
    const float identity = 0.0;
    const int32_t editDist = -1;

    const auto& sl = indexCache->GetSeedsLine(targetId);
    const int32_t targetLen = sl.numBases;

    return createOverlap(querySeq.Id(), targetId, score, identity, 0, beginHit.queryPos,
                         endHit.queryPos, querySeq.Bases().size(), beginHit.targetRev,
                         beginHit.targetPos, endHit.targetPos, targetLen, editDist, numSeeds);
}

std::vector<OverlapPtr> Mapper::FormDiagonalAnchors_(
    const std::vector<SeedHit>& sortedHits, const PacBio::Pancake::FastaSequenceId& querySeq,
    const std::shared_ptr<PacBio::Pancake::SeedDBIndexCache> indexCache, int32_t chainBandwidth,
    int32_t minNumSeeds, int32_t minChainSpan, bool skipSelfHits, bool skipSymmetricOverlaps)
{

    if (sortedHits.empty()) {
        return {};
    }

    std::vector<OverlapPtr> overlaps;

    const int32_t numHits = static_cast<int32_t>(sortedHits.size());
    int32_t beginId = 0;
    int32_t beginDiag = sortedHits[beginId].Diagonal();

    // This is a combination of <targetPos, queryPos>, intended for simple comparison
    // without defining a custom complicated comparison operator.
    uint64_t minTargetQueryPosCombo = (static_cast<uint64_t>(sortedHits[beginId].targetPos) << 32) |
                                      (static_cast<uint64_t>(sortedHits[beginId].queryPos));
    uint64_t maxTargetQueryPosCombo = minTargetQueryPosCombo;
    int32_t minPosId = 0;
    int32_t maxPosId = 0;

    for (int32_t i = 0; i < numHits; ++i) {
        const auto& prevHit = sortedHits[beginId];
        const auto& currHit = sortedHits[i];
        const int32_t currDiag = currHit.Diagonal();
        const int32_t diagDiff = abs(currDiag - beginDiag);
        const uint64_t targetQueryPosCombo =
            (static_cast<uint64_t>(sortedHits[i].targetPos) << 32) |
            (static_cast<uint64_t>(sortedHits[i].queryPos));

        if (currHit.targetId != prevHit.targetId || currHit.targetRev != prevHit.targetRev ||
            diagDiff > chainBandwidth) {
            auto ovl =
                MakeOverlap_(sortedHits, querySeq, indexCache, beginId, i, minPosId, maxPosId);
            beginId = i;
            beginDiag = currDiag;

            // Add a new overlap.
            if (ovl->NumSeeds >= minNumSeeds && ovl->ASpan() > minChainSpan &&
                ovl->BSpan() > minChainSpan &&
                (skipSelfHits == false || (skipSelfHits && ovl->Bid != ovl->Aid)) &&
                (skipSymmetricOverlaps == false ||
                 (skipSymmetricOverlaps && ovl->Bid < ovl->Aid))) {
                overlaps.emplace_back(std::move(ovl));
            }
            minPosId = maxPosId = i;
            minTargetQueryPosCombo = maxTargetQueryPosCombo = targetQueryPosCombo;
        }

        // Track the minimum and maximum target positions for each diagonal.
        if (targetQueryPosCombo < minTargetQueryPosCombo) {
            minPosId = i;
            minTargetQueryPosCombo = targetQueryPosCombo;
        }
        if (targetQueryPosCombo > maxTargetQueryPosCombo) {
            maxPosId = i;
            maxTargetQueryPosCombo = targetQueryPosCombo;
        }
    }

    if ((numHits - beginId) > 0) {
        auto ovl =
            MakeOverlap_(sortedHits, querySeq, indexCache, beginId, numHits, minPosId, maxPosId);
        // Add a new overlap.
        if (ovl->NumSeeds >= minNumSeeds && ovl->ASpan() > minChainSpan &&
            ovl->BSpan() > minChainSpan &&
            (skipSelfHits == false || (skipSelfHits && ovl->Bid != ovl->Aid)) &&
            (skipSymmetricOverlaps == false || (skipSymmetricOverlaps && ovl->Bid < ovl->Aid))) {
            overlaps.emplace_back(std::move(ovl));
        }
    }

    return overlaps;
}

std::vector<OverlapPtr> Mapper::FilterOverlaps_(const std::vector<OverlapPtr>& overlaps,
                                                int32_t minNumSeeds, float minIdentity,
                                                int32_t minMappedSpan, int32_t minQueryLen,
                                                int32_t minTargetLen)
{

    std::vector<OverlapPtr> ret;
    for (const auto& ovl : overlaps) {
        if (ovl->Identity < minIdentity || ovl->ASpan() < minMappedSpan ||
            ovl->BSpan() < minMappedSpan || ovl->NumSeeds < minNumSeeds ||
            ovl->Alen < minQueryLen || ovl->Blen < minTargetLen) {
            continue;
        }
        ret.emplace_back(createOverlap(ovl));
    }

    return ret;
}

std::vector<OverlapPtr> Mapper::FilterTandemOverlaps_(const std::vector<OverlapPtr>& overlaps)
{
    if (overlaps.empty()) {
        return {};
    }

    // Make an internal copy for sorting.
    std::vector<OverlapPtr> overlaps_copy;
    for (const auto& ovl : overlaps) {
        overlaps_copy.emplace_back(createOverlap(ovl));
    }

    // Sort by length.
    std::sort(overlaps_copy.begin(), overlaps_copy.end(), [](const auto& a, const auto& b) {
        return a->Bid < b->Bid ||
               (a->Bid == b->Bid &&
                std::max(a->ASpan(), a->BSpan()) > std::max(b->ASpan(), b->BSpan()));
    });

    // Accumulate the results.
    std::vector<OverlapPtr> ret;
    ret.emplace_back(createOverlap(overlaps_copy.front()));
    for (const auto& ovl : overlaps_copy) {
        if (ovl->Bid == ret.back()->Bid) {
            continue;
        }
        ret.emplace_back(createOverlap(ovl));
    }

    return ret;
}

std::vector<OverlapPtr> Mapper::AlignOverlaps_(const PacBio::Pancake::SeqDBReaderCached& targetSeqs,
                                               const PacBio::Pancake::FastaSequenceId& querySeq,
                                               const std::vector<OverlapPtr>& overlaps,
                                               double alignBandwidth, double alignMaxDiff)
{
    std::vector<OverlapPtr> ret;
    const std::string reverseQuerySeq =
        PacBio::Pancake::ReverseComplement(querySeq.Bases(), 0, querySeq.Bases().size());

    for (size_t i = 0; i < overlaps.size(); ++i) {
        const auto& targetSeq = targetSeqs.GetSequence(overlaps[i]->Bid);
        auto newOverlap = AlignOverlap_(targetSeq, querySeq, reverseQuerySeq, overlaps[i],
                                        alignBandwidth, alignMaxDiff);
        ret.emplace_back(std::move(newOverlap));
    }
    return ret;
}

std::string Mapper::FetchTargetSubsequence_(const PacBio::Pancake::FastaSequenceId& targetSeq,
                                            int32_t seqStart, int32_t seqEnd, bool revCmp)
{
    const int32_t seqLen = targetSeq.Bases().size();
    if (seqEnd == seqStart) {
        return {};
    }
    if (seqStart < 0 || seqEnd < 0 || seqStart > seqLen || seqEnd > seqLen || seqEnd < seqStart) {
        std::ostringstream oss;
        oss << "Invalid seqStart or seqEnd in a call to FetchTargetSubsequence_. seqStart = "
            << seqStart << ", seqEnd = " << seqEnd << ", seqLen = " << seqLen
            << ", revCmp = " << revCmp << ".";
        std::cerr << oss.str() << "\n";
        throw std::runtime_error(oss.str());
    }
    seqEnd = (seqEnd == 0) ? seqLen : seqEnd;
    if (revCmp) {
        return PacBio::Pancake::ReverseComplement(targetSeq.Bases(), seqStart, seqEnd);
    }
    // No need to reverse complement.
    return targetSeq.Bases().substr(seqStart, seqEnd - seqStart);
}

OverlapPtr Mapper::AlignOverlap_(const PacBio::Pancake::FastaSequenceId& targetSeq,
                                 const PacBio::Pancake::FastaSequenceId& querySeq,
                                 const std::string reverseQuerySeq, const OverlapPtr& ovl,
                                 double alignBandwidth, double alignMaxDiff)
{

    if (ovl == nullptr) {
        return nullptr;
    }

    OverlapPtr ret = createOverlap(ovl);
    int32_t diffsRight = 0;

    ///////////////////////////
    /// Align forward pass. ///
    ///////////////////////////
    {
        const int32_t qStart = ovl->Astart;
        const int32_t qEnd = ovl->Alen;
        const int32_t qSpan = qEnd - qStart;
        const int32_t tStartFwd = ovl->Brev ? (ovl->Blen - ovl->Bend) : ovl->Bstart;
        const int32_t tEndFwd = ovl->Brev ? (ovl->Blen - ovl->Bstart) : ovl->Bend;
        const std::string tseq =
            (ovl->Brev) ? FetchTargetSubsequence_(targetSeq, 0, tEndFwd, ovl->Brev)
                        : FetchTargetSubsequence_(targetSeq, tStartFwd, ovl->Blen, ovl->Brev);
        const int32_t tSpan = tseq.size();
        const int32_t dMax = ovl->Alen * alignMaxDiff;
        const int32_t bandwidth = std::min(ovl->Blen, ovl->Alen) * alignBandwidth;
        const auto sesResult = PacBio::Pancake::Alignment::SESDistanceBanded(
            querySeq.Bases().c_str() + qStart, qSpan, tseq.c_str(), tSpan, dMax, bandwidth);
        ret->Aend = sesResult.lastQueryPos;
        ret->Bend = sesResult.lastTargetPos;
        ret->Aend += ovl->Astart;
        ret->Bend += ovl->Bstart;
        ret->EditDistance = sesResult.diffs;
        ret->Score = -std::max(ret->ASpan(), ret->BSpan());
        diffsRight = sesResult.diffs;
    }

    ///////////////////////////
    /// Align reverse pass. ///
    ///////////////////////////
    {
        // Reverse query coordinates.
        const int32_t qStart = ret->Alen - ret->Astart;
        const int32_t qEnd = ret->Alen;
        const int32_t qSpan = qEnd - qStart;
        const int32_t tStartFwd = ret->Brev ? (ret->Blen - ret->Bend) : ret->Bstart;
        const int32_t tEndFwd = ret->Brev ? (ret->Blen - ret->Bstart) : ret->Bend;
        const std::string tSeq =
            (ovl->Brev) ? FetchTargetSubsequence_(targetSeq, tEndFwd, ret->Blen, !ret->Brev)
                        : FetchTargetSubsequence_(targetSeq, 0, tStartFwd, !ret->Brev);
        const int32_t tSpan = tSeq.size();
        const int32_t dMax = ovl->Alen * alignMaxDiff - diffsRight;
        const int32_t bandwidth = std::min(ovl->Blen, ovl->Alen) * alignBandwidth;
        const auto sesResult = PacBio::Pancake::Alignment::SESDistanceBanded(
            reverseQuerySeq.c_str() + qStart, qSpan, tSeq.c_str(), tSpan, dMax, bandwidth);
        ret->Astart = ovl->Astart - sesResult.lastQueryPos;
        ret->Bstart = ovl->Bstart - sesResult.lastTargetPos;
        ret->EditDistance = diffsRight + sesResult.diffs;
        ret->Score = -std::max(ret->ASpan(), ret->BSpan());
        const float span = std::max(ret->ASpan(), ret->BSpan());
        ret->Identity =
            100.0f *
            ((span != 0) ? ((span - static_cast<float>(ret->EditDistance)) / span) : -2.0f);
    }

    return ret;
}

void Mapper::DebugWriteSeedHits_(const std::string& outPath, const std::vector<SeedHit>& hits,
                                 int32_t seedLen, const std::string& queryName, int64_t queryLen,
                                 const std::string& targetName, int64_t targetLen)
{
    std::ofstream ofs(outPath);
    // Simply walk away if the file cannot be open.
    // Avoid writing debug output if a specific path is not available.
    if (ofs.is_open() == false) {
        return;
    }
    ofs << queryName << "\t0\t" << queryLen << "\t" << targetName << "\t0\t" << targetLen << "\t0.0"
        << std::endl;
    for (size_t i = 0; i < hits.size(); ++i) {
        int32_t clusterId = hits[i].targetId * 2 + (hits[i].targetRev ? 1 : 0);
        ofs << hits[i].queryPos << "\t" << hits[i].targetPos << "\t"
            << "\t" << clusterId << std::endl;
        ofs << hits[i].queryPos + seedLen << "\t" << hits[i].targetPos + seedLen << "\t"
            << clusterId << std::endl;
    }
}

__int128 Mapper::PackSeedHitWithDiagonalTo128_(const SeedHit& sh)
{
    __int128 ret = 0;
    const int32_t diag = (sh.targetPos - sh.queryPos);
    ret = ((static_cast<__int128>(sh.targetId) & MASK_32bit) << 97) |
          ((static_cast<__int128>(sh.targetRev) & MASK_32bit) << 96) |
          ((static_cast<__int128>(diag) & MASK_32bit) << 64) |
          ((static_cast<__int128>(sh.targetPos) & MASK_32bit) << 32) |
          (static_cast<__int128>(sh.queryPos) & MASK_32bit);
    return ret;
}

}  // namespace Pancake
}  // namespace PacBio