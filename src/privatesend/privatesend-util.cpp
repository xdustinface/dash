// Copyright (c) 2014-2019 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <privatesend/privatesend-util.h>
#include <script/sign.h>
#include <validation.h>
#include <wallet/fees.h>

CKeyHolder::CKeyHolder(CWallet* pwallet) :
    reserveKey(pwallet)
{
    reserveKey.GetReservedKey(pubKey, false);
}

void CKeyHolder::KeepKey()
{
    reserveKey.KeepKey();
}

void CKeyHolder::ReturnKey()
{
    reserveKey.ReturnKey();
}

CScript CKeyHolder::GetScriptForDestination() const
{
    return ::GetScriptForDestination(pubKey.GetID());
}


CScript CKeyHolderStorage::AddKey(CWallet* pwallet)
{
    auto keyHolderPtr = std::unique_ptr<CKeyHolder>(new CKeyHolder(pwallet));
    auto script = keyHolderPtr->GetScriptForDestination();

    LOCK(cs_storage);
    storage.emplace_back(std::move(keyHolderPtr));
    LogPrintf("CKeyHolderStorage::%s -- storage size %lld\n", __func__, storage.size());
    return script;
}

void CKeyHolderStorage::KeepAll()
{
    std::vector<std::unique_ptr<CKeyHolder> > tmp;
    {
        // don't hold cs_storage while calling KeepKey(), which might lock cs_wallet
        LOCK(cs_storage);
        std::swap(storage, tmp);
    }

    if (!tmp.empty()) {
        for (auto& key : tmp) {
            key->KeepKey();
        }
        LogPrintf("CKeyHolderStorage::%s -- %lld keys kept\n", __func__, tmp.size());
    }
}

void CKeyHolderStorage::ReturnAll()
{
    std::vector<std::unique_ptr<CKeyHolder> > tmp;
    {
        // don't hold cs_storage while calling ReturnKey(), which might lock cs_wallet
        LOCK(cs_storage);
        std::swap(storage, tmp);
    }

    if (!tmp.empty()) {
        for (auto& key : tmp) {
            key->ReturnKey();
        }
        LogPrintf("CKeyHolderStorage::%s -- %lld keys returned\n", __func__, tmp.size());
    }
}

CTransactionBuilderOutput::CTransactionBuilderOutput(CTransactionBuilder* pTxBuilderIn, CWallet* pwalletIn, CAmount nAmountIn) :
    pTxBuilder(pTxBuilderIn),
    key(pwalletIn),
    nAmount(nAmountIn)
{
    assert(pTxBuilder);
    CPubKey pubKey;
    key.GetReservedKey(pubKey, false);
    script = ::GetScriptForDestination(pubKey.GetID());
}

bool CTransactionBuilderOutput::UpdateAmount(const CAmount nNewAmount)
{
    LOCK(pTxBuilder->cs_outputs);

    const CAmount nAmountDiff = nNewAmount - nAmount;

    if (nNewAmount > 0 && nAmountDiff <= pTxBuilder->GetAmountLeft()) {
        if (nAmountDiff != 0) {
            nAmount = nNewAmount;
        }
        return true;
    }
    return false;
}

CTransactionBuilder::CTransactionBuilder(CWallet* pwalletIn, const CompactTallyItem& tallyItemIn) :
    pwallet(pwalletIn),
    dummyReserveKey(pwalletIn),
    tallyItem(tallyItemIn)
{
    // Generate d feerate which will be used to consider if the remainder is dust and will go into fees or not
    coinControl.m_dust_feerate = ::GetDiscardRate(::feeEstimator);
    // Generate a feerate which will be used by calculations of this class and also by CWallet::CreateTransaction
    coinControl.m_feerate = ::feeEstimator.estimateSmartFee(::nTxConfirmTarget, nullptr, true);
    // Fee will always go back to origin
    coinControl.destChange = tallyItemIn.txdest;
    // Only allow tallyItems inputs for tx creation
    coinControl.fAllowOtherInputs = false;
    // Select all tallyItem outputs in the coinControl so that CreateTransaction knows what to use
    for (const auto& outpoint : tallyItem.vecOutPoints) {
        coinControl.Select(outpoint);
    }
    // Create dummy tx to calculate the exact required fees upfront for accurate amount and fee calculations
    CMutableTransaction dummyTx;
    // Get a comparable dummy scriptPubKey
    CTransactionBuilderOutput dummyOutput(this, pwallet, 0);
    CScript dummyScript = dummyOutput.GetScript();
    dummyOutput.ReturnKey();
    // And create dummy signatures for all inputs
    SignatureData dummySignature;
    ProduceSignature(DummySignatureCreator(pwallet), dummyScript, dummySignature);
    for (auto out : tallyItem.vecOutPoints) {
        dummyTx.vin.push_back(CTxIn(out, dummySignature.scriptSig));
    }
    // Calculate required bytes for the dummy tx with tallyItem's inputs only
    nBytesBase = ::GetSerializeSize(dummyTx, SER_NETWORK, PROTOCOL_VERSION);
    // Calculate the output size
    nBytesOutput = ::GetSerializeSize(CTxOut(0, dummyScript), SER_NETWORK, PROTOCOL_VERSION);
    // Just to make sure..
    Clear();
}

CTransactionBuilder::~CTransactionBuilder()
{
    Clear();
}

void CTransactionBuilder::Clear()
{
    std::vector<std::unique_ptr<CTransactionBuilderOutput>> vecOutputsTmp;
    {
        // Don't hold cs_outputs while clearing the outputs which might indirectly call lock cs_wallet
        LOCK(cs_outputs);
        std::swap(vecOutputs, vecOutputsTmp);
        vecOutputs.clear();
    }

    for (auto& key : vecOutputsTmp) {
        if (fKeepKeys) {
            key->KeepKey();
        } else {
            key->ReturnKey();
        }
    }
    // Always return this key just to make sure..
    dummyReserveKey.ReturnKey();
}

bool CTransactionBuilder::TryAddOutput(CAmount nAmount) const
{
    return GetAmountLeft(tallyItem.nAmount, GetAmountUsed() + nAmount, GetFee(GetBytesTotal() + nBytesOutput)) >= 0;
}

bool CTransactionBuilder::TryAddOutputs(const std::vector<CAmount>& vecAmounts) const
{
    CAmount nAmountSum{0};
    for (auto nAmount : vecAmounts) {
        nAmountSum += nAmount;
    }
    return GetAmountLeft(tallyItem.nAmount, GetAmountUsed() + nAmountSum, GetFee(GetBytesTotal() + nBytesOutput * vecAmounts.size())) >= 0;
}

CTransactionBuilderOutput* CTransactionBuilder::AddOutput(CAmount nAmount)
{
    LOCK(cs_outputs);
    if (TryAddOutput(nAmount)) {
        vecOutputs.push_back(std::make_unique<CTransactionBuilderOutput>(this, pwallet, nAmount));
        return vecOutputs.back().get();
    }
    return nullptr;
}

CAmount CTransactionBuilder::GetAmountLeft(const CAmount nAmount, const CAmount nAmountUsed, const CAmount nFee)
{
    return nAmount - nAmountUsed - nFee;
}

CAmount CTransactionBuilder::GetAmountUsed() const
{
    CAmount nAmountUsed{0};
    for (const auto& out : vecOutputs) {
        nAmountUsed += out->GetAmount();
    }
    return nAmountUsed;
}

CAmount CTransactionBuilder::GetFee(int nBytes) const
{
    CAmount nFeeCalc = coinControl.m_feerate->GetFee(nBytes);
    CAmount nRequiredFee = GetRequiredFee(nBytes);
    if (nRequiredFee > nFeeCalc) {
        nFeeCalc = nRequiredFee;
    }
    if (nFeeCalc > ::maxTxFee) {
        nFeeCalc = ::maxTxFee;
    }
    return nFeeCalc;
}

bool CTransactionBuilder::IsDust(CAmount nAmount) const
{
    return ::IsDust(CTxOut(nAmount, ::GetScriptForDestination(tallyItem.txdest)), coinControl.m_dust_feerate.get());
}

bool CTransactionBuilder::Commit(std::string& strResult)
{
    CWalletTx wtx;
    CAmount nFeeRet = 0;
    int nChangePosRet = -1;

    // Transfor the outputs to the format CWallet::CreateTransaction requires
    std::vector<CRecipient> vecSend;
    {
        LOCK(cs_outputs);
        vecSend.reserve(vecOutputs.size());
        for (const auto& out : vecOutputs) {
            vecSend.push_back((CRecipient){out->GetScript(), out->GetAmount(), false});
        }
    }

    if (!pwallet->CreateTransaction(vecSend, wtx, dummyReserveKey, nFeeRet, nChangePosRet, strResult, coinControl)) {
        return false;
    }

    CAmount nAmountLeft = GetAmountLeft();
    CAmount nFeeAdditional = nAmountLeft && IsDust(nAmountLeft) ? nAmountLeft : 0;
    int nBytesAdditional = !IsDust(nAmountLeft) ? nBytesOutput : 0;
    CAmount nFeeCalc = GetFee(GetBytesTotal() + nBytesAdditional) + nFeeAdditional;

    // If there is a either remainder which is considered to be dust (will be added to fee in this case) or no amount left there should be no change output, return if there is a change output.
    if (nChangePosRet != -1 && IsDust(nAmountLeft)) {
        strResult = strprintf("Unexpected change output %s at position %d", wtx.tx->vout[nChangePosRet].ToString(), nChangePosRet);
        return false;
    }

    // If there is a remainder which is not considered to be dust it should end up in a change output, return if not.
    if (!IsDust(nAmountLeft) && nChangePosRet == -1) {
        strResult = strprintf("Change output missing: %d", GetAmountLeft());
        return false;
    }

    // If the calculated fee does not match the fee returned by CreateTransaction aka if this check fails something is messed!
    if (nFeeRet != nFeeCalc) {
        strResult = strprintf("Fee validation failed -> nFeeRet: %d, nFeeCalc: %d, nFeeAdditional: %d, nBytesAdditional: %d, %s", nFeeRet, nFeeCalc, nFeeAdditional, nBytesAdditional, ToString());
        return false;
    }

    CValidationState state;
    if (!pwallet->CommitTransaction(wtx, dummyReserveKey, g_connman.get(), state)) {
        strResult = state.GetRejectReason();
        return false;
    }

    fKeepKeys = true;

    strResult = wtx.GetHash().ToString();

    return true;
}

std::string CTransactionBuilder::ToString()
{
    return strprintf("CTransactionBuilder(Amount left: %d, Bytes base: %d, Bytes output %d, Bytes total: %d, Amount used: %d, Outputs: %d, Fee rate: %d, Fee: %d)",
        GetAmountLeft(),
        nBytesBase,
        nBytesOutput,
        GetBytesTotal(),
        GetAmountUsed(),
        CountOutputs(),
        coinControl.m_feerate->GetFeePerK(),
        GetFee(GetBytesTotal()));
}
