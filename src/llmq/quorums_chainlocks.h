// Copyright (c) 2019-2020 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_QUORUMS_CHAINLOCKS_H
#define BITCOIN_LLMQ_QUORUMS_CHAINLOCKS_H

#include <llmq/quorums.h>
#include <llmq/quorums_signing.h>

#include <net.h>
#include <chainparams.h>

#include <atomic>
#include <unordered_set>

#include <boost/thread.hpp>

class CBlockIndex;
class CScheduler;

namespace llmq
{

class CChainLockSig
{
public:
    int32_t nHeight{-1};
    uint256 blockHash;
    CBLSSignature sig;
    std::vector<bool> signers;

public:
    ADD_SERIALIZE_METHODS

    template<typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(nHeight);
        READWRITE(blockHash);
        READWRITE(sig);
        if (!(s.GetType() & SER_GETHASH) && (s.GetVersion() >= MULTI_QUORUM_CHAINLOCKS_VERSION)) {
            READWRITE(DYNBITSET(signers));
        }
    }

    bool IsNull() const;
    std::string ToString() const;
};

typedef std::shared_ptr<const CChainLockSig> CChainLockSigCPtr;

struct ReverseHeightComparator
{
    bool operator()(const int h1, const int h2) const {
        return h1 > h2;
    }
};

class CChainLocksHandler : public CRecoveredSigsListener
{
    static const int64_t CLEANUP_INTERVAL = 1000 * 30;
    static const int64_t CLEANUP_SEEN_TIMEOUT = 24 * 60 * 60 * 1000;

    // how long to wait for islocks until we consider a block with non-islocked TXs to be safe to sign
    static const int64_t WAIT_FOR_ISLOCK_TIMEOUT = 10 * 60;

private:
    CScheduler* scheduler;
    boost::thread* scheduler_thread;
    CCriticalSection cs;
    bool tryLockChainTipScheduled{false};
    bool isEnabled{false};
    bool isEnforced{false};

    CChainLockSig mostRecentChainLockShare;
    CChainLockSig bestChainLockWithKnownBlock;
    const CBlockIndex* bestChainLockBlockIndex{nullptr};
    const CBlockIndex* lastNotifyChainLockBlockIndex{nullptr};

    // Keep best chainlock shares and candidates, sorted by height (highest heght first).
    std::map<int, std::map<CQuorumCPtr, CChainLockSigCPtr>, ReverseHeightComparator> bestChainLockShares;
    std::map<int, CChainLockSigCPtr, ReverseHeightComparator> bestChainLockCandidates;

    int32_t lastSignedHeight{-1};
    std::set<uint256> lastSignedRequestIds;
    uint256 lastSignedMsgHash;

    // We keep track of txids from recently received blocks so that we can check if all TXs got islocked
    typedef std::unordered_map<uint256, std::shared_ptr<std::unordered_set<uint256, StaticSaltedHasher>>> BlockTxs;
    BlockTxs blockTxs;
    std::unordered_map<uint256, int64_t> txFirstSeenTime;

    std::map<uint256, int64_t> seenChainLocks;

    int64_t lastCleanupTime{0};

public:
    explicit CChainLocksHandler();
    ~CChainLocksHandler();

    void Start();
    void Stop();

    bool AlreadyHave(const CInv& inv);
    bool GetChainLockByHash(const uint256& hash, CChainLockSig& ret);
    const CChainLockSig GetMostRecentChainLock();
    const CChainLockSig GetBestChainLock();

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv);
    void ProcessNewChainLock(NodeId from, CChainLockSig& clsig, const uint256& hash, const uint256& idIn = uint256());
    void AcceptedBlockHeader(const CBlockIndex* pindexNew);
    void UpdatedBlockTip(const CBlockIndex* pindexNew);
    void TransactionAddedToMempool(const CTransactionRef& tx, int64_t nAcceptTime);
    void BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex, const std::vector<CTransactionRef>& vtxConflicted);
    void BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexDisconnected);
    void CheckActiveState();
    void TrySignChainTip();
    void EnforceBestChainLock();
    virtual void HandleNewRecoveredSig(const CRecoveredSig& recoveredSig);

    bool HasChainLock(int nHeight, const uint256& blockHash);
    bool HasConflictingChainLock(int nHeight, const uint256& blockHash);

    bool IsTxSafeForMining(const uint256& txid);

private:
    // these require locks to be held already
    bool InternalHasChainLock(int nHeight, const uint256& blockHash);
    bool InternalHasConflictingChainLock(int nHeight, const uint256& blockHash);

    BlockTxs::mapped_type GetBlockTxs(const uint256& blockHash);

    bool TryUpdateBestChainLock(const CBlockIndex* pindex);
    bool VerifyChainLockShare(const CChainLockSig& clsig, const CBlockIndex* pindexScan, const uint256& idIn, std::pair<int, CQuorumCPtr>& ret);
    bool VerifyAggregatedChainLock(const CChainLockSig& clsig, const CBlockIndex* pindexScan);

    void Cleanup();
};

extern CChainLocksHandler* chainLocksHandler;

bool AreChainLocksEnabled();
bool AreMultiQuorumChainLocksEnabled();

} // namespace llmq

#endif // BITCOIN_LLMQ_QUORUMS_CHAINLOCKS_H
