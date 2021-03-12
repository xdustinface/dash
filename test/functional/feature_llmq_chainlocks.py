#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import struct
import time

from test_framework.blocktools import (create_block, create_coinbase)
from test_framework.messages import CInv, hash256, msg_clsig, msg_inv, ser_string, uint256_from_str
from test_framework.mininode import P2PInterface, network_thread_join, network_thread_start
from test_framework.test_framework import DashTestFramework
from test_framework.util import connect_nodes, hex_str_to_bytes, isolate_node

'''
feature_llmq_chainlocks.py

Checks LLMQs based ChainLocks

'''


class TestP2PConn(P2PInterface):
    def __init__(self):
        super().__init__()
        self.clsigs = {}

    def send_clsig(self, clsig):
        clhash = uint256_from_str(hash256(clsig.serialize()))
        self.clsigs[clhash] = clsig

        inv = msg_inv([CInv(29, clhash)])
        self.send_message(inv)

    def on_getdata(self, message):
        for inv in message.inv:
            if inv.hash in self.clsigs:
                self.send_message(self.clsigs[inv.hash])


class LLMQChainLocksTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(4, 3, fast_dip3_enforcement=True)

    def run_test(self):

        # Connect all nodes to node1 so that we always have the whole network connected
        # Otherwise only masternode connections will be established between nodes, which won't propagate TXs/blocks
        # Usually node0 is the one that does this, but in this test we isolate it multiple times
        for i in range(len(self.nodes)):
            if i != 1:
                connect_nodes(self.nodes[i], 1)

        self.activate_dip8()

        self.nodes[0].spork("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        self.log.info("Mining 4 quorums")
        for i in range(4):
            self.mine_quorum()

        self.test_steps(False)
        self.test_steps(True)

    def test_steps(self, multi_quorum):
        self.bump_mocktime(1)
        if multi_quorum:
            self.log.info("Testing multi-quorum ChainLocks")
            self.nodes[0].spork("SPORK_19_CHAINLOCKS_ENABLED", 1)
            self.wait_for_sporks_same()
        else:
            self.log.info("Testing single-quorum ChainLocks")
            self.nodes[0].spork("SPORK_19_CHAINLOCKS_ENABLED", 0)
            self.wait_for_sporks_same()

        self.log.info("Mine single block, wait for chainlock")
        self.nodes[0].generate(1)
        self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash())

        self.log.info("Mine many blocks, wait for chainlock")
        self.nodes[0].generate(20)
        # We need more time here due to 20 blocks being generated at once
        self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash(), timeout=30)

        self.log.info("Assert that all blocks up until the tip are chainlocked")
        for h in range(1, self.nodes[0].getblockcount()):
            block = self.nodes[0].getblock(self.nodes[0].getblockhash(h))
            assert(block['chainlock'])

        self.log.info("Isolate node, mine on another, and reconnect")
        isolate_node(self.nodes[0])
        node0_mining_addr = self.nodes[0].getnewaddress()
        node0_tip = self.nodes[0].getbestblockhash()
        self.nodes[1].generatetoaddress(5, node0_mining_addr)
        self.wait_for_chainlocked_block(self.nodes[1], self.nodes[1].getbestblockhash())
        assert(self.nodes[0].getbestblockhash() == node0_tip)
        self.nodes[0].setnetworkactive(True)
        for i in range(self.num_nodes - 1):
            connect_nodes(self.nodes[0], i + 1)
        self.nodes[1].generatetoaddress(1, node0_mining_addr)
        self.wait_for_chainlocked_block(self.nodes[0], self.nodes[1].getbestblockhash())

        self.log.info("Isolate node, mine on both parts of the network, and reconnect")
        isolate_node(self.nodes[0])
        bad_tip = self.nodes[0].generate(5)[-1]
        self.nodes[1].generatetoaddress(1, node0_mining_addr)
        good_tip = self.nodes[1].getbestblockhash()
        self.wait_for_chainlocked_block(self.nodes[1], good_tip)
        assert(not self.nodes[0].getblock(self.nodes[0].getbestblockhash())["chainlock"])
        self.nodes[0].setnetworkactive(True)
        for i in range(self.num_nodes - 1):
            connect_nodes(self.nodes[0], i + 1)
        self.nodes[1].generatetoaddress(1, node0_mining_addr)
        self.wait_for_chainlocked_block(self.nodes[0], self.nodes[1].getbestblockhash())
        assert(self.nodes[0].getblock(self.nodes[0].getbestblockhash())["previousblockhash"] == good_tip)
        assert(self.nodes[1].getblock(self.nodes[1].getbestblockhash())["previousblockhash"] == good_tip)

        self.log.info("The tip mined while this node was isolated should be marked conflicting now")
        found = False
        for tip in self.nodes[0].getchaintips(2):
            if tip["hash"] == bad_tip:
                assert(tip["status"] == "conflicting")
                found = True
                break
        assert(found)

        self.log.info("Keep node connected and let it try to reorg the chain")
        good_tip = self.nodes[0].getbestblockhash()
        self.log.info("Restart it so that it forgets all the chainlock messages from the past")
        self.stop_node(0)
        self.start_node(0)
        connect_nodes(self.nodes[0], 1)
        assert(self.nodes[0].getbestblockhash() == good_tip)
        self.nodes[0].invalidateblock(good_tip)
        self.log.info("Now try to reorg the chain")
        self.nodes[0].generate(2)
        time.sleep(6)
        assert(self.nodes[1].getbestblockhash() == good_tip)
        bad_tip = self.nodes[0].generate(2)[-1]
        time.sleep(6)
        assert(self.nodes[0].getbestblockhash() == bad_tip)
        assert(self.nodes[1].getbestblockhash() == good_tip)

        self.log.info("Now let the node which is on the wrong chain reorg back to the locked chain")
        self.nodes[0].reconsiderblock(good_tip)
        assert(self.nodes[0].getbestblockhash() != good_tip)
        good_fork = good_tip
        good_tip = self.nodes[1].generatetoaddress(1, node0_mining_addr)[-1]  # this should mark bad_tip as conflicting
        self.wait_for_chainlocked_block(self.nodes[0], good_tip)
        assert(self.nodes[0].getbestblockhash() == good_tip)
        found = False
        for tip in self.nodes[0].getchaintips(2):
            if tip["hash"] == bad_tip:
                assert(tip["status"] == "conflicting")
                found = True
                break
        assert(found)

        self.log.info("Should switch to the best non-conflicting tip (not to the most work chain) on restart")
        assert(int(self.nodes[0].getblock(bad_tip)["chainwork"], 16) > int(self.nodes[1].getblock(good_tip)["chainwork"], 16))
        self.stop_node(0)
        self.start_node(0)
        self.nodes[0].invalidateblock(good_fork)
        self.stop_node(0)
        self.start_node(0)
        time.sleep(1)
        assert(self.nodes[0].getbestblockhash() == good_tip)

        self.log.info("Isolate a node and let it create some transactions which won't get IS locked")
        isolate_node(self.nodes[0])
        txs = []
        for i in range(3):
            txs.append(self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1))
        txs += self.create_chained_txs(self.nodes[0], 1)
        self.log.info("Assert that after block generation these TXs are NOT included (as they are \"unsafe\")")
        node0_tip = self.nodes[0].generate(1)[-1]
        for txid in txs:
            tx = self.nodes[0].getrawtransaction(txid, 1)
            assert("confirmations" not in tx)
        time.sleep(1)
        node0_tip_block = self.nodes[0].getblock(node0_tip)
        assert(not node0_tip_block["chainlock"])
        assert(node0_tip_block["previousblockhash"] == good_tip)
        self.log.info("Disable LLMQ based InstantSend for a very short time (this never gets propagated to other nodes)")
        self.nodes[0].spork("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.log.info("Now the TXs should be included")
        self.nodes[0].generate(1)
        self.nodes[0].spork("SPORK_2_INSTANTSEND_ENABLED", 0)
        self.log.info("Assert that TXs got included now")
        for txid in txs:
            tx = self.nodes[0].getrawtransaction(txid, 1)
            assert("confirmations" in tx and tx["confirmations"] > 0)
        # Enable network on first node again, which will cause the blocks to propagate and IS locks to happen retroactively
        # for the mined TXs, which will then allow the network to create a CLSIG
        self.log.info("Reenable network on first node and wait for chainlock")
        self.nodes[0].setnetworkactive(True)
        for i in range(self.num_nodes - 1):
            connect_nodes(self.nodes[0], i + 1)
        self.wait_for_chainlocked_block(self.nodes[0], self.nodes[0].getbestblockhash(), timeout=30)

        self.log.info("Send fake future clsigs and see if this breaks ChainLocks")
        SIGN_HEIGHT_OFFSET = 8
        p2p_node = self.nodes[0].add_p2p_connection(TestP2PConn())
        network_thread_start()
        p2p_node.wait_for_verack()
        if multi_quorum:
            self.log.info("Should accept fake clsig but other quorums should sign the actual block on the same height and override the malicious one")
        else:
            self.log.info("Should accept fake clsig but won't sign the same height twice (normally)")
        fake_clsig1, fake_block_hash1 = self.create_fake_clsig(1, multi_quorum)
        p2p_node.send_clsig(fake_clsig1)
        for node in self.nodes:
            self.wait_for_most_recent_chainlock(node, fake_block_hash1, timeout=5)
        tip = self.nodes[0].generate(1)[-1]
        if multi_quorum:
            # Honest nodes win
            self.wait_for_chainlocked_block_all_nodes(tip, timeout=5)
        else:
            # No new clsig for the same height
            time.sleep(5)
            for node in self.nodes:
                assert(self.nodes[0].getchainlocks()["recent_chainlock"]["blockhash"] != tip)
        self.log.info("Shouldn't accept fake clsig for 'tip + SIGN_HEIGHT_OFFSET + 1' block height")
        fake_clsig2, fake_block_hash2 = self.create_fake_clsig(SIGN_HEIGHT_OFFSET + 1, multi_quorum)
        p2p_node.send_clsig(fake_clsig2)
        if multi_quorum:
            time.sleep(5)
            for node in self.nodes:
                assert(node.getchainlocks()["recent_chainlock"]["blockhash"] == tip)
                assert(node.getchainlocks()["active_chainlock"]["blockhash"] == tip)
        else:
            # Note: fake_block_hash1 is a blockhash for the fake_clsig1 we accepted initially, not for the new fake_clsig2
            assert(self.nodes[0].getchainlocks()["recent_chainlock"]["blockhash"] == fake_block_hash1)
        self.log.info("Should accept fake clsig for 'tip + SIGN_HEIGHT_OFFSET' but new clsigs should still be formed")
        fake_clsig3, fake_block_hash3 = self.create_fake_clsig(SIGN_HEIGHT_OFFSET, multi_quorum)
        p2p_node.send_clsig(fake_clsig3)
        for node in self.nodes:
            self.wait_for_most_recent_chainlock(node, fake_block_hash3, timeout=5)
        tip = self.nodes[0].generate(1)[-1]
        self.wait_for_chainlocked_block_all_nodes(tip, timeout=5)
        self.nodes[0].disconnect_p2ps()
        network_thread_join()

    def create_fake_clsig(self, height_offset, multi_quorum):
        # create a fake block height_offset blocks ahead of the tip
        height = self.nodes[0].getblockcount() + height_offset
        fake_block = create_block(0xff, create_coinbase(height, dip4_activated=True))
        # create a fake clsig for that block
        quorum_hash = self.nodes[0].quorum('list', 1)["llmq_test"][0]
        request_id_buf = ser_string(b"clsig") + struct.pack("<I", height)
        if multi_quorum:
            request_id_buf += hex_str_to_bytes(quorum_hash)[::-1]
        request_id = hash256(request_id_buf)[::-1].hex()
        for mn in self.mninfo:
            mn.node.quorum('sign', 100, request_id, fake_block.hash, quorum_hash if multi_quorum else None)
        rec_sig = self.get_recovered_sig(request_id, fake_block.hash)
        fake_clsig = msg_clsig(height, fake_block.sha256, hex_str_to_bytes(rec_sig['sig']), [1,0,0,0] if multi_quorum else [])
        return fake_clsig, fake_block.hash

    def create_chained_txs(self, node, amount):
        txid = node.sendtoaddress(node.getnewaddress(), amount)
        tx = node.getrawtransaction(txid, 1)
        inputs = []
        valueIn = 0
        for txout in tx["vout"]:
            inputs.append({"txid": txid, "vout": txout["n"]})
            valueIn += txout["value"]
        outputs = {
            node.getnewaddress(): round(float(valueIn) - 0.0001, 6)
        }

        rawtx = node.createrawtransaction(inputs, outputs)
        rawtx = node.signrawtransactionwithwallet(rawtx)
        rawtxid = node.sendrawtransaction(rawtx["hex"])

        return [txid, rawtxid]


if __name__ == '__main__':
    LLMQChainLocksTest().main()
