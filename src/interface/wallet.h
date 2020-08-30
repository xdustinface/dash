// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACE_WALLET_H
#define BITCOIN_INTERFACE_WALLET_H

#include <amount.h>                    // For CAmount
#include <fs.h>                        // For fs::path
#include <script/ismine.h>             // For isminefilter, isminetype
#include <script/standard.h>           // For CTxDestination
#include <support/allocators/secure.h> // For SecureString
#include <ui_interface.h>              // For ChangeType

#include <functional>
#include <map>
#include <memory>
#include <stdint.h>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class CCoinControl;
class CKey;
class CWallet;
struct CRecipient;

namespace interface {

class Handler;
class PendingWalletTx;
struct WalletBalances;
struct WalletTxOut;

using WalletOrderForm = std::vector<std::pair<std::string, std::string>>;
using WalletValueMap = std::map<std::string, std::string>;

//! Interface for accessing a wallet.
class Wallet
{
public:
    virtual ~Wallet() {}

    //! Encrypt wallet.
    virtual bool encryptWallet(const SecureString& wallet_passphrase) = 0;

    //! Return whether wallet is encrypted.
    virtual bool isCrypted() = 0;

    //! Lock wallet.
    virtual bool lock(bool fAllowMixing = false) = 0;

    //! Unlock wallet.
    virtual bool unlock(const SecureString& wallet_passphrase, bool fAllowMixing = false) = 0;

    //! Return whether wallet is locked.
    virtual bool isLocked(bool fForMixing = false) = 0;

    //! Change wallet passphrase.
    virtual bool changeWalletPassphrase(const SecureString& old_wallet_passphrase,
        const SecureString& new_wallet_passphrase) = 0;

    //! Back up wallet.
    virtual bool backupWallet(const std::string& filename) = 0;

    //! Automatically backup up wallet.
    virtual bool autoBackupWallet(const fs::path& wallet_path, std::string& strBackupWarningRet, std::string& strBackupErrorRet) = 0;

    //! Get the number of keys since the last auto backup
    virtual int64_t getKeysLeftSinceAutoBackup() = 0;

    //! Get wallet name.
    virtual std::string getWalletName() = 0;

    //! Get public key.
    virtual bool getPubKey(const CKeyID& address, CPubKey& pub_key) = 0;

    //! Get private key.
    virtual bool getPrivKey(const CKeyID& address, CKey& key) = 0;

    //! Return whether wallet has private key.
    virtual bool isSpendable(const CTxDestination& dest) = 0;

    //! Return whether wallet has watch only keys.
    virtual bool haveWatchOnly() = 0;

    //! Add or update address.
    virtual bool setAddressBook(const CTxDestination& dest, const std::string& name, const std::string& purpose) = 0;

    //! Look up address in wallet, return whether exists.
    virtual bool getAddress(const CTxDestination& dest,
        std::string* name = nullptr,
        isminetype* is_mine = nullptr) = 0;

    //! Add dest data.
    virtual bool addDestData(const CTxDestination& dest, const std::string& key, const std::string& value) = 0;

    //! Erase dest data.
    virtual bool eraseDestData(const CTxDestination& dest, const std::string& key) = 0;

    //! Get dest values with prefix.
    virtual std::vector<std::string> getDestValues(const std::string& prefix) = 0;

    //! Lock coin.
    virtual void lockCoin(const COutPoint& output) = 0;

    //! Unlock coin.
    virtual void unlockCoin(const COutPoint& output) = 0;

    //! Return whether coin is locked.
    virtual bool isLockedCoin(const COutPoint& output) = 0;

    //! List locked coins.
    virtual void listLockedCoins(std::vector<COutPoint>& outputs) = 0;

    //! List protx coins.
    virtual void listProTxCoins(std::vector<COutPoint>& vOutpts) = 0;

    //! Create transaction.
    virtual std::unique_ptr<PendingWalletTx> createTransaction(const std::vector<CRecipient>& recipients,
        const CCoinControl& coin_control,
        bool sign,
        int& change_pos,
        CAmount& fee,
        std::string& fail_reason) = 0;

    //! Return whether transaction can be abandoned.
    virtual bool transactionCanBeAbandoned(const uint256& txid) = 0;

    //! Abandon transaction.
    virtual bool abandonTransaction(const uint256& txid) = 0;

    // Get the number of privatesend rounds an output went through
    virtual int getRealOutpointPrivateSendRounds(const COutPoint& outpoint) = 0;

    // Check if an outpoint is fully mixed
    virtual bool isFullyMixed(const COutPoint& outpoint) = 0;

    //! Get balances.
    virtual WalletBalances getBalances() = 0;

    //! Get balances if possible without blocking.
    virtual bool tryGetBalances(WalletBalances& balances, int& num_blocks) = 0;

    //! Get balance.
    virtual CAmount getBalance() = 0;

    //! Get anonymizable balance.
    virtual CAmount getAnonymizableBalance(bool fSkipDenominated, bool fSkipUnconfirmed) = 0;

    //! Get anonymized balance.
    virtual CAmount getAnonymizedBalance() = 0;

    //! Get denominated balance.
    virtual CAmount getDenominatedBalance(bool unconfirmed) = 0;

    //! Get normalized anonymized balance.
    virtual CAmount getNormalizedAnonymizedBalance() = 0;

    //! Get average anonymized rounds.
    virtual CAmount getAverageAnonymizedRounds() = 0;

    //! Get available balance.
    virtual CAmount getAvailableBalance(const CCoinControl& coin_control) = 0;

    //! Return AvailableCoins + LockedCoins grouped by wallet address.
    //! (put change in one group with wallet address)
    using CoinsList = std::map<CTxDestination, std::vector<std::tuple<COutPoint, WalletTxOut>>>;
    virtual CoinsList listCoins() = 0;

    //! Return wallet transaction output information.
    virtual std::vector<WalletTxOut> getCoins(const std::vector<COutPoint>& outputs) = 0;

    // Return whether HD enabled.
    virtual bool hdEnabled() = 0;

    //! Register handler for show progress messages.
    using ShowProgressFn = std::function<void(const std::string& title, int progress)>;
    virtual std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) = 0;

    //! Register handler for status changed messages.
    using StatusChangedFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleStatusChanged(StatusChangedFn fn) = 0;

    //! Register handler for address book changed messages.
    using AddressBookChangedFn = std::function<void(const CTxDestination& address,
        const std::string& label,
        bool is_mine,
        const std::string& purpose,
        ChangeType status)>;
    virtual std::unique_ptr<Handler> handleAddressBookChanged(AddressBookChangedFn fn) = 0;

    //! Register handler for transaction changed messages.
    using TransactionChangedFn = std::function<void(const uint256& txid, ChangeType status)>;
    virtual std::unique_ptr<Handler> handleTransactionChanged(TransactionChangedFn fn) = 0;

    //! Register handler for instantlock messages.
    using InstantLockReceivedFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleInstantLockReceived(InstantLockReceivedFn fn) = 0;

    //! Register handler for chainlock messages.
    using ChainLockReceivedFn = std::function<void(int chainLockHeight)>;
    virtual std::unique_ptr<Handler> handleChainLockReceived(ChainLockReceivedFn fn) = 0;

    //! Register handler for watchonly changed messages.
    using WatchOnlyChangedFn = std::function<void(bool have_watch_only)>;
    virtual std::unique_ptr<Handler> handleWatchOnlyChanged(WatchOnlyChangedFn fn) = 0;
};

//! Tracking object returned by CreateTransaction and passed to CommitTransaction.
class PendingWalletTx
{
public:
    virtual ~PendingWalletTx() {}

    //! Get transaction data.
    virtual const CTransaction& get() = 0;

    //! Send pending transaction and commit to wallet.
    virtual bool commit(WalletValueMap value_map,
        WalletOrderForm order_form,
        std::string from_account,
        std::string& reject_reason) = 0;
};

//! Collection of wallet balances.
struct WalletBalances
{
    CAmount balance = 0;
    CAmount unconfirmed_balance = 0;
    CAmount immature_balance = 0;
    CAmount anonymized_balance = 0;
    bool have_watch_only = false;
    CAmount watch_only_balance = 0;
    CAmount unconfirmed_watch_only_balance = 0;
    CAmount immature_watch_only_balance = 0;

    bool balanceChanged(const WalletBalances& prev) const
    {
        return balance != prev.balance || unconfirmed_balance != prev.unconfirmed_balance ||  anonymized_balance != prev.anonymized_balance ||
               immature_balance != prev.immature_balance || watch_only_balance != prev.watch_only_balance ||
               unconfirmed_watch_only_balance != prev.unconfirmed_watch_only_balance ||
               immature_watch_only_balance != prev.immature_watch_only_balance;
    }
};

//! Wallet transaction output.
struct WalletTxOut
{
    CTxOut txout;
    int64_t time;
    int depth_in_main_chain = -1;
    bool is_spent = false;
};

//! Return implementation of Wallet interface. This function will be undefined
//! in builds where ENABLE_WALLET is false.
std::unique_ptr<Wallet> MakeWallet(CWallet& wallet);

} // namespace interface

#endif // BITCOIN_INTERFACE_WALLET_H
