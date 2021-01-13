#!/usr/bin/env python3
# Copyright (c) 2021 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.mininode import *
from test_framework.test_framework import DashTestFramework
from test_framework.util import force_finish_mnsync, assert_equal, connect_nodes

'''
p2p_quorum_data.py

Tests QGETDATA/QDATA functionality
'''


def assert_qdata(qdata, qgetdata, error, len_vvec=0, len_contributions=0):
    assert qdata is not None and qgetdata is not None
    assert_equal(qdata.quorum_type, qgetdata.quorum_type)
    assert_equal(qdata.quorum_hash, qgetdata.quorum_hash)
    assert_equal(qdata.data_mask, qgetdata.data_mask)
    assert_equal(qdata.protx_hash, qgetdata.protx_hash)
    assert_equal(qdata.error, error)
    assert_equal(len(qdata.quorum_vvec), len_vvec)
    assert_equal(len(qdata.enc_contributions), len_contributions)

def assert_banscore(node, peer_id, expexted_score):
    score = None
    for peer in node.getpeerinfo():
        if peer["id"] == peer_id:
            score = peer["banscore"]
    assert_equal(score, expexted_score)


class QuorumDataInterface(P2PInterface):
    def __init__(self):
        super().__init__()

    def test_qgetdata(self, qgetdata, expected_error=0, len_vvec=0, len_contributions=0, response_expected=True):
        self.send_message(qgetdata)
        self.wait_for_qdata(message_expected=response_expected)
        if response_expected:
            assert_qdata(self.get_qdata(), qgetdata, expected_error, len_vvec, len_contributions)

    def get_qgetdata(self):
        return self.last_message["qdata"]

    def wait_for_qgetdata(self, timeout=3, message_expected=True):
        def test_function():
            return self.message_count["qgetdata"]
        wait_until(test_function, timeout=timeout, lock=mininode_lock, do_assert=message_expected)
        self.message_count["qgetdata"] = 0
        if not message_expected:
            assert (not self.message_count["qgetdata"])

    def get_qdata(self):
        return self.last_message["qdata"]

    def wait_for_qdata(self, timeout=3, message_expected=True):
        def test_function():
            return self.message_count["qdata"]
        wait_until(test_function, timeout=timeout, lock=mininode_lock, do_assert=message_expected)
        self.message_count["qdata"] = 0
        if not message_expected:
            assert (not self.message_count["qdata"])


class QuorumDataMessagesTest(DashTestFramework):
    def set_test_params(self):
        # add one node with extra arg to allow not requested qdata messages
        self.set_dash_test_params(4, 3, fast_dip3_enforcement=True)

    def run_test(self):

        def p2p_connect(node, mnauth=None, start_thread=False):
            p2p = node.add_p2p_connection(QuorumDataInterface())
            if start_thread:
                network_thread_start()
            p2p.wait_for_verack()
            p2p_node_id = node.getpeerinfo()[-1]["id"]
            if mnauth is not None:
                node.mnauth(p2p_node_id, mnauth.proTxHash, mnauth.pubKeyOperator)
            return p2p, p2p_node_id

        def restart_mn(mn, reindex=False):
            args = self.extra_args[mn.nodeIdx] + ['-masternodeblsprivkey=%s' % mn.keyOperator]
            if reindex:
                args.append('-reindex')
            self.restart_node(mn.nodeIdx, args)
            force_finish_mnsync(mn.node)
            connect_nodes(mn.node, 0)
            self.sync_all()

        def test_dkg_data(mn, p2p, quorum_type_in, quorum_hash_in):
            info = mn.node.quorum("info", quorum_type_in, quorum_hash_in, True)
            assert("secretKeyShare" in info)
            assert("members" in info)
            assert(len(info["members"]) > 0)
            valid = 0
            for member in info["members"]:
                if member["valid"]:
                    assert("pubKeyShare" in member)
                    valid += 1
            assert(valid > 0)

        def force_request_expire(bump_seconds=300):
            self.bump_mocktime(bump_seconds)
            if fullnode.getblockcount() % 2:  # Test with/without expired request cleanup
                fullnode.generate(1)
                self.sync_blocks()

        self.nodes[0].spork("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.nodes[0].spork("SPORK_19_CHAINLOCKS_ENABLED", 4070908800)

        self.wait_for_sporks_same()
        quorum_hash = self.mine_quorum()

        fullnode = self.nodes[0]
        mn1 = self.mninfo[0]
        mn2 = self.mninfo[1]
        mn3 = self.mninfo[2]

        p2p_full, id_p2p_full = p2p_connect(fullnode, start_thread=True)
        p2p_mn1, id_p2p_mn1 = p2p_connect(mn1.node)

        assert_banscore(fullnode, id_p2p_full, 0)
        assert_banscore(mn1.node, id_p2p_mn1, 0)

        quorum_hash_int = int(quorum_hash, 16)
        protx_hash_int = int(mn1.proTxHash, 16)

        # Valid requests
        qgetdata_vvec = msg_qgetdata(quorum_hash_int, 100, 0x01, protx_hash_int)
        qgetdata_contributions = msg_qgetdata(quorum_hash_int, 100, 0x02, protx_hash_int)
        qgetdata_all = msg_qgetdata(quorum_hash_int, 100, 0x03, protx_hash_int)
        # The normal fullnode should not respond to qgetdata
        p2p_full.test_qgetdata(qgetdata_all, response_expected=False)
        assert_banscore(fullnode, id_p2p_full, 10)
        # The masternode should not respond to qgetdata for non-masternode connections
        p2p_mn1.test_qgetdata(qgetdata_all, response_expected=False)
        assert_banscore(mn1.node, id_p2p_mn1, 10)
        # Open a fake MNAUTH authenticated P2P connection to the masternode to allow qgetdata
        p2p_mn1, id_p2p_mn1 = p2p_connect(mn1.node, mnauth=mn1)
        # The masternode should now respond to qgetdata requests
        # Request verification vector
        p2p_mn1.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
        assert_banscore(mn1.node, id_p2p_mn1, 0)
        # Request encrypted contributions
        p2p_mn1.test_qgetdata(qgetdata_contributions, 0, 0, self.llmq_size)
        assert_banscore(mn1.node, id_p2p_mn1, 25)
        # Request both
        p2p_mn1.test_qgetdata(qgetdata_all, 0, self.llmq_threshold, self.llmq_size)
        assert_banscore(mn1.node, id_p2p_mn1, 50)
        # Create invalid messages for all error cases
        qgetdata_invalid_type = msg_qgetdata(quorum_hash_int, 103, 0x01, protx_hash_int)
        qgetdata_invalid_block = msg_qgetdata(protx_hash_int, 100, 0x01, protx_hash_int)
        qgetdata_invalid_quorum = msg_qgetdata(int(mn1.node.getblockhash(0), 16), 100, 0x01, protx_hash_int)
        qgetdata_invalid_no_member = msg_qgetdata(quorum_hash_int, 100, 0x02, quorum_hash_int)
        # Possible error values of QDATA
        QUORUM_TYPE_INVALID = 1
        QUORUM_BLOCK_NOT_FOUND = 2
        QUORUM_NOT_FOUND = 3
        MASTERNODE_IS_NO_MEMBER = 4
        QUORUM_VERIFICATION_VECTOR_MISSING = 5
        ENCRYPTED_CONTRIBUTIONS_MISSING = 6
        # Test all invalid messages
        p2p_mn1, id_p2p_mn1 = p2p_connect(mn1.node, mnauth=mn1)  # Reconnect to clear banscore
        p2p_mn1.test_qgetdata(qgetdata_invalid_type, QUORUM_TYPE_INVALID)
        p2p_mn1.test_qgetdata(qgetdata_invalid_block, QUORUM_BLOCK_NOT_FOUND)
        p2p_mn1.test_qgetdata(qgetdata_invalid_quorum, QUORUM_NOT_FOUND)
        p2p_mn1.test_qgetdata(qgetdata_invalid_no_member, MASTERNODE_IS_NO_MEMBER)
        # The last two error case require the node to miss its DKG data so we just reindex the node.
        restart_mn(mn1, reindex=True)
        # Re-connect to the masternode
        p2p_mn1, id_p2p_mn1 = p2p_connect(mn1.node, mnauth=mn1)
        # Validate the DKG data is missing
        p2p_mn1.test_qgetdata(qgetdata_vvec, QUORUM_VERIFICATION_VECTOR_MISSING)
        p2p_mn1.test_qgetdata(qgetdata_contributions, ENCRYPTED_CONTRIBUTIONS_MISSING)
        # Now that mn1 is missing its DKG data try to recover it by querying the data from mn2 and then sending it to
        # mn1 with a direct QDATA message.
        #
        # P2P - QGETDATA -> mn2 - QDATA -> P2P - QDATA -> mn1
        #
        # However, mn1 only accepts self requested QDATA messages, that's why we trigger mn1 - QGETDATA -> P2P
        # via the RPC command "quorum getdata".
        #
        # First establish a authenticated P2P connection to mn2
        p2p_mn2, _ = p2p_connect(mn2.node, mnauth=mn2)
        # Get the required DKG data for mn1
        p2p_mn2.test_qgetdata(qgetdata_all, 0, self.llmq_threshold, self.llmq_size)
        # Trigger mn1 - QGETDATA -> p2p_mn1
        assert(mn1.node.quorum("getdata", id_p2p_mn1, 100, quorum_hash, 0x03, mn1.proTxHash))
        # Wait until mn1 sent the QGETDATA to p2p_mn1
        p2p_mn1.wait_for_qgetdata()
        # Send the QDATA received from mn2 to mn1
        p2p_mn1.send_message(p2p_mn2.get_qdata())
        # Now mn1 should have its data back!
        test_dkg_data(mn1, p2p_mn1, 100, quorum_hash)
        # Restart one more time and make sure data gets saved to db
        restart_mn(mn1)
        p2p_mn1, id_p2p_mn1 = p2p_connect(mn1.node, mnauth=mn1)
        test_dkg_data(mn1, p2p_mn1, 100, quorum_hash)
        # Test request limiting / banscore increase
        p2p_mn1.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
        assert_banscore(mn1.node, id_p2p_mn1, 0)
        force_request_expire(299)  # This shouldn't clear requests, next request should bump score
        p2p_mn1.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
        assert_banscore(mn1.node, id_p2p_mn1, 25)
        force_request_expire(1)  # This should clear the requests now, next request should not bump score
        p2p_mn1.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
        assert_banscore(mn1.node, id_p2p_mn1, 25)
        # Requesting one QDATA with mn1 and mn2 from mn3 should not result in banscore increase
        # for either of both.
        p2p_mn3_1, id_p2p_mn3_1 = p2p_connect(mn3.node, mnauth=mn1)
        p2p_mn3_2, id_p2p_mn3_2 = p2p_connect(mn3.node, mnauth=mn2)

        def test_mn3(expected_banscore, clear_requests=False):
            p2p_mn3_1.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
            p2p_mn3_2.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
            assert_banscore(mn3.node, id_p2p_mn3_1, expected_banscore)
            assert_banscore(mn3.node, id_p2p_mn3_2, expected_banscore)
            if clear_requests:
                force_request_expire()

        test_mn3(0, True)
        test_mn3(0, True)
        test_mn3(0)
        test_mn3(25)
        test_mn3(50)
        test_mn3(75)
        test_mn3(None)
        # Both should be banned now
        assert(not p2p_mn3_1.is_connected)
        assert(not p2p_mn3_2.is_connected)
        # Test ban score increase for invalid/unexpected QDATA
        p2p_mn1, id_p2p_mn1 = p2p_connect(mn1.node, mnauth=mn1)
        assert_banscore(mn1.node, id_p2p_mn1, 0)
        qdata_valid = p2p_mn2.get_qdata()
        # - Not requested
        p2p_mn1.send_message(qdata_valid)
        assert_banscore(mn1.node, id_p2p_mn1, 10)
        # - Already received
        force_request_expire()
        assert(mn1.node.quorum("getdata", id_p2p_mn1, 100, quorum_hash, 0x03, mn1.proTxHash))
        p2p_mn1.wait_for_qgetdata()
        p2p_mn1.send_message(qdata_valid)
        time.sleep(1)
        p2p_mn1.send_message(qdata_valid)
        assert_banscore(mn1.node, id_p2p_mn1, 20)
        # - Not like requested
        force_request_expire()
        assert(mn1.node.quorum("getdata", id_p2p_mn1, 100, quorum_hash, 0x03, mn1.proTxHash))
        p2p_mn1.wait_for_qgetdata()
        qdata_invalid_request = qdata_valid
        qdata_invalid_request.data_mask = 2
        p2p_mn1.send_message(qdata_invalid_request)
        assert_banscore(mn1.node, id_p2p_mn1, 30)
        # - Invalid verification vector
        force_request_expire()
        assert(mn1.node.quorum("getdata", id_p2p_mn1, 100, quorum_hash, 0x03, mn1.proTxHash))
        p2p_mn1.wait_for_qgetdata()
        qdata_invalid_vvec = qdata_valid
        qdata_invalid_vvec.quorum_vvec.pop()
        p2p_mn1.send_message(qdata_invalid_vvec)
        assert_banscore(mn1.node, id_p2p_mn1, 40)
        # - Invalid contributions
        force_request_expire()
        assert(mn1.node.quorum("getdata", id_p2p_mn1, 100, quorum_hash, 0x03, mn1.proTxHash))
        p2p_mn1.wait_for_qgetdata()
        qdata_invalid_contribution = qdata_valid
        qdata_invalid_contribution.enc_contributions.pop()
        p2p_mn1.send_message(qdata_invalid_contribution)
        assert_banscore(mn1.node, id_p2p_mn1, 50)


if __name__ == '__main__':
    QuorumDataMessagesTest().main()
