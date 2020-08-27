// Copyright (c) 2020 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/test_dash.h>

#include <amount.h>
#include <consensus/validation.h>
#include <privatesend/privatesend-util.h>
#include <privatesend/privatesend.h>
#include <validation.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(privatesend_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(ps_collatoral_tests)
{
    // Good collateral values
    BOOST_CHECK(CPrivateSend::IsCollateralAmount(0.00010000 * COIN));
    BOOST_CHECK(CPrivateSend::IsCollateralAmount(0.00012345 * COIN));
    BOOST_CHECK(CPrivateSend::IsCollateralAmount(0.00032123 * COIN));
    BOOST_CHECK(CPrivateSend::IsCollateralAmount(0.00019000 * COIN));

    // Bad collateral values
    BOOST_CHECK(!CPrivateSend::IsCollateralAmount(0.00009999 * COIN));
    BOOST_CHECK(!CPrivateSend::IsCollateralAmount(0.00040001 * COIN));
    BOOST_CHECK(!CPrivateSend::IsCollateralAmount(0.00100000 * COIN));
    BOOST_CHECK(!CPrivateSend::IsCollateralAmount(0.00100001 * COIN));
}

class CTransactionBuilderTestSetup : public TestChain100Setup
{
public:
    CTransactionBuilderTestSetup()
    {
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        wallet = MakeUnique<CWallet>("mock", WalletDatabase::CreateMock());
        bool firstRun;
        wallet->LoadWallet(firstRun);
        AddWallet(wallet.get());
        {
            LOCK(wallet->cs_wallet);
            wallet->AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());
        }
        WalletRescanReserver reserver(wallet.get());
        reserver.reserve();
        wallet->ScanForWalletTransactions(chainActive.Genesis(), nullptr, reserver);
    }

    ~CTransactionBuilderTestSetup()
    {
        RemoveWallet(wallet.get());
    }

    std::unique_ptr<CWallet> wallet;

    CWalletTx& AddTxToChain(uint256 nTxHash)
    {
        assert(wallet->mapWallet.find(nTxHash) != wallet->mapWallet.end());
        CMutableTransaction blocktx;
        {
            LOCK(wallet->cs_wallet);
            blocktx = CMutableTransaction(*wallet->mapWallet.at(nTxHash).tx);
        }
        CreateAndProcessBlock({blocktx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        LOCK(cs_main);
        LOCK(wallet->cs_wallet);
        auto it = wallet->mapWallet.find(blocktx.GetHash());
        BOOST_CHECK(it != wallet->mapWallet.end());
        it->second.SetMerkleBranch(chainActive.Tip(), 1);
        return it->second;
    }
    CompactTallyItem GetTallyItem(const std::vector<CAmount>& vecAmounts)
    {
        CompactTallyItem tallyItem;
        CWalletTx tx;
        CReserveKey destKey(wallet.get());
        CReserveKey reserveKey(wallet.get());
        CAmount nFeeRet;
        int nChangePosRet = -1;
        std::string strError;
        CCoinControl coinControl;
        CPubKey pubKey;
        BOOST_CHECK(destKey.GetReservedKey(pubKey, false));
        tallyItem.txdest = pubKey.GetID();

        for (CAmount nAmount : vecAmounts) {
            BOOST_CHECK(wallet->CreateTransaction({{GetScriptForDestination(tallyItem.txdest), nAmount, false}}, tx, reserveKey, nFeeRet, nChangePosRet, strError, coinControl));
            CValidationState state;
            BOOST_CHECK(wallet->CommitTransaction(tx, reserveKey, nullptr, state));
            CWalletTx& wtx = AddTxToChain(tx.GetHash());
            for (size_t n = 0; n < wtx.tx->vout.size(); ++n) {
                if (nChangePosRet != -1 && n == nChangePosRet) {
                    // Skip the change output to only return the requested coins
                    continue;
                }
                tallyItem.vecOutPoints.push_back(COutPoint(wtx.GetHash(), n));
                tallyItem.nAmount += wtx.tx->vout[n].nValue;
            }
        }
        assert(tallyItem.vecOutPoints.size() == vecAmounts.size());
        destKey.KeepKey();
        return tallyItem;
    }
};

BOOST_FIXTURE_TEST_CASE(CTransactionBuilderTest, CTransactionBuilderTestSetup)
{
    minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);
    // Tests with single outpoint tallyItem
    {
        CompactTallyItem tallyItem = GetTallyItem({5000});
        CTransactionBuilder txBuilder(wallet.get(), tallyItem);

        BOOST_CHECK_EQUAL(txBuilder.CountOutputs(), 0);
        BOOST_CHECK_EQUAL(tallyItem.nAmount, txBuilder.GetAmountInitial());
        BOOST_CHECK_EQUAL(txBuilder.GetAmountLeft(), 4810);

        BOOST_CHECK(txBuilder.CouldAddOutput(4776));
        BOOST_CHECK(!txBuilder.CouldAddOutput(4777));

        BOOST_CHECK(txBuilder.CouldAddOutput(0));
        BOOST_CHECK(!txBuilder.CouldAddOutput(-1));

        BOOST_CHECK(txBuilder.CouldAddOutputs({1000, 1000, 2708}));
        BOOST_CHECK(!txBuilder.CouldAddOutputs({1000, 1000, 2709}));

        BOOST_CHECK_EQUAL(txBuilder.AddOutput(5000), nullptr);
        BOOST_CHECK_EQUAL(txBuilder.AddOutput(-1), nullptr);

        CTransactionBuilderOutput* output = txBuilder.AddOutput();
        BOOST_CHECK(output->UpdateAmount(txBuilder.GetAmountLeft()));
        BOOST_CHECK(output->UpdateAmount(1));
        BOOST_CHECK(output->UpdateAmount(output->GetAmount() + txBuilder.GetAmountLeft()));
        BOOST_CHECK(!output->UpdateAmount(output->GetAmount() + 1));
        BOOST_CHECK(!output->UpdateAmount(0));
        BOOST_CHECK(!output->UpdateAmount(-1));
        BOOST_CHECK_EQUAL(txBuilder.CountOutputs(), 1);

        std::string strResult;
        BOOST_CHECK(txBuilder.Commit(strResult));
        CWalletTx& wtx = AddTxToChain(uint256S(strResult));
        BOOST_CHECK_EQUAL(wtx.tx->vout.size(), txBuilder.CountOutputs()); // should have no change output
        BOOST_CHECK_EQUAL(wtx.tx->vout[0].nValue, output->GetAmount());
        BOOST_CHECK(wtx.tx->vout[0].scriptPubKey == output->GetScript());
    }
    // Tests with multiple outpoint tallyItem
    {
        CompactTallyItem tallyItem = GetTallyItem({10000, 20000, 30000, 40000, 50000});
        CTransactionBuilder txBuilder(wallet.get(), tallyItem);
        std::vector<CTransactionBuilderOutput*> vecOutputs;
        std::string strResult;

        vecOutputs.push_back(txBuilder.AddOutput(100));
        BOOST_CHECK(!txBuilder.Commit(strResult));

        vecOutputs.back()->UpdateAmount(1000);
        while (vecOutputs.size() < 100) {
            vecOutputs.push_back(txBuilder.AddOutput(1000 + vecOutputs.size()));
        }
        BOOST_CHECK_EQUAL(vecOutputs.size(), 100);
        BOOST_CHECK(txBuilder.Commit(strResult));
        CWalletTx& wtx = AddTxToChain(uint256S(strResult));
        BOOST_CHECK_EQUAL(wtx.tx->vout.size(), txBuilder.CountOutputs() + 1); // should have change output
        for (const auto& out : wtx.tx->vout) {
            auto it = std::find_if(vecOutputs.begin(), vecOutputs.end(), [&](CTransactionBuilderOutput* output) -> bool {
                return output->GetAmount() == out.nValue && output->GetScript() == out.scriptPubKey;
            });
            if (it != vecOutputs.end()) {
                vecOutputs.erase(it);
            } else {
                // change output
                BOOST_CHECK_EQUAL(txBuilder.GetAmountLeft() - 34, out.nValue);
            }
        }
        BOOST_CHECK(vecOutputs.size() == 0);
    }
}

BOOST_AUTO_TEST_SUITE_END()
