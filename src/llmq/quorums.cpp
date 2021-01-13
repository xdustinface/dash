// Copyright (c) 2018-2019 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/quorums.h>
#include <llmq/quorums_blockprocessor.h>
#include <llmq/quorums_dkgsession.h>
#include <llmq/quorums_dkgsessionmgr.h>
#include <llmq/quorums_init.h>
#include <llmq/quorums_utils.h>

#include <evo/specialtx.h>

#include <masternode/activemasternode.h>
#include <chainparams.h>
#include <init.h>
#include <masternode/masternode-sync.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <univalue.h>
#include <validation.h>

#include <cxxtimer.hpp>

namespace llmq
{

static const std::string DB_QUORUM_SK_SHARE = "q_Qsk";
static const std::string DB_QUORUM_QUORUM_VVEC = "q_Qqvvec";

CQuorumManager* quorumManager;

static uint256 MakeQuorumKey(const CQuorum& q)
{
    CHashWriter hw(SER_NETWORK, 0);
    hw << q.params.type;
    hw << q.qc.quorumHash;
    for (const auto& dmn : q.members) {
        hw << dmn->proTxHash;
    }
    return hw.GetHash();
}

class CQuorumDataRequest
{
    Consensus::LLMQType llmqType;
    uint256 quorumHash;
    uint16_t nDataMask;
    uint256 proTxHash;

    int64_t nTime;
    static const int64_t nExpirySeconds{300};

    bool fProcessed;

public:
    CQuorumDataRequest() : nTime(GetAdjustedTime()) {}
    CQuorumDataRequest(const Consensus::LLMQType llmqTypeIn, const uint256& quorumHashIn, const uint16_t nDataMaskIn, const uint256& proTxHashIn = uint256()) :
        llmqType(llmqTypeIn),
        quorumHash(quorumHashIn),
        nDataMask(nDataMaskIn),
        proTxHash(proTxHashIn),
        nTime(GetAdjustedTime()),
        fProcessed(false) {}

    ADD_SERIALIZE_METHODS

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(llmqType);
        READWRITE(quorumHash);
        READWRITE(nDataMask);
        READWRITE(proTxHash);
    }

    const Consensus::LLMQType GetLLMQType() const { return llmqType; }
    const uint256& GetQuorumHash() const { return quorumHash; }
    const uint16_t GetDataMask() const { return nDataMask; }
    const uint256& GetProTxHash() const { return proTxHash; }

    bool IsExpired() const
    {
        return (GetAdjustedTime() - nTime) >= nExpirySeconds;
    }

    bool IsProcessed() const
    {
        return fProcessed;
    }

    void SetProcessed()
    {
        fProcessed = true;
    }

    bool operator==(const CQuorumDataRequest& other)
    {
        return llmqType == other.llmqType &&
               quorumHash == other.quorumHash &&
               nDataMask == other.nDataMask &&
               proTxHash == other.proTxHash;
    }
    bool operator!=(const CQuorumDataRequest& other)
    {
        return !(*this == other);
    }
};

CQuorum::~CQuorum()
{
    // most likely the thread is already done
    stopCachePopulatorThread = true;
    // watch out to not join the thread when we're called from inside the thread, which might happen on shutdown. This
    // is because on shutdown the thread is the last owner of the shared CQuorum instance and thus the destroyer of it.
    if (cachePopulatorThread.joinable() && cachePopulatorThread.get_id() != std::this_thread::get_id()) {
        cachePopulatorThread.join();
    }
}

void CQuorum::Init(const CFinalCommitment& _qc, const CBlockIndex* _pindexQuorum, const uint256& _minedBlockHash, const std::vector<CDeterministicMNCPtr>& _members)
{
    qc = _qc;
    pindexQuorum = _pindexQuorum;
    members = _members;
    minedBlockHash = _minedBlockHash;
}

bool CQuorum::SetVerificationVector(const BLSVerificationVector& quorumVecIn)
{
    auto nHashIn = ::SerializeHash(quorumVecIn);
    if (nHashIn != qc.quorumVvecHash) {
        return false;
    }

    if (quorumVvec == nullptr) {
        quorumVvec = std::make_shared<BLSVerificationVector>(quorumVecIn);
    }

    //CQuorum::StartCachePopulatorThread(std::shared_ptr<CQuorum>(this));

    return true;
}

bool CQuorum::SetSecretKeyShare(const CBLSSecretKey& secretKeyShare)
{
    if (!secretKeyShare.IsValid()) {
        return false;
    }

    CBLSPublicKey publicKeyShareExpected = GetPubKeyShare(GetMemberIndex(activeMasternodeInfo.proTxHash));

    if (secretKeyShare.GetPublicKey() != publicKeyShareExpected) {
        return false;
    }

    skShare = secretKeyShare;

    return true;
}

bool CQuorum::IsMember(const uint256& proTxHash) const
{
    for (auto& dmn : members) {
        if (dmn->proTxHash == proTxHash) {
            return true;
        }
    }
    return false;
}

bool CQuorum::IsValidMember(const uint256& proTxHash) const
{
    for (size_t i = 0; i < members.size(); i++) {
        if (members[i]->proTxHash == proTxHash) {
            return qc.validMembers[i];
        }
    }
    return false;
}

CBLSPublicKey CQuorum::GetPubKeyShare(size_t memberIdx) const
{
    if (quorumVvec == nullptr || memberIdx >= members.size() || !qc.validMembers[memberIdx]) {
        return CBLSPublicKey();
    }
    auto& m = members[memberIdx];
    return blsCache.BuildPubKeyShare(m->proTxHash, quorumVvec, CBLSId(m->proTxHash));
}

CBLSSecretKey CQuorum::GetSkShare() const
{
    return skShare;
}

int CQuorum::GetMemberIndex(const uint256& proTxHash) const
{
    for (size_t i = 0; i < members.size(); i++) {
        if (members[i]->proTxHash == proTxHash) {
            return (int)i;
        }
    }
    return -1;
}

void CQuorum::WriteContributions(CEvoDB& evoDb)
{
    uint256 dbKey = MakeQuorumKey(*this);

    if (quorumVvec != nullptr) {
        evoDb.GetRawDB().Write(std::make_pair(DB_QUORUM_QUORUM_VVEC, dbKey), *quorumVvec);
    }
    if (skShare.IsValid()) {
        evoDb.GetRawDB().Write(std::make_pair(DB_QUORUM_SK_SHARE, dbKey), skShare);
    }
}

bool CQuorum::ReadContributions(CEvoDB& evoDb)
{
    uint256 dbKey = MakeQuorumKey(*this);

    BLSVerificationVector qv;
    if (evoDb.Read(std::make_pair(DB_QUORUM_QUORUM_VVEC, dbKey), qv)) {
        quorumVvec = std::make_shared<BLSVerificationVector>(std::move(qv));
    } else {
        return false;
    }

    // We ignore the return value here as it is ok if this fails. If it fails, it usually means that we are not a
    // member of the quorum but observed the whole DKG process to have the quorum verification vector.
    evoDb.Read(std::make_pair(DB_QUORUM_SK_SHARE, dbKey), skShare);

    return true;
}

void CQuorum::StartCachePopulatorThread(std::shared_ptr<CQuorum> _this)
{
    if (_this->quorumVvec == nullptr) {
        return;
    }

    cxxtimer::Timer t(true);
    LogPrint(BCLog::LLMQ, "CQuorum::StartCachePopulatorThread -- start\n");

    // this thread will exit after some time
    // when then later some other thread tries to get keys, it will be much faster
    _this->cachePopulatorThread = std::thread([_this, t]() {
        RenameThread("dash-q-cachepop");
        for (size_t i = 0; i < _this->members.size() && !_this->stopCachePopulatorThread && !ShutdownRequested(); i++) {
            if (_this->qc.validMembers[i]) {
                _this->GetPubKeyShare(i);
            }
        }
        LogPrint(BCLog::LLMQ, "CQuorum::StartCachePopulatorThread -- done. time=%d\n", t.count());
    });
}

CQuorumManager::CQuorumManager(CEvoDB& _evoDb, CBLSWorker& _blsWorker, CDKGSessionManager& _dkgManager) :
    evoDb(_evoDb),
    blsWorker(_blsWorker),
    dkgManager(_dkgManager)
{
}

void CQuorumManager::UpdatedBlockTip(const CBlockIndex* pindexNew, bool fInitialDownload)
{
    if (!masternodeSync.IsBlockchainSynced()) {
        return;
    }

    for (auto& p : Params().GetConsensus().llmqs) {
        EnsureQuorumConnections(p.first, pindexNew);
    }
}

void CQuorumManager::EnsureQuorumConnections(Consensus::LLMQType llmqType, const CBlockIndex* pindexNew)
{
    const auto& params = Params().GetConsensus().llmqs.at(llmqType);

    auto myProTxHash = activeMasternodeInfo.proTxHash;
    auto lastQuorums = ScanQuorums(llmqType, pindexNew, (size_t)params.keepOldConnections);

    auto connmanQuorumsToDelete = g_connman->GetMasternodeQuorums(llmqType);

    // don't remove connections for the currently in-progress DKG round
    int curDkgHeight = pindexNew->nHeight - (pindexNew->nHeight % params.dkgInterval);
    auto curDkgBlock = pindexNew->GetAncestor(curDkgHeight)->GetBlockHash();
    connmanQuorumsToDelete.erase(curDkgBlock);

    bool allowWatch = gArgs.GetBoolArg("-watchquorums", DEFAULT_WATCH_QUORUMS);
    for (auto& quorum : lastQuorums) {
        if (!quorum->IsMember(myProTxHash) && !allowWatch) {
            continue;
        }

        CLLMQUtils::EnsureQuorumConnections(llmqType, quorum->pindexQuorum, myProTxHash, allowWatch);

        connmanQuorumsToDelete.erase(quorum->qc.quorumHash);
    }

    for (auto& qh : connmanQuorumsToDelete) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- removing masternodes quorum connections for quorum %s:\n", __func__, qh.ToString());
        g_connman->RemoveMasternodeQuorumNodes(llmqType, qh);
    }
}

bool CQuorumManager::BuildQuorumFromCommitment(const CFinalCommitment& qc, const CBlockIndex* pindexQuorum, const uint256& minedBlockHash, std::shared_ptr<CQuorum>& quorum) const
{
    assert(pindexQuorum);
    assert(qc.quorumHash == pindexQuorum->GetBlockHash());

    auto members = CLLMQUtils::GetAllQuorumMembers((Consensus::LLMQType)qc.llmqType, pindexQuorum);

    quorum->Init(qc, pindexQuorum, minedBlockHash, members);

    bool hasValidVvec = false;
    if (quorum->ReadContributions(evoDb)) {
        hasValidVvec = true;
    } else {
        if (BuildQuorumContributions(qc, quorum)) {
            quorum->WriteContributions(evoDb);
            hasValidVvec = true;
        } else {
            LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- quorum.ReadContributions and BuildQuorumContributions for block %s failed\n", __func__, qc.quorumHash.ToString());
        }
    }

    if (hasValidVvec) {
        // pre-populate caches in the background
        // recovering public key shares is quite expensive and would result in serious lags for the first few signing
        // sessions if the shares would be calculated on-demand
        CQuorum::StartCachePopulatorThread(quorum);
    }

    return true;
}

bool CQuorumManager::BuildQuorumContributions(const CFinalCommitment& fqc, std::shared_ptr<CQuorum>& quorum) const
{
    std::vector<uint16_t> memberIndexes;
    std::vector<BLSVerificationVectorPtr> vvecs;
    BLSSecretKeyVector skContributions;
    if (!dkgManager.GetVerifiedContributions((Consensus::LLMQType)fqc.llmqType, quorum->pindexQuorum, fqc.validMembers, memberIndexes, vvecs, skContributions)) {
        return false;
    }

    BLSVerificationVectorPtr quorumVvec;
    CBLSSecretKey skShare;

    cxxtimer::Timer t2(true);
    quorumVvec = blsWorker.BuildQuorumVerificationVector(vvecs);
    if (quorumVvec == nullptr) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- failed to build quorumVvec\n", __func__);
        // without the quorum vvec, there can't be a skShare, so we fail here. Failure is not fatal here, as it still
        // allows to use the quorum as a non-member (verification through the quorum pub key)
        return false;
    }
    skShare = blsWorker.AggregateSecretKeys(skContributions);
    if (!skShare.IsValid()) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- failed to build skShare\n", __func__);
        // We don't bail out here as this is not a fatal error and still allows us to recover public key shares (as we
        // have a valid quorum vvec at this point)
    }
    t2.stop();

    LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- built quorum vvec and skShare. time=%d\n", __func__, t2.count());

    quorum->quorumVvec = quorumVvec;
    quorum->skShare = skShare;

    return true;
}

bool CQuorumManager::HasQuorum(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    return quorumBlockProcessor->HasMinedCommitment(llmqType, quorumHash);
}

std::vector<CQuorumCPtr> CQuorumManager::ScanQuorums(Consensus::LLMQType llmqType, size_t maxCount)
{
    const CBlockIndex* pindex;
    {
        LOCK(cs_main);
        pindex = chainActive.Tip();
    }
    return ScanQuorums(llmqType, pindex, maxCount);
}

std::vector<CQuorumCPtr> CQuorumManager::ScanQuorums(Consensus::LLMQType llmqType, const CBlockIndex* pindexStart, size_t maxCount)
{
    auto& params = Params().GetConsensus().llmqs.at(llmqType);

    auto cacheKey = std::make_pair(llmqType, pindexStart->GetBlockHash());
    const size_t cacheMaxSize = params.signingActiveQuorumCount + 1;

    std::vector<CQuorumCPtr> result;

    if (maxCount <= cacheMaxSize) {
        LOCK(quorumsCacheCs);
        if (scanQuorumsCache.get(cacheKey, result)) {
            if (result.size() > maxCount) {
                result.resize(maxCount);
            }
            return result;
        }
    }

    bool storeCache = false;
    size_t maxCount2 = maxCount;
    if (maxCount2 <= cacheMaxSize) {
        maxCount2 = cacheMaxSize;
        storeCache = true;
    }

    auto quorumIndexes = quorumBlockProcessor->GetMinedCommitmentsUntilBlock(params.type, pindexStart, maxCount2);
    result.reserve(quorumIndexes.size());

    for (auto& quorumIndex : quorumIndexes) {
        assert(quorumIndex);
        auto quorum = GetQuorum(params.type, quorumIndex);
        assert(quorum != nullptr);
        result.emplace_back(quorum);
    }

    if (storeCache) {
        LOCK(quorumsCacheCs);
        scanQuorumsCache.insert(cacheKey, result);
    }

    if (result.size() > maxCount) {
        result.resize(maxCount);
    }

    return result;
}

CQuorumCPtr CQuorumManager::GetQuorum(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    CBlockIndex* pindexQuorum;
    {
        LOCK(cs_main);

        pindexQuorum = LookupBlockIndex(quorumHash);
        if (!pindexQuorum) {
            LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- block %s not found\n", __func__, quorumHash.ToString());
            return nullptr;
        }
    }
    return GetQuorum(llmqType, pindexQuorum);
}

CQuorumCPtr CQuorumManager::GetQuorum(Consensus::LLMQType llmqType, const CBlockIndex* pindexQuorum)
{
    assert(pindexQuorum);

    auto quorumHash = pindexQuorum->GetBlockHash();

    // we must check this before we look into the cache. Reorgs might have happened which would mean we might have
    // cached quorums which are not in the active chain anymore
    if (!HasQuorum(llmqType, quorumHash)) {
        return nullptr;
    }

    LOCK(quorumsCacheCs);

    auto it = quorumsCache.find(std::make_pair(llmqType, quorumHash));
    if (it != quorumsCache.end()) {
        return it->second;
    }

    CFinalCommitment qc;
    uint256 minedBlockHash;
    if (!quorumBlockProcessor->GetMinedCommitment(llmqType, quorumHash, qc, minedBlockHash)) {
        return nullptr;
    }

    auto& params = Params().GetConsensus().llmqs.at(llmqType);

    auto quorum = std::make_shared<CQuorum>(params, blsWorker);

    if (!BuildQuorumFromCommitment(qc, pindexQuorum, minedBlockHash, quorum)) {
        return nullptr;
    }

    quorumsCache.emplace(std::make_pair(llmqType, quorumHash), quorum);

    return quorum;
}

void CQuorumManager::ProcessMessage(CNode* pFrom, const std::string& strCommand, CDataStream& vRecv)
{
    auto strFunc = __func__;
    auto error = [&](const std::string strError, bool fIncreaseScore = true, int nScore = 10) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- %s: %s, from peer=%d\n", strFunc, strCommand, strError, pFrom->GetId());
        if (fIncreaseScore) {
            LOCK(cs_main);
            Misbehaving(pFrom->GetId(), nScore);
        }
    };

    if (strCommand == NetMsgType::QGETDATA) {

        CQuorumDataRequest request;
        vRecv >> request;

        auto sendQDATA = [&](uint8_t nError, const CDataStream& body = CDataStream(SER_NETWORK, PROTOCOL_VERSION)) {
            CDataStream ssResponse(SER_NETWORK, pFrom->GetSendVersion());
            ssResponse << request;
            ssResponse << nError;
            ssResponse << body;
            CNetMsgMaker msgMaker(pFrom->GetSendVersion());
            g_connman->PushMessage(pFrom, msgMaker.Make(NetMsgType::QDATA, ssResponse));
        };

        if (Params().GetConsensus().llmqs.count(request.GetLLMQType()) == 0) {
            sendQDATA(llmq::QuorumData::QUORUM_TYPE_INVALID);
            return;
        }

        const CBlockIndex* pQuorumIndex = LookupBlockIndex(request.GetQuorumHash());
        if (pQuorumIndex == nullptr) {
            sendQDATA(llmq::QuorumData::QUORUM_BLOCK_NOT_FOUND);
            return;
        }

        const CQuorumCPtr pQuorum = GetQuorum(request.GetLLMQType(), pQuorumIndex);
        if (pQuorum == nullptr) {
            sendQDATA(llmq::QuorumData::QUORUM_NOT_FOUND);
            return;
        }

        CDataStream ssData(SER_NETWORK, pFrom->GetSendVersion());

        if (request.GetDataMask() & llmq::QuorumData::QUORUM_VERIFICATION_VECTOR) {

            if (!pQuorum->quorumVvec) {
                sendQDATA(llmq::QuorumData::QUORUM_VERIFICATION_VECTOR_MISSING);
                return;
            }

            ssData << *pQuorum->quorumVvec;
        }

        if (request.GetDataMask() & llmq::QuorumData::ENCRYPTED_CONTRIBUTIONS) {

            int memberIdx = pQuorum->GetMemberIndex(request.GetProTxHash());
            if (memberIdx == -1) {
                sendQDATA(llmq::QuorumData::MASTERNODE_IS_NO_MEMBER);
                return;
            }

            std::vector<CBLSIESEncryptedObject<CBLSSecretKey>> vecEncrypted;
            if (!quorumDKGSessionManager->GetEncryptedContributions(request.GetLLMQType(), pQuorumIndex, pQuorum->qc.validMembers, request.GetProTxHash(), vecEncrypted)) {
                sendQDATA(llmq::QuorumData::ENCRYPTED_CONTRIBUTIONS_MISSING);
                return;
            }

            ssData << vecEncrypted;
        }

        sendQDATA(llmq::QuorumData::NO_ERROR, ssData);
        return;
    }

    if (strCommand == NetMsgType::QDATA) {

        CQuorumDataRequest request;
        uint8_t nError;

        vRecv >> request;
        vRecv >> nError;

        const CBlockIndex* pQuorumIndex = LookupBlockIndex(request.GetQuorumHash());
        if (pQuorumIndex == nullptr) {
            error("Quorum block not found");
            return;
        }

        LOCK(quorumsCacheCs);
        auto itQuorum = quorumsCache.find(std::make_pair(request.GetLLMQType(), request.GetQuorumHash()));
        if (itQuorum == quorumsCache.end()) {
            error("Quorum not found");
            return;
        }
        CQuorumPtr pQuorum = itQuorum->second;

        if (request.GetDataMask() & llmq::QuorumData::QUORUM_VERIFICATION_VECTOR) {

            BLSVerificationVector verficationVector;
            vRecv >> verficationVector;

            if (!pQuorum->SetVerificationVector(verficationVector)) {
                error("Invalid quorum verification vector");
                return;
            }
        }

        if (request.GetDataMask() & llmq::QuorumData::ENCRYPTED_CONTRIBUTIONS) {

            if (pQuorum->quorumVvec->size() != pQuorum->params.threshold) {
                error("No valid quorum verification vector available", false);
                return;
            }

            int memberIdx = pQuorum->GetMemberIndex(request.GetProTxHash());
            if (memberIdx == -1) {
                error("Not a member of the quorum");
                return;
            }

            std::vector<CBLSIESEncryptedObject<CBLSSecretKey>> vecEncrypted;
            vRecv >> vecEncrypted;

            BLSSecretKeyVector vecSecretKeys;
            vecSecretKeys.resize(vecEncrypted.size());
            for (size_t i = 0; i < vecEncrypted.size(); ++i) {
                if (!vecEncrypted[i].Decrypt(*activeMasternodeInfo.blsKeyOperator, vecSecretKeys[i], PROTOCOL_VERSION)) {
                    error("Failed to decrypt");
                    return;
                }
            }

            CBLSSecretKey secretKeyShare = blsWorker.AggregateSecretKeys(vecSecretKeys);
            if (!pQuorum->SetSecretKeyShare(secretKeyShare)) {
                error("Invalid secret key share received");
                return;
            }
        }
        pQuorum->WriteContributions(evoDb);
        return;
    }
}

} // namespace llmq
