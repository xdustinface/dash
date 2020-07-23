// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/test/wallet_test_fixture.h>

#include <rpc/server.h>
#include <wallet/db.h>

WalletTestingSetup::WalletTestingSetup(const std::string& chainName):
    TestingSetup(chainName), m_wallet(std::make_shared<CWallet>("mock", WalletDatabase::CreateMock()))
{
    bool fFirstRun;
    CWallet::LoadWallet(m_wallet, fFirstRun);
    RegisterValidationInterface(m_wallet.get());

    RegisterWalletRPCCommands(tableRPC);
}

WalletTestingSetup::~WalletTestingSetup()
{
    UnregisterValidationInterface(m_wallet.get());
}
