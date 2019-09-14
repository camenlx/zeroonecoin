// Copyright (c) 2014-2019 The Dash Core developers
// Copyright (c) 2018-2019 The ZeroOne Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "addrman.h"
#include "clientversion.h"
#include "governance.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "messagesigner.h"
#include "netfulfilledman.h"
#include "netmessagemaker.h"
#include "net.h"
#include "netbase.h"
#include "validation.h"
#ifdef ENABLE_WALLET
#include "privatesend-client.h"
#endif // ENABLE_WALLET
#include "script/standard.h"
#include "ui_interface.h"
#include "util.h"
#include "warnings.h"

/** Masternode manager */
CMasternodeMan mnodeman;

const std::string CMasternodeMan::SERIALIZATION_VERSION_STRING = "CMasternodeMan-Version-8";
const int CMasternodeMan::LAST_PAID_SCAN_BLOCKS = 100;

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, const CMasternode*>& t1,
                    const std::pair<int, const CMasternode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};

struct CompareScoreMN
{
    bool operator()(const std::pair<arith_uint256, const CMasternode*>& t1,
                    const std::pair<arith_uint256, const CMasternode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};

struct CompareByAddr
{
    bool operator()(const CMasternode* t1,
                    const CMasternode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

struct CompareByPoSeBanScore
{
    bool operator()(const CMasternode* t1,
                    const CMasternode* t2) const
    {
        return t1->nPoSeBanScore < t2->nPoSeBanScore;
    }
};

template < typename T>
std::pair<bool, int > findInVector(const std::vector<T>  & vecOfElements, const T  & element)
{
	std::pair<bool, int > result;
 
	// Find given element in vector
	auto it = std::find(vecOfElements.begin(), vecOfElements.end(), element);
 
	if (it != vecOfElements.end())
	{
		result.second = distance(vecOfElements.begin(), it);
		result.first = true;
	}
	else
	{
		result.first = false;
		result.second = -1;
	}
 
	return result;
}


CMasternodeMan::CMasternodeMan():
    cs(),
    mapMasternodes(),
    mAskedUsForMasternodeList(),
    mWeAskedForMasternodeList(),
    mWeAskedForMasternodeListEntry(),
    mWeAskedForVerification(),
    mMnbRecoveryRequests(),
    mMnbRecoveryGoodReplies(),
    listScheduledMnbRequestConnections(),
    fMasternodesAdded(false),
    fMasternodesRemoved(false),
    vecDirtyGovernanceObjectHashes(),
    nLastSentinelPingTime(0),
    mapSeenMasternodeBroadcast(),
    mapSeenMasternodePing(),
    nDsqCount(0)
{}

bool CMasternodeMan::Add(CMasternode &mn)
{
    LOCK(cs);

    if (Has(mn.outpoint)) return false;
    if (HasAddr(mn.addr)) return false;

    LogPrint("masternode", "CMasternodeMan::Add -- Adding new Masternode: addr=%s, %i now\n", mn.addr.ToString(), size() + 1);
    mapMasternodes[mn.outpoint] = mn;
    fMasternodesAdded = true;
    return true;
}

void CMasternodeMan::AskForMN(CNode* pnode, const COutPoint& outpoint, CConnman& connman)
{
    if(!pnode) return;

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    LOCK(cs);

    CService addrSquashed = Params().AllowMultiplePorts() ? (CService)pnode->addr : CService(pnode->addr, 0);
    auto it1 = mWeAskedForMasternodeListEntry.find(outpoint);
    if (it1 != mWeAskedForMasternodeListEntry.end()) {
        auto it2 = it1->second.find(addrSquashed);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintf("CMasternodeMan::AskForMN -- Asking same peer %s for missing masternode entry again: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CMasternodeMan::AskForMN -- Asking new peer %s for missing masternode entry: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintf("CMasternodeMan::AskForMN -- Asking peer %s for missing masternode entry for the first time: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
    }
    mWeAskedForMasternodeListEntry[outpoint][addrSquashed] = GetTime() + DSEG_UPDATE_SECONDS;
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, outpoint));
}

void CMasternodeMan::AskForMnv(const CService& addr, const COutPoint& outpoint)
{
    if(activeMasternode.outpoint.IsNull()) return;
    if(!masternodeSync.IsSynced()) return;

    CAddress caddr = CAddress(addr, NODE_NETWORK);
    
    netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
    g_connman->AddPendingMasternode(addr);

    // use random nonce, store it and require node to reply with correct one later
    CMasternodeVerification mnv(addr, GetRandInt(999999), nCachedBlockHeight - 1);
    {
        LOCK(cs_mapPendingMNV);
        mapPendingMNV.insert(std::make_pair(addr, std::make_pair(GetTime(), mnv)));
    }
    LogPrintf("CMasternodeMan::AskForMnv -- verifying node using nonce %d addr=%s\n", mnv.nonce, addr.ToString());
}

bool CMasternodeMan::AllowMixing(const COutPoint &outpoint)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    nDsqCount++;
    pmn->nLastDsq = nDsqCount;
    pmn->fAllowMixingTx = true;

    return true;
}

bool CMasternodeMan::DisallowMixing(const COutPoint &outpoint)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    pmn->fAllowMixingTx = false;

    return true;
}

bool CMasternodeMan::IncreasePoSeBanScore(const COutPoint &outpoint)
{
    // this function is not for ourselves
    if(outpoint == activeMasternode.outpoint) return false;
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    pmn->IncreasePoSeBanScore();

    return true;
}

bool CMasternodeMan::DecreasePoSeBanScore(const COutPoint &outpoint)
{
    // this function is not for ourselves
    if(outpoint == activeMasternode.outpoint) return false;
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    pmn->DecreasePoSeBanScore();

    return true;
}

bool CMasternodeMan::PoSeBan(const COutPoint &outpoint)
{
    // this function is not for ourselves
    if(outpoint == activeMasternode.outpoint) return false;
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    pmn->PoSeBan();

    return true;
}

bool CMasternodeMan::IncreasePoSeBanScore(const CService& addr)
{
    // this function is not for ourselves
    if(addr == activeMasternode.service) return false;
    LOCK(cs);
    for (const auto& mnpair : mapMasternodes) {
        if (addr == mnpair.second.addr) {
            return IncreasePoSeBanScore(mnpair.second.outpoint);
        }
    }
    return false;
}

bool CMasternodeMan::DecreasePoSeBanScore(const CService& addr)
{
    // this function is not for ourselves
    if(addr == activeMasternode.service) return false;
    LOCK(cs);
    for (const auto& mnpair : mapMasternodes) {
        if (addr == mnpair.second.addr) {
            return DecreasePoSeBanScore(mnpair.second.outpoint);
        }
    }
    return false;
}

bool CMasternodeMan::PoSeBan(const CService& addr)
{
    // this function is not for ourselves
    if(addr == activeMasternode.service) return false;
    LOCK(cs);
    for (const auto& mnpair : mapMasternodes) {
        if (addr == mnpair.second.addr) {
            return PoSeBan(mnpair.second.outpoint);
        }
    }
    return false;
}

void CMasternodeMan::Check()
{
    LOCK2(cs_main, cs);

    LogPrint("masternode", "CMasternodeMan::Check -- nLastSentinelPingTime=%d, IsSentinelPingActive()=%d\n", nLastSentinelPingTime, IsSentinelPingActive());

    for (auto& mnpair : mapMasternodes) {
        // NOTE: internally it checks only every MASTERNODE_CHECK_SECONDS seconds
        // since the last time, so expect some MNs to skip this
        mnpair.second.Check();
    }
}

void CMasternodeMan::CheckAndRemove(CConnman& connman)
{
    if(!masternodeSync.IsMasternodeListSynced()) return;

    LogPrintf("CMasternodeMan::CheckAndRemove\n");

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckMnbAndUpdateMasternodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent masternodes, prepare structures and make requests to reasure the state of inactive ones
        rank_pair_vec_t vecMasternodeRanks;
        // ask for up to MNB_RECOVERY_MAX_ASK_ENTRIES masternode entries at a time
        int nAskForMnbRecovery = MNB_RECOVERY_MAX_ASK_ENTRIES;
        std::map<COutPoint, CMasternode>::iterator it = mapMasternodes.begin();
        while (it != mapMasternodes.end()) {
            CMasternodeBroadcast mnb = CMasternodeBroadcast(it->second);
            uint256 hash = mnb.GetHash();
            // If collateral was spent ...
            if (it->second.IsOutpointSpent() || it->second.IsUpdateRequired() || it->second.IsPoSeBanned()) {
                LogPrint("masternode", "CMasternodeMan::CheckAndRemove -- Removing Masternode: %s  addr=%s  %i now\n", it->second.GetStateString(), it->second.addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenMasternodeBroadcast.erase(hash);
                mWeAskedForMasternodeListEntry.erase(it->first);

                // and finally remove it from the list
                it->second.FlagGovernanceItemsAsDirty();
                mapMasternodes.erase(it++);
                fMasternodesRemoved = true;
            } else {
                bool fAsk = (nAskForMnbRecovery > 0) &&
                            masternodeSync.IsSynced() &&
                            it->second.IsNewStartRequired() &&
                            !IsMnbRecoveryRequested(hash) &&
                            !IsArgSet("-connect");
                if(fAsk) {
                    // this mn is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CService> setRequested;
                    // calulate only once and only when it's needed
                    if(vecMasternodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(nCachedBlockHeight);
                        GetMasternodeRanks(vecMasternodeRanks, nRandomBlockHeight);
                    }
                    bool fAskedForMnbRecovery = false;
                    // ask first MNB_RECOVERY_QUORUM_TOTAL masternodes we can connect to and we haven't asked recently
                    for(int i = 0; setRequested.size() < MNB_RECOVERY_QUORUM_TOTAL && i < (int)vecMasternodeRanks.size(); i++) {
                        // avoid banning
                        if(mWeAskedForMasternodeListEntry.count(it->first) && mWeAskedForMasternodeListEntry[it->first].count(vecMasternodeRanks[i].second.addr)) continue;
                        // didn't ask recently, ok to ask now
                        CService addr = vecMasternodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledMnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForMnbRecovery = true;
                    }
                    if(fAskedForMnbRecovery) {
                        LogPrint("masternode", "CMasternodeMan::CheckAndRemove -- Recovery initiated, masternode=%s\n", it->first.ToStringShort());
                        nAskForMnbRecovery--;
                    }
                    // wait for mnb recovery replies for MNB_RECOVERY_WAIT_SECONDS seconds
                    mMnbRecoveryRequests[hash] = std::make_pair(GetTime() + MNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }

        // proces replies for MASTERNODE_NEW_START_REQUIRED masternodes
        LogPrint("masternode", "CMasternodeMan::CheckAndRemove -- mMnbRecoveryGoodReplies size=%d\n", (int)mMnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CMasternodeBroadcast> >::iterator itMnbReplies = mMnbRecoveryGoodReplies.begin();
        while(itMnbReplies != mMnbRecoveryGoodReplies.end()){
            if(mMnbRecoveryRequests[itMnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if(itMnbReplies->second.size() >= MNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this mn doesn't require new mnb, reprocess one of new mnbs
                    LogPrint("masternode", "CMasternodeMan::CheckAndRemove -- reprocessing mnb, masternode=%s\n", itMnbReplies->second[0].outpoint.ToStringShort());
                    // mapSeenMasternodeBroadcast.erase(itMnbReplies->first);
                    int nDos;
                    itMnbReplies->second[0].fRecovery = true;
                    CheckMnbAndUpdateMasternodeList(NULL, itMnbReplies->second[0], nDos, connman);
                }
                LogPrint("masternode", "CMasternodeMan::CheckAndRemove -- removing mnb recovery reply, masternode=%s, size=%d\n", itMnbReplies->second[0].outpoint.ToStringShort(), (int)itMnbReplies->second.size());
                mMnbRecoveryGoodReplies.erase(itMnbReplies++);
            } else {
                ++itMnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        auto itMnbRequest = mMnbRecoveryRequests.begin();
        while(itMnbRequest != mMnbRecoveryRequests.end()){
            // Allow this mnb to be re-verified again after MNB_RECOVERY_RETRY_SECONDS seconds
            // if mn is still in MASTERNODE_NEW_START_REQUIRED state.
            if(GetTime() - itMnbRequest->second.first > MNB_RECOVERY_RETRY_SECONDS) {
                mMnbRecoveryRequests.erase(itMnbRequest++);
            } else {
                ++itMnbRequest;
            }
        }

        // check who's asked for the Masternode list
        auto it1 = mAskedUsForMasternodeList.begin();
        while(it1 != mAskedUsForMasternodeList.end()){
            if((*it1).second < GetTime()) {
                mAskedUsForMasternodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Masternode list
        it1 = mWeAskedForMasternodeList.begin();
        while(it1 != mWeAskedForMasternodeList.end()){
            if((*it1).second < GetTime()){
                mWeAskedForMasternodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Masternodes we've asked for
        auto it2 = mWeAskedForMasternodeListEntry.begin();
        while(it2 != mWeAskedForMasternodeListEntry.end()){
            auto it3 = it2->second.begin();
            while(it3 != it2->second.end()){
                if(it3->second < GetTime()){
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if(it2->second.empty()) {
                mWeAskedForMasternodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        auto it3 = mWeAskedForVerification.begin();
        while(it3 != mWeAskedForVerification.end()){
            if(it3->second.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenMasternodeBroadcast entries here, clean them on mnb updates!

        // remove expired mapSeenMasternodePing
        std::map<uint256, CMasternodePing>::iterator it4 = mapSeenMasternodePing.begin();
        while(it4 != mapSeenMasternodePing.end()){
            if((*it4).second.IsExpired()) {
                LogPrint("masternode", "CMasternodeMan::CheckAndRemove -- Removing expired Masternode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenMasternodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenMasternodeVerification
        std::map<uint256, CMasternodeVerification>::iterator itv2 = mapSeenMasternodeVerification.begin();
        while(itv2 != mapSeenMasternodeVerification.end()){
            if((*itv2).second.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS){
                LogPrint("masternode", "CMasternodeMan::CheckAndRemove -- Removing expired Masternode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenMasternodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrintf("CMasternodeMan::CheckAndRemove -- %s\n", ToString());
    }

    if(fMasternodesRemoved) {
        NotifyMasternodeUpdates(connman);
    }
}

void CMasternodeMan::Clear()
{
    LOCK(cs);
    mapMasternodes.clear();
    mAskedUsForMasternodeList.clear();
    mWeAskedForMasternodeList.clear();
    mWeAskedForMasternodeListEntry.clear();
    mapSeenMasternodeBroadcast.clear();
    mapSeenMasternodePing.clear();
    nDsqCount = 0;
    nLastSentinelPingTime = 0;
}

int CMasternodeMan::CountMasternodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinMasternodePaymentsProto() : nProtocolVersion;

    for (const auto& mnpair : mapMasternodes) {
        if(mnpair.second.nProtocolVersion < nProtocolVersion) continue;
        nCount++;
    }

    return nCount;
}

int CMasternodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinMasternodePaymentsProto() : nProtocolVersion;

    for (const auto& mnpair : mapMasternodes) {
        if(mnpair.second.nProtocolVersion < nProtocolVersion || !mnpair.second.IsEnabled()) continue;
        nCount++;
    }

    return nCount;
}

int CMasternodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    for (const auto& mnpair : mapMasternodes)
        if ((nNetworkType == NET_IPV4 && mnpair.second.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mnpair.second.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mnpair.second.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}

void CMasternodeMan::DsegUpdate(CNode* pnode, CConnman& connman)
{
    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    LOCK(cs);

    CService addrSquashed = Params().AllowMultiplePorts() ? (CService)pnode->addr : CService(pnode->addr, 0);
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            auto it = mWeAskedForMasternodeList.find(addrSquashed);
            if(it != mWeAskedForMasternodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CMasternodeMan::DsegUpdate -- we already asked %s for the list; skipping...\n", addrSquashed.ToString());
                return;
            }
        }
    }
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, COutPoint()));
    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForMasternodeList[addrSquashed] = askAgain;

    LogPrint("masternode", "CMasternodeMan::DsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CMasternode* CMasternodeMan::Find(const COutPoint &outpoint)
{
    LOCK(cs);
    auto it = mapMasternodes.find(outpoint);
    return it == mapMasternodes.end() ? NULL : &(it->second);
}

bool CMasternodeMan::Get(const COutPoint& outpoint, CMasternode& masternodeRet)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    auto it = mapMasternodes.find(outpoint);
    if (it == mapMasternodes.end()) {
        return false;
    }

    masternodeRet = it->second;
    return true;
}

bool CMasternodeMan::GetMasternodeInfo(const COutPoint& outpoint, masternode_info_t& mnInfoRet)
{
    LOCK(cs);
    auto it = mapMasternodes.find(outpoint);
    if (it == mapMasternodes.end()) {
        return false;
    }
    mnInfoRet = it->second.GetInfo();
    return true;
}

bool CMasternodeMan::GetMasternodeInfo(const CPubKey& pubKeyMasternode, masternode_info_t& mnInfoRet)
{
    LOCK(cs);
    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.pubKeyMasternode == pubKeyMasternode) {
            mnInfoRet = mnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CMasternodeMan::GetMasternodeInfo(const CScript& payee, masternode_info_t& mnInfoRet)
{
    LOCK(cs);
    for (const auto& mnpair : mapMasternodes) {
        CScript scriptCollateralAddress = GetScriptForDestination(mnpair.second.pubKeyCollateralAddress.GetID());
        if (scriptCollateralAddress == payee) {
            mnInfoRet = mnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CMasternodeMan::Has(const COutPoint& outpoint)
{
    LOCK(cs);
    return mapMasternodes.find(outpoint) != mapMasternodes.end();
}

bool CMasternodeMan::HasAddr(const CService& addr)
{
    LOCK(cs);
    for (const auto& mnpair : mapMasternodes) {
        if (addr == mnpair.second.addr) {
            return true;
        }
    }
    return false;
}

//
// Deterministically select the oldest/best masternode to pay on the network
//
bool CMasternodeMan::GetNextMasternodeInQueueForPayment(bool fFilterSigTime, int& nCountRet, masternode_info_t& mnInfoRet)
{
    return GetNextMasternodeInQueueForPayment(nCachedBlockHeight, fFilterSigTime, nCountRet, mnInfoRet);
}

bool CMasternodeMan::GetNextMasternodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCountRet, masternode_info_t& mnInfoRet)
{
    mnInfoRet = masternode_info_t();
    nCountRet = 0;

    if (!masternodeSync.IsWinnersListSynced()) {
        // without winner list we can't reliably find the next winner anyway
        return false;
    }

    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main,cs);

    std::vector<std::pair<int, const CMasternode*> > vecMasternodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */

    int nMnCount = CountMasternodes();

    for (const auto& mnpair : mapMasternodes) {
        if(!mnpair.second.IsValidForPayment()) continue;

        //check protocol version
        if(mnpair.second.nProtocolVersion < mnpayments.GetMinMasternodePaymentsProto()) continue;

        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if(mnpayments.IsScheduled(mnpair.second, nBlockHeight)) continue;

        //it's too new, wait for a cycle
        if(fFilterSigTime && mnpair.second.sigTime + (nMnCount*2.6*60) > GetAdjustedTime()) continue;

        //make sure it has at least as many confirmations as there are masternodes
        if(GetUTXOConfirmations(mnpair.first) < nMnCount) continue;

        vecMasternodeLastPaid.push_back(std::make_pair(mnpair.second.GetLastPaidBlock(), &mnpair.second));
    }

    nCountRet = (int)vecMasternodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if(fFilterSigTime && nCountRet < nMnCount/3)
        return GetNextMasternodeInQueueForPayment(nBlockHeight, false, nCountRet, mnInfoRet);

    // Sort them low to high
    sort(vecMasternodeLastPaid.begin(), vecMasternodeLastPaid.end(), CompareLastPaidBlock());

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight - 101)) {
        LogPrintf("CMasternode::GetNextMasternodeInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - 101);
        return false;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nMnCount/10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    const CMasternode *pBestMasternode = NULL;
    for (const auto& s : vecMasternodeLastPaid) {
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if(nScore > nHighest){
            nHighest = nScore;
            pBestMasternode = s.second;
        }
        nCountTenth++;
        if(nCountTenth >= nTenthNetwork) break;
    }
    if (pBestMasternode) {
        mnInfoRet = pBestMasternode->GetInfo();
    }
    return mnInfoRet.fInfoValid;
}

masternode_info_t CMasternodeMan::FindRandomNotInVec(const std::vector<COutPoint> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinMasternodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CMasternodeMan::FindRandomNotInVec -- %d enabled masternodes, %d masternodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if(nCountNotExcluded < 1) return masternode_info_t();

    // fill a vector of pointers
    std::vector<const CMasternode*> vpMasternodesShuffled;
    for (const auto& mnpair : mapMasternodes) {
        vpMasternodesShuffled.push_back(&mnpair.second);
    }

    FastRandomContext insecure_rand;
    // shuffle pointers
    std::random_shuffle(vpMasternodesShuffled.begin(), vpMasternodesShuffled.end(), insecure_rand);
    bool fExclude;

    // loop through
    for (const auto& pmn : vpMasternodesShuffled) {
        if(pmn->nProtocolVersion < nProtocolVersion || !pmn->IsEnabled()) continue;
        fExclude = false;
        for (const auto& outpointToExclude : vecToExclude) {
            if(pmn->outpoint == outpointToExclude) {
                fExclude = true;
                break;
            }
        }
        if(fExclude) continue;
        // found the one not in vecToExclude
        LogPrint("masternode", "CMasternodeMan::FindRandomNotInVec -- found, masternode=%s\n", pmn->outpoint.ToStringShort());
        return pmn->GetInfo();
    }

    LogPrint("masternode", "CMasternodeMan::FindRandomNotInVec -- failed\n");
    return masternode_info_t();
}

bool CMasternodeMan::GetMasternodeScores(const uint256& nBlockHash, CMasternodeMan::score_pair_vec_t& vecMasternodeScoresRet, int nMinProtocol)
{
    vecMasternodeScoresRet.clear();

    if (!masternodeSync.IsMasternodeListSynced())
        return false;

    AssertLockHeld(cs);

    if (mapMasternodes.empty())
        return false;

    // calculate scores
    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.nProtocolVersion >= nMinProtocol) {
            vecMasternodeScoresRet.push_back(std::make_pair(mnpair.second.CalculateScore(nBlockHash), &mnpair.second));
        }
    }

    sort(vecMasternodeScoresRet.rbegin(), vecMasternodeScoresRet.rend(), CompareScoreMN());
    return !vecMasternodeScoresRet.empty();
}

bool CMasternodeMan::GetMasternodeRank(const COutPoint& outpoint, int& nRankRet, int nBlockHeight, int nMinProtocol)
{
    nRankRet = -1;

    if (!masternodeSync.IsMasternodeListSynced())
        return false;

    // make sure we know about this block
    uint256 nBlockHash = uint256();
    if (!GetBlockHash(nBlockHash, nBlockHeight)) {
        LogPrintf("CMasternodeMan::%s -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", __func__, nBlockHeight);
        return false;
    }

    LOCK(cs);

    score_pair_vec_t vecMasternodeScores;
    if (!GetMasternodeScores(nBlockHash, vecMasternodeScores, nMinProtocol))
        return false;

    int nRank = 0;
    for (const auto& scorePair : vecMasternodeScores) {
        nRank++;
        if(scorePair.second->outpoint == outpoint) {
            nRankRet = nRank;
            return true;
        }
    }

    return false;
}

bool CMasternodeMan::GetMasternodeRanks(CMasternodeMan::rank_pair_vec_t& vecMasternodeRanksRet, int nBlockHeight, int nMinProtocol)
{
    vecMasternodeRanksRet.clear();

    if (!masternodeSync.IsMasternodeListSynced())
        return false;

    // make sure we know about this block
    uint256 nBlockHash = uint256();
    if (!GetBlockHash(nBlockHash, nBlockHeight)) {
        LogPrintf("CMasternodeMan::%s -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", __func__, nBlockHeight);
        return false;
    }

    LOCK(cs);

    score_pair_vec_t vecMasternodeScores;
    if (!GetMasternodeScores(nBlockHash, vecMasternodeScores, nMinProtocol))
        return false;

    int nRank = 0;
    for (const auto& scorePair : vecMasternodeScores) {
        nRank++;
        vecMasternodeRanksRet.push_back(std::make_pair(nRank, *scorePair.second));
    }

    return true;
}

void CMasternodeMan::ProcessMasternodeConnections(CConnman& connman)
{
    //we don't care about this for regtest
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) return;

    connman.ForEachNode(CConnman::AllNodes, [](CNode* pnode) {
#ifdef ENABLE_WALLET
        if(pnode->fMasternode && !privateSendClient.IsMixingMasternode(pnode)) {
#else
        if(pnode->fMasternode) {
#endif // ENABLE_WALLET
            LogPrintf("Closing Masternode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    });
}

std::pair<CService, std::set<uint256> > CMasternodeMan::PopScheduledMnbRequestConnection()
{
    LOCK(cs);
    if(listScheduledMnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledMnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledMnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list< std::pair<CService, uint256> >::iterator it = listScheduledMnbRequestConnections.begin();
    while(it != listScheduledMnbRequestConnections.end()) {
        if(pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledMnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}

void CMasternodeMan::ProcessPendingMnbRequests(CConnman& connman)
{
    std::pair<CService, std::set<uint256> > p = PopScheduledMnbRequestConnection();
    if (!(p.first == CService() || p.second.empty())) {
        if (connman.IsMasternodeOrDisconnectRequested(p.first)) return;
        mapPendingMNB.insert(std::make_pair(p.first, std::make_pair(GetTime(), p.second)));
        connman.AddPendingMasternode(p.first);
    }

    std::map<CService, std::pair<int64_t, std::set<uint256> > >::iterator itPendingMNB = mapPendingMNB.begin();
    while (itPendingMNB != mapPendingMNB.end()) {
        bool fDone = connman.ForNode(itPendingMNB->first, [&](CNode* pnode) {
            // compile request vector
            std::vector<CInv> vToFetch;
            std::set<uint256>& setHashes = itPendingMNB->second.second;
            std::set<uint256>::iterator it = setHashes.begin();
            while(it != setHashes.end()) {
                if(*it != uint256()) {
                    vToFetch.push_back(CInv(MSG_MASTERNODE_ANNOUNCE, *it));
                    LogPrint("masternode", "-- asking for mnb %s from addr=%s\n", it->ToString(), pnode->addr.ToString());
                }
                ++it;
            }

            // ask for data
            CNetMsgMaker msgMaker(pnode->GetSendVersion());
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETDATA, vToFetch));
            return true;
        });

        int64_t nTimeAdded = itPendingMNB->second.first;
        if (fDone || (GetTime() - nTimeAdded > 15)) {
            if (!fDone) {
                LogPrintf("CMasternodeMan::%s -- failed to connect to %s\n", __func__, itPendingMNB->first.ToString());
                //Punish not reachable MN , required cs_main
                //PunishNode(itPendingMNB->first,connman);
            }
            mapPendingMNB.erase(itPendingMNB++);
        } else {
            ++itPendingMNB;
        }
    }
    LogPrintf("%s -- mapPendingMNB size: %d\n", __func__, mapPendingMNB.size());
}

void CMasternodeMan::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if(fLiteMode) return; // disable all ZOC specific functionality

    if (strCommand == NetMsgType::MNANNOUNCE) { //Masternode Broadcast

        CMasternodeBroadcast mnb;
        vRecv >> mnb;

        pfrom->setAskFor.erase(mnb.GetHash());

        if(!masternodeSync.IsBlockchainSynced()) return;

        LogPrint("masternode", "MNANNOUNCE -- Masternode announce, masternode=%s\n", mnb.outpoint.ToStringShort());

        int nDos = 0;

        if (CheckMnbAndUpdateMasternodeList(pfrom, mnb, nDos, connman)) {
            // use announced Masternode as a peer
            connman.AddNewAddress(CAddress(mnb.addr, NODE_NETWORK), pfrom->addr, 2*60*60);
        } else if(nDos > 0) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), nDos);
        }

        if(fMasternodesAdded) {
            NotifyMasternodeUpdates(connman);
        }
    } else if (strCommand == NetMsgType::MNPING) { //Masternode Ping

        CMasternodePing mnp;
        vRecv >> mnp;

        uint256 nHash = mnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        if(!masternodeSync.IsBlockchainSynced()) return;

        LogPrint("masternode", "MNPING -- Masternode ping, masternode=%s\n", mnp.masternodeOutpoint.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if(mapSeenMasternodePing.count(nHash)) return; //seen
        mapSeenMasternodePing.insert(std::make_pair(nHash, mnp));

        LogPrint("masternode", "MNPING -- Masternode ping, masternode=%s new\n", mnp.masternodeOutpoint.ToStringShort());

        // see if we have this Masternode
        CMasternode* pmn = Find(mnp.masternodeOutpoint);

        if(pmn && mnp.fSentinelIsCurrent)
            UpdateLastSentinelPingTime();

        // too late, new MNANNOUNCE is required
        if(pmn && pmn->IsNewStartRequired()) return;

        int nDos = 0;
        if(mnp.CheckAndUpdate(pmn, false, nDos, connman)) return;

        if(nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if(pmn != NULL) {
            // nothing significant failed, mn is a known one too
            return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a masternode entry once
        AskForMN(pfrom, mnp.masternodeOutpoint, connman);

    } else if (strCommand == NetMsgType::DSEG) { //Get Masternode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after masternode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!masternodeSync.IsSynced()) return;

        COutPoint masternodeOutpoint;
        vRecv >> masternodeOutpoint;

        LogPrint("masternode", "DSEG -- Masternode list, masternode=%s\n", masternodeOutpoint.ToStringShort());

        if(masternodeOutpoint.IsNull()) {
            SyncAll(pfrom, connman);
        } else {
            SyncSingle(pfrom, masternodeOutpoint, connman);
        }

    } else if (strCommand == NetMsgType::MNVERIFY) { // Masternode Verify

        // Need LOCK2 here to ensure consistent locking order because all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CMasternodeVerification mnv;
        vRecv >> mnv;

        pfrom->setAskFor.erase(mnv.GetHash());

        if(!masternodeSync.IsMasternodeListSynced()) return;

        if(mnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, mnv, connman);
        } else if (mnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some masternode
            ProcessVerifyReply(pfrom, mnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some masternode which verified another one
            ProcessVerifyBroadcast(pfrom, mnv);
        }
    }
}

void CMasternodeMan::SyncSingle(CNode* pnode, const COutPoint& outpoint, CConnman& connman)
{
    // do not provide any data until our node is synced
    if (!masternodeSync.IsSynced()) return;

    LOCK(cs);

    auto it = mapMasternodes.find(outpoint);

    if(it != mapMasternodes.end()) {
        if (it->second.addr.IsRFC1918() || it->second.addr.IsLocal()) return; // do not send local network masternode
        // NOTE: send masternode regardless of its current state, the other node will need it to verify old votes.
        LogPrint("masternode", "CMasternodeMan::%s -- Sending Masternode entry: masternode=%s  addr=%s\n", __func__, outpoint.ToStringShort(), it->second.addr.ToString());
        PushDsegInvs(pnode, it->second);
        LogPrintf("CMasternodeMan::%s -- Sent 1 Masternode inv to peer=%d\n", __func__, pnode->id);
    }
}

void CMasternodeMan::SyncAll(CNode* pnode, CConnman& connman)
{
    // do not provide any data until our node is synced
    if (!masternodeSync.IsSynced()) return;

    // local network
    bool isLocal = (pnode->addr.IsRFC1918() || pnode->addr.IsLocal());

    CService addrSquashed = Params().AllowMultiplePorts() ? (CService)pnode->addr : CService(pnode->addr, 0);
    // should only ask for this once
    if(!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
        LOCK2(cs_main, cs);
        auto it = mAskedUsForMasternodeList.find(addrSquashed);
        if (it != mAskedUsForMasternodeList.end() && it->second > GetTime()) {
            Misbehaving(pnode->GetId(), 34);
            LogPrintf("CMasternodeMan::%s -- peer already asked me for the list, peer=%d\n", __func__, pnode->id);
            return;
        }
        int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
        mAskedUsForMasternodeList[addrSquashed] = askAgain;
    }

    int nInvCount = 0;

    LOCK(cs);

    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.addr.IsRFC1918() || mnpair.second.addr.IsLocal()) continue; // do not send local network masternode
        // NOTE: send masternode regardless of its current state, the other node will need it to verify old votes.
        LogPrint("masternode", "CMasternodeMan::%s -- Sending Masternode entry: masternode=%s  addr=%s\n", __func__, mnpair.first.ToStringShort(), mnpair.second.addr.ToString());
        PushDsegInvs(pnode, mnpair.second);
        nInvCount++;
    }

    connman.PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_LIST, nInvCount));
    LogPrintf("CMasternodeMan::%s -- Sent %d Masternode invs to peer=%d\n", __func__, nInvCount, pnode->id);
}

void CMasternodeMan::PushDsegInvs(CNode* pnode, const CMasternode& mn)
{
    AssertLockHeld(cs);

    CMasternodeBroadcast mnb(mn);
    CMasternodePing mnp = mnb.lastPing;
    uint256 hashMNB = mnb.GetHash();
    uint256 hashMNP = mnp.GetHash();
    pnode->PushInventory(CInv(MSG_MASTERNODE_ANNOUNCE, hashMNB));
    pnode->PushInventory(CInv(MSG_MASTERNODE_PING, hashMNP));
    mapSeenMasternodeBroadcast.insert(std::make_pair(hashMNB, std::make_pair(GetTime(), mnb)));
    mapSeenMasternodePing.insert(std::make_pair(hashMNP, mnp));
}

// Requires cs_main.
void CMasternodeMan::PunishNode(const CService& addr, CConnman& connman)
{
    if(!masternodeSync.IsSynced()) return;
    // do not auto-punish
    if(addr == activeMasternode.service) return;

    CNode* found = connman.FindNode(addr);
    LogPrint("masternode","CMasternodeMan::%s -- searching bad node-id at addr=%s\n", __func__, addr.ToString());
    if(found){
      LogPrintf("CMasternodeMan::PunishNode -- found Misbehaving node-id=%d at addr=%s\n", found->id, addr.ToString());
      LOCK(cs_main);
      Misbehaving(found->id, 20);
    }
}

// check socket connect
bool CMasternodeMan::MnCheckConnect(const CMasternode& mn)
{
    bool docheck = fOkDual || (fOkIPv4 && mn.addr.IsIPv4()) || (fOkIPv6 && mn.addr.IsIPv6());
    if (!docheck) {
        LogPrintf("CMasternodeMan::MnCheckConnect -- Cannot check connection to '%s'\n", mn.addr.ToString());
        return docheck;
    }

    // Check socket connectivity
    LogPrintf("CMasternodeMan::MnCheckConnect -- Check connection to '%s'\n", mn.addr.ToString());
    SOCKET hSocket;
    bool fConnected = ConnectSocket(mn.addr, hSocket, nConnectTimeout) && IsSelectableSocket(hSocket);
    CloseSocket(hSocket);

    if (!fConnected) {
        LogPrintf("CMasternodeMan::MnCheckConnect -- %s: Could not connect to %s\n", mn.outpoint.ToStringShort(), mn.addr.ToString());
    }
    return fConnected;
}

// Verification of masternodes via unique direct requests
void CMasternodeMan::DoFullVerificationStep(CConnman& connman)
{
    if(activeMasternode.outpoint.IsNull()) return;
    if(!masternodeSync.IsSynced()) return;

    rank_pair_vec_t vecMasternodeRanks;
    GetMasternodeRanks(vecMasternodeRanks, nCachedBlockHeight - 1, MIN_POSE_PROTO_VERSION);
    std::vector<CAddress> vAddr;
    int nCount = 0;

    {
    LOCK(cs);

    int nMyRank = -1;
    int nRanksTotal = (int)vecMasternodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    rank_pair_vec_t::iterator it = vecMasternodeRanks.begin();
    while(it != vecMasternodeRanks.end()) {
        if(it->second.outpoint == activeMasternode.outpoint) {
            nMyRank = it->first;
            LogPrintf("CMasternodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d masternodes\n",
                        nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            if(it->first > MAX_POSE_RANK) {
                LogPrintf("CMasternodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                            (int)MAX_POSE_RANK);
                return;
            }
            break;
        }
        ++it;
    }

    // edge case: list is too short or this masternode is not enabled
    if(nMyRank == -1) {
    	LogPrintf("CMasternodeMan::DoFullVerificationStep -- list is too short or this masternode is not enabled\n");
        return;
    }

    // send verify requests to up to MAX_POSE_CONNECTIONS masternodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if(nOffset >= (int)vecMasternodeRanks.size()) return;

    it = vecMasternodeRanks.begin() + nOffset;
    while(it != vecMasternodeRanks.end()) {
        if(it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint("masternode", "CMasternodeMan::DoFullVerificationStep -- Already %s%s%s masternode %s address %s, skipping...\n",
                        it->second.IsPoSeVerified() ? "verified" : "",
                        it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                        it->second.IsPoSeBanned() ? "banned" : "",
                        it->second.outpoint.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if(nOffset >= (int)vecMasternodeRanks.size()) break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }

        CAddress addr = CAddress(it->second.addr, NODE_NETWORK);
        if(VerifyRequest(addr, connman)) {
            vAddr.push_back(addr);

            // so avoid double AskForMnv 
            mapWeShouldAskForVerification.erase(it->second.outpoint);

            LogPrintf("CMasternodeMan::DoFullVerificationStep -- Verifying masternode %s rank %d/%d address %s\n",
                       it->second.outpoint.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
            nCount++;
            if(nCount >= MAX_POSE_CONNECTIONS) break;
        }

        nOffset += MAX_POSE_CONNECTIONS;
        if(nOffset >= (int)vecMasternodeRanks.size()) break;
        it += MAX_POSE_CONNECTIONS;
    }

    // include also the ones we think WeShouldAskForVerification
    for (const auto& outpt : mapWeShouldAskForVerification) {
        CMasternode mn4v;
        if (Get(outpt.first, mn4v)) {
            CAddress addr = CAddress(mn4v.addr, NODE_NETWORK);
            vAddr.push_back(addr);
            int TimePassed = GetTime() - outpt.second;
            LogPrintf("CMasternodeMan::DoFullVerificationStep -- Verifying masternode %s after %d secs, address %s\n",
                       mn4v.outpoint.ToStringShort(), TimePassed, mn4v.addr.ToString());
        }
        mapWeShouldAskForVerification.erase(outpt.first);
    }

    } // end lock CS

    for (const auto& addr : vAddr) {
        connman.AddPendingMasternode(addr);
        // use random nonce, store it and require node to reply with correct one later
        CMasternodeVerification mnv(addr, GetRandInt(999999), nCachedBlockHeight - 1);
        LOCK(cs_mapPendingMNV);
        mapPendingMNV.insert(std::make_pair(addr, std::make_pair(GetTime(), mnv)));
        LogPrintf("CMasternodeMan::DoFullVerificationStep -- verifying node using nonce %d addr=%s\n", mnv.nonce, addr.ToString());
    }

    // show allways how many VerifyRequest we think we have sent
    LogPrintf("CMasternodeMan::DoFullVerificationStep -- Sent verification requests to %d masternodes\n", nCount);
}

// This function tries to find masternodes with the same addr,
// find the lower ban score one and ban all the others.
void CMasternodeMan::CheckSameAddr()
{
    if(!masternodeSync.IsSynced() || mapMasternodes.empty()) return;

    int mncount = 0;
    std::vector<CMasternode*> vBan;
    std::vector<CMasternode*> vSortedByAddr;
    std::vector<CMasternode*> vSortedByPoSe;
    std::map< CNetAddr, CMasternode*> mapAskForMnv;

    {
        LOCK(cs);

        CMasternode* pprevMasternode = NULL;        
        std::pair<int, CMasternode*> pLowerPoSeBanScoreMasternode = std::make_pair( -1, pprevMasternode);

        for (auto& mnpair : mapMasternodes) {
            // do not auto-ban myself
            if(mnpair.second.outpoint == activeMasternode.outpoint) {
                continue;
            } else {
                // someone else is using my address
                if(mnpair.second.addr == activeMasternode.service) {
                    LogPrintf("CMasternodeMan::CheckSameAddr -- Ban masternode %s, at my addr %s\n",
                        mnpair.second.outpoint.ToStringShort(),mnpair.second.addr.ToString());
                    mnpair.second.PoSeBan();
                    continue;
                } else {
                    vSortedByAddr.push_back(&mnpair.second);
                    vSortedByPoSe.push_back(&mnpair.second);
                }
            }
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());
        sort(vSortedByPoSe.begin(), vSortedByPoSe.end(), CompareByPoSeBanScore());        

        for (const auto& pmn : vSortedByAddr) {
            // check all valid masternodes
            if(pmn->IsOutpointSpent() || pmn->IsUpdateRequired() || pmn->IsPoSeBanned()) continue;
            mncount++;
            // initial step
            if(!pprevMasternode) {
                pprevMasternode = pmn;
                std::pair<bool, int> result = findInVector<CMasternode*>( vSortedByPoSe, pmn);
                pLowerPoSeBanScoreMasternode = std::make_pair( result.second, pmn);
                continue;
            }
            // second+ step
            CNetAddr ippmm = (CNetAddr)(pmn->addr);
            CNetAddr ippvMn = (CNetAddr)(pprevMasternode->addr);
            std::pair<bool, int> result = findInVector<CMasternode*>( vSortedByPoSe, pmn);
            std::pair<int, CMasternode*> pLPSBSMasternode = std::make_pair( result.second, pmn);

            if(ippmm == ippvMn) {
                if(pLPSBSMasternode.first > pLowerPoSeBanScoreMasternode.first) {
                    // previous masternode with same ip have lower ban score, ban this one
                    vBan.push_back(pmn);
                } else {
                    // this masternode with the same ip have lower ban score, ban previous one
                    vBan.push_back(pprevMasternode);
                    // and keep a reference to be able to ban following masternodes with the same ip
                    pLowerPoSeBanScoreMasternode = pLPSBSMasternode;
                }
                mapAskForMnv.emplace(ippmm,pLowerPoSeBanScoreMasternode.second);
            } else {
                // update new 1st search address
                pLowerPoSeBanScoreMasternode = pLPSBSMasternode;
            }
            pprevMasternode = pmn;
        }
    }

    int i = (int)vBan.size();
    int j = (int)vSortedByAddr.size();
    LogPrintf("CMasternodeMan::CheckSameAddr -- PoSe ban list num: %d from %d mnodes of total:%d\n", i, mncount, j);
    // ban duplicates
    for (auto& pmn : vBan) {
        LogPrintf("CMasternodeMan::CheckSameAddr -- PoSe ban for masternode %s\n", pmn->outpoint.ToStringShort());
        pmn->PoSeBan();
    }

    // AskForMnv duplicate PoSeBanScore winners to Verify themselfs
    for (auto& pmn : mapAskForMnv) {
        if (MnCheckConnect(pmn.second)) {
            // ask these MNs to verify when possible
            LogPrintf("CMasternodeMan::CheckSameAddr -- should be asked mnv masternode %s, addr %s\n", pmn.second->outpoint.ToStringShort(), pmn.second->addr.ToString());
            mapWeShouldAskForVerification.emplace(pmn.second->outpoint, GetTime());
            //AskForMnv(pmn.second->addr, pmn.second->outpoint);
        } else {
            LogPrintf("CMasternodeMan::CheckSameAddr -- inc.PoSeBanScore, could not mnv masternode %s, addr %s\n", pmn.second->outpoint.ToStringShort(), pmn.second->addr.ToString());
            // could not check if MN is a true MN
            pmn.second->IncreasePoSeBanScore();
        }
    }

}

void CMasternodeMan::CheckMissingMasternodes()
{
    if(!masternodeSync.IsSynced() || mapMasternodes.empty()) return;

    int mncount = 0;
    std::vector<CMasternode*> vBan;
    std::vector<CMasternode*> vSortedByAddr;

    {
        LOCK(cs);

        for (auto& mnpair : mapMasternodes) {
            // do not auto-ban myself
            if(mnpair.second.outpoint == activeMasternode.outpoint) {
                continue;
            } else {
                // someone else is using my address
                if(mnpair.second.addr == activeMasternode.service) {
                    LogPrintf("CMasternodeMan::CheckMissingMasternodes -- Ban masternode %s, at my addr %s\n",
                        mnpair.second.outpoint.ToStringShort(),mnpair.second.addr.ToString());
                    mnpair.second.PoSeBan();
                    continue;
                } else vSortedByAddr.push_back(&mnpair.second);
            }
        }
        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());
        for (const auto& pmn : vSortedByAddr) {
            // check only valid masternodes
            if(pmn->IsOutpointSpent() || pmn->IsUpdateRequired() || pmn->IsPoSeBanned()) continue;
            mncount++;
            auto it = mapMissingMNs.find(pmn->addr);
            if (it != mapMissingMNs.end()) {
                if((it->second == 111 || it->second == 13 || it->second == 113)
                  && !pmn->addr.IsLocal() && pmn->addr.IsRoutable()
                  && ((fOkIPv4 && pmn->addr.IsIPv4())||(fOkIPv6 && pmn->addr.IsIPv6()))){
                    vBan.push_back(pmn);
                    mapMissingMNs.erase(pmn->addr);
                  }
            }
        }

    } // end LOCK

    int i = (int)vBan.size();
    int j = (int)vSortedByAddr.size();
    LogPrintf("CMasternodeMan::CheckMissingMasternodes -- Increase PoSe Ban Score list num: %d from %d (valid mn) of total:%d\n", i, mncount, j);

    // ban missing service Masternodes
    for (auto& pmn : vBan) {
        LogPrintf("CMasternodeMan::CheckMissingMasternodes -- Increase PoSe Ban Score for masternode %s\n", pmn->outpoint.ToStringShort());
        pmn->IncreasePoSeBanScore();
    }
}

bool CMasternodeMan::VerifyRequest(const CAddress& addr, CConnman& connman)
{
    if(netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, but we can not skip it
        LogPrintf("CMasternodeMan::SendVerifyRequest -- do we repeat request, just asking... addr=%s\n", addr.ToString());
        // now, this is a little misbehaving only, we as real nodes we send requests
        // return false;
    }

    return !connman.IsMasternodeOrDisconnectRequested(addr);
}

void CMasternodeMan::ProcessPendingMnvRequests(CConnman& connman)
{
    LOCK(cs_mapPendingMNV);
	//LOCK2(cs_mapPendingMNV,cs_main);

    std::map<CService, std::pair<int64_t, CMasternodeVerification> >::iterator itPendingMNV = mapPendingMNV.begin();

    while (itPendingMNV != mapPendingMNV.end()) {
        bool fDoneSending = connman.ForNode(itPendingMNV->first, [&](CNode* pnode) {
            netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
            // use random nonce, store it and require node to reply with correct one later
            mWeAskedForVerification[pnode->addr] = itPendingMNV->second.second;
            LogPrintf("CMasternodeMan::%s -- verifying node using nonce %d addr=%s\n", __func__, itPendingMNV->second.second.nonce, pnode->addr.ToString());
            CNetMsgMaker msgMaker(pnode->GetSendVersion()); // TODO this gives a warning about version not being set (we should wait for VERSION exchange)
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::MNVERIFY, itPendingMNV->second.second));
            return true;
        });

        int64_t nTimeAdded = itPendingMNV->second.first;
        int64_t nTimePassed = GetTime() - nTimeAdded;
        bool fOver15sPassed = nTimePassed > 15;
        if (fDoneSending || fOver15sPassed) {
            if (!fDoneSending) {
                LogPrintf("CMasternodeMan::%s -- failed to connect to %s, %i sec\n", __func__, itPendingMNV->first.ToString(),(int)nTimePassed);
                // Requires cs. Punish not reachable MN.
                IncreasePoSeBanScore(itPendingMNV->first);
                // Requires cs_main. Punish not reachable Node-peer
                PunishNode(itPendingMNV->first,connman);
                // give up mnv request
                mapPendingMNV.erase(itPendingMNV++);
            } else { // fDoneSending
                bool fMnvRequest = netfulfilledman.HasFulfilledRequest(itPendingMNV->first, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
                bool fMnvDone    = netfulfilledman.HasFulfilledRequest(itPendingMNV->first, strprintf("%s", NetMsgType::MNVERIFY)+"-done");
                if( fMnvRequest && fMnvDone ) { // MNV request && done
                    // once done: copy of the mnv is at:
                    // mWeAskedForVerification[pnode->addr] = mnv;
                    // mapSeenMasternodeVerification.insert(std::make_pair(mnv.GetHash(), mnv));
                    LogPrintf("CMasternodeMan::%s -- done verify from %s in %i sec\n", __func__, itPendingMNV->first.ToString(),(int)nTimePassed);
                    mapPendingMNV.erase(itPendingMNV++);

                } else { // MNV was ignored or failed
                    LogPrintf("CMasternodeMan::%s -- still pending from %s, %i sec\n", __func__, itPendingMNV->first.ToString(),(int)nTimePassed);
                    if (fOver15sPassed) {
                        // Requires cs. Punish not replying or failing MN.
                        IncreasePoSeBanScore(itPendingMNV->first);
                        // Requires cs_main. Punish not replying or failing Node-peer
                        PunishNode(itPendingMNV->first,connman);
                        // give up mnv request
                        mapPendingMNV.erase(itPendingMNV++);
                    }
                    // Retry: re-ProcessPendingMnvRequests (re-send MNV)
                }
            }
            // in case not send and not received ProcessPendingMnvRequests will be called every 1 sec/clockTick
        } else {
            // process next in PendingMNV list
            ++itPendingMNV;
        }
    }
    LogPrintf("CMasternodeMan::%s -- mapPendingMNV size: %d\n", __func__, mapPendingMNV.size());
}

void CMasternodeMan::SendVerifyReply(CNode* pnode, CMasternodeVerification& mnv, CConnman& connman)
{
    AssertLockHeld(cs_main);

    // only masternodes can sign this, why would someone ask regular node?
    if(!fMasternodeMode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply")) {
        // peer should not ask us that often
        LogPrintf("CMasternodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        // it is a little misbehaving only, probable only real nodes will send a request
        Misbehaving(pnode->id, 02);        
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        LogPrintf("CMasternodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    std::string strError;

    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        uint256 hash = mnv.GetSignatureHash1(blockHash);

        if(!CHashSigner::SignHash(hash, activeMasternode.keyMasternode, mnv.vchSig1)) {
            LogPrintf("CMasternodeMan::SendVerifyReply -- SignHash() failed\n");
            return;
        }

        if (!CHashSigner::VerifyHash(hash, activeMasternode.pubKeyMasternode, mnv.vchSig1, strError)) {
            LogPrintf("CMasternodeMan::SendVerifyReply -- VerifyHash() failed, error: %s\n", strError);
            return;
        }
    } else {
        std::string strMessage = strprintf("%s%d%s", activeMasternode.service.ToString(false), mnv.nonce, blockHash.ToString());

        if(!CMessageSigner::SignMessage(strMessage, mnv.vchSig1, activeMasternode.keyMasternode)) {
            LogPrintf("CMasternodeMan::SendVerifyReply -- SignMessage() failed\n");
            return;
        }

        if(!CMessageSigner::VerifyMessage(activeMasternode.pubKeyMasternode, mnv.vchSig1, strMessage, strError)) {
            LogPrintf("CMasternodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
            return;
        }
    }

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::MNVERIFY, mnv));
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply");
}

void CMasternodeMan::ProcessVerifyReply(CNode* pnode, CMasternodeVerification& mnv)
{
    AssertLockHeld(cs_main);

    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if(!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        LogPrintf("CMasternodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        // we could have crashed and lost the copy requestd
        // it is a little misbehaving only, probable only real nodes will send a reply
        Misbehaving(pnode->id, 02);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nonce != mnv.nonce) {
        LogPrintf("CMasternodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d, %s\n",
                    mWeAskedForVerification[pnode->addr].nonce, mnv.nonce, pnode->id, pnode->addr.ToString());
        // Requires cs. Punish wrong MN answer.
        IncreasePoSeBanScore((CService)pnode->addr);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nBlockHeight != mnv.nBlockHeight) {
        LogPrintf("CMasternodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d, %s\n",
                    mWeAskedForVerification[pnode->addr].nBlockHeight, mnv.nBlockHeight, pnode->id, pnode->addr.ToString());
        // Requires cs. Punish wrong MN answer.
        IncreasePoSeBanScore((CService)pnode->addr);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("CMasternodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d, %s\n",
                    mnv.nBlockHeight, pnode->id, pnode->addr.ToString());
        return;
    }

    // we already verified this address, why node is spamming?
    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done")) {
        LogPrintf("CMasternodeMan::ProcessVerifyReply -- WARN: already verified %s recently\n", pnode->addr.ToString());
        // it is a little misbehaving only, probable only real nodes will send a reply
        Misbehaving(pnode->id, 02);
        // process the reply anyway
    }

    {
        LOCK(cs);

        CMasternode* prealMasternode = NULL;
        std::vector<CMasternode*> vpMasternodesToBan;

        uint256 hash1 = mnv.GetSignatureHash1(blockHash);
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(false), mnv.nonce, blockHash.ToString());

        for (auto& mnpair : mapMasternodes) {
            if(CAddress(mnpair.second.addr, NODE_NETWORK) == pnode->addr) {
                bool fFound = false;
                if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
                    fFound = CHashSigner::VerifyHash(hash1, mnpair.second.pubKeyMasternode, mnv.vchSig1, strError);
                    // we don't care about mnv with signature in old format
                } else {
                    fFound = CMessageSigner::VerifyMessage(mnpair.second.pubKeyMasternode, mnv.vchSig1, strMessage1, strError);
                }
                if (fFound) {
                    // found it!
                    prealMasternode = &mnpair.second;
                    if(!mnpair.second.IsPoSeVerified()) {
                        mnpair.second.DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done");

                    // we can only broadcast it if we are an activated masternode
                    if(activeMasternode.outpoint.IsNull()) continue;
                    // update ...
                    mnv.addr = mnpair.second.addr;
                    mnv.masternodeOutpoint1 = mnpair.second.outpoint;
                    mnv.masternodeOutpoint2 = activeMasternode.outpoint;
                    // ... and sign it
                    std::string strError;

                    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
                        uint256 hash2 = mnv.GetSignatureHash2(blockHash);

                        if(!CHashSigner::SignHash(hash2, activeMasternode.keyMasternode, mnv.vchSig2)) {
                            LogPrintf("CMasternodeMan::ProcessVerifyReply -- SignHash() failed\n");
                            return;
                        }

                        if(!CHashSigner::VerifyHash(hash2, activeMasternode.pubKeyMasternode, mnv.vchSig2, strError)) {
                            LogPrintf("CMasternodeMan::ProcessVerifyReply -- VerifyHash() failed, error: %s\n", strError);
                            return;
                        }
                    } else {
                        std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString(),
                                                mnv.masternodeOutpoint1.ToStringShort(), mnv.masternodeOutpoint2.ToStringShort());

                        if(!CMessageSigner::SignMessage(strMessage2, mnv.vchSig2, activeMasternode.keyMasternode)) {
                            LogPrintf("CMasternodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                            return;
                        }

                        if(!CMessageSigner::VerifyMessage(activeMasternode.pubKeyMasternode, mnv.vchSig2, strMessage2, strError)) {
                            LogPrintf("CMasternodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                            return;
                        }
                    }

                    mWeAskedForVerification[pnode->addr] = mnv;
                    mapSeenMasternodeVerification.insert(std::make_pair(mnv.GetHash(), mnv));
                    mnv.Relay();

                } else {
                    vpMasternodesToBan.push_back(&mnpair.second);
                }
            }
        }
        // real masternode found?...
        if(prealMasternode) {
            LogPrintf("CMasternodeMan::ProcessVerifyReply -- verified real masternode %s for addr %s\n",
                    prealMasternode->outpoint.ToStringShort(), pnode->addr.ToString());
        }
        else {
            // no real masternode found?...
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CMasternodeMan::ProcessVerifyReply -- ERROR: no real masternode found for addr %s\n", pnode->addr.ToString());
            // negative verify costs reputation
            Misbehaving(pnode->id, 40);
            // return;
        }
        // increase ban score for everyone else found to be fake
        for (const auto& pmn : vpMasternodesToBan) {
            pmn->IncreasePoSeBanScore();
            LogPrintf("CMasternodeMan::ProcessVerifyReply -- increased PoSe ban score for %s addr %s, new score %d\n",
                        pmn->outpoint.ToStringShort(), pmn->addr.ToString(), pmn->nPoSeBanScore);
        }
        if(!vpMasternodesToBan.empty())
            LogPrintf("CMasternodeMan::ProcessVerifyReply -- PoSe score increased for %d fake masternodes, addr %s\n",
                        (int)vpMasternodesToBan.size(), pnode->addr.ToString());
    }
}

void CMasternodeMan::ProcessVerifyBroadcast(CNode* pnode, const CMasternodeVerification& mnv)
{
    AssertLockHeld(cs_main);

    std::string strError;

    if(mapSeenMasternodeVerification.find(mnv.GetHash()) != mapSeenMasternodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenMasternodeVerification[mnv.GetHash()] = mnv;

    // we don't care about history
    if(mnv.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
        LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d, %s\n",
                    nCachedBlockHeight, mnv.nBlockHeight, pnode->id, pnode->addr.ToString());
        return;
    }

    if(mnv.masternodeOutpoint1 == mnv.masternodeOutpoint2) {
        LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- ERROR: same outpoints %s, peer=%d, %s\n",
                    mnv.masternodeOutpoint1.ToStringShort(), pnode->id, pnode->addr.ToString());
        // that was NOT a good idea to cheat and verify itself
        // Requires cs. Punish wrong MN behaviour, but can be victim of a DoS by 3rd party as well
        // PoSeBan(mnv.masternodeOutpoint1);
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d, %s\n",
                    mnv.nBlockHeight, pnode->id, pnode->addr.ToString());
        return;
    }

    int nRank;

    if (!GetMasternodeRank(mnv.masternodeOutpoint2, nRank, mnv.nBlockHeight, MIN_POSE_PROTO_VERSION)) {
        LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- Can't calculate rank for masternode %s\n",
                    mnv.masternodeOutpoint2.ToStringShort());
        return;
    }

    if(nRank > MAX_POSE_RANK) {
        LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- Masternode %s is not in top %d, current rank %d, peer=%d, %s\n",
                    mnv.masternodeOutpoint2.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->id, pnode->addr.ToString());
        return;
    }

    {
        LOCK(cs);

        CMasternode* pmn1 = Find(mnv.masternodeOutpoint1);
        if(!pmn1) {
            LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- can't find masternode1 %s\n", mnv.masternodeOutpoint1.ToStringShort());
            return;
        }

        CMasternode* pmn2 = Find(mnv.masternodeOutpoint2);
        if(!pmn2) {
            LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- can't find masternode2 %s\n", mnv.masternodeOutpoint2.ToStringShort());
            return;
        }

        if(pmn1->addr != mnv.addr) {
            LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- mnv addr %s does not match our %s for mn1 %s\n",
                                mnv.addr.ToString(), pmn1->addr.ToString(), mnv.masternodeOutpoint1.ToStringShort());
            // node mn2 is relaying/broadcasting wrong information, but can be victim of a DoS by 3rd party as well
            // pmn2->IncreasePoSeBanScore();
            // peer pnode-id is also helping spreading the wrong information
            Misbehaving(pnode->id, 20);
            return;
        }

        if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
            uint256 hash1 = mnv.GetSignatureHash1(blockHash);
            uint256 hash2 = mnv.GetSignatureHash2(blockHash);

            if(!CHashSigner::VerifyHash(hash1, pmn1->pubKeyMasternode, mnv.vchSig1, strError)) {
                LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- VerifyHash() failed, error: %s\n", strError);
                return;
            }

            if(!CHashSigner::VerifyHash(hash2, pmn2->pubKeyMasternode, mnv.vchSig2, strError)) {
                LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- VerifyHash() failed, error: %s\n", strError);
                return;
            }
        } else {
            std::string strMessage1 = strprintf("%s%d%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString());
            std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString(),
                                    mnv.masternodeOutpoint1.ToStringShort(), mnv.masternodeOutpoint2.ToStringShort());

            if(!CMessageSigner::VerifyMessage(pmn1->pubKeyMasternode, mnv.vchSig1, strMessage1, strError)) {
                LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- VerifyMessage() for masternode1 failed, error: %s\n", strError);
                return;
            }

            if(!CMessageSigner::VerifyMessage(pmn2->pubKeyMasternode, mnv.vchSig2, strMessage2, strError)) {
                LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- VerifyMessage() for masternode2 failed, error: %s\n", strError);
                return;
            }
        }

        if(!pmn1->IsPoSeVerified()) {
            pmn1->DecreasePoSeBanScore();
        }
        mnv.Relay();

        LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- verified masternode %s for addr %s\n",
                    pmn1->outpoint.ToStringShort(), pmn1->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        for (auto& mnpair : mapMasternodes) {
            if(mnpair.second.addr != mnv.addr || mnpair.first == mnv.masternodeOutpoint1) continue;
            mnpair.second.IncreasePoSeBanScore();
            nCount++;
            LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        mnpair.first.ToStringShort(), mnpair.second.addr.ToString(), mnpair.second.nPoSeBanScore);
        }
        if(nCount)
            LogPrintf("CMasternodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake masternodes, addr %s\n",
                        nCount, pmn1->addr.ToString());
    }
}

std::string CMasternodeMan::ToString() const
{
    std::ostringstream info;

    info << "Masternodes: " << (int)mapMasternodes.size() <<
            ", peers who asked us for Masternode list: " << (int)mAskedUsForMasternodeList.size() <<
            ", peers we asked for Masternode list: " << (int)mWeAskedForMasternodeList.size() <<
            ", entries in Masternode list we asked for: " << (int)mWeAskedForMasternodeListEntry.size() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}

bool CMasternodeMan::CheckMnbAndUpdateMasternodeList(CNode* pfrom, CMasternodeBroadcast mnb, int& nDos, CConnman& connman)
{
    // Need to lock cs_main here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK(cs_main);

    {
        LOCK(cs);
        nDos = 0;
        LogPrint("masternode", "CMasternodeMan::CheckMnbAndUpdateMasternodeList -- masternode=%s\n", mnb.outpoint.ToStringShort());

        uint256 hash = mnb.GetHash();
        if(mapSeenMasternodeBroadcast.count(hash) && !mnb.fRecovery) { //seen
            LogPrint("masternode", "CMasternodeMan::CheckMnbAndUpdateMasternodeList -- masternode=%s seen\n", mnb.outpoint.ToStringShort());
            // less then 2 pings left before this MN goes into non-recoverable state, bump sync timeout
            if(GetTime() - mapSeenMasternodeBroadcast[hash].first > MASTERNODE_NEW_START_REQUIRED_SECONDS - MASTERNODE_MIN_MNP_SECONDS * 2) {
                LogPrint("masternode", "CMasternodeMan::CheckMnbAndUpdateMasternodeList -- masternode=%s seen update\n", mnb.outpoint.ToStringShort());
                mapSeenMasternodeBroadcast[hash].first = GetTime();
                masternodeSync.BumpAssetLastTime("CMasternodeMan::CheckMnbAndUpdateMasternodeList - seen");
            }
            // did we ask this node for it?
            if(pfrom && IsMnbRecoveryRequested(hash) && GetTime() < mMnbRecoveryRequests[hash].first) {
                LogPrint("masternode", "CMasternodeMan::CheckMnbAndUpdateMasternodeList -- mnb=%s seen request\n", hash.ToString());
                if(mMnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrint("masternode", "CMasternodeMan::CheckMnbAndUpdateMasternodeList -- mnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same mnb multiple times in recovery mode
                    mMnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if(mnb.lastPing.sigTime > mapSeenMasternodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CMasternode mnTemp = CMasternode(mnb);
                        mnTemp.Check();
                        LogPrint("masternode", "CMasternodeMan::CheckMnbAndUpdateMasternodeList -- mnb=%s seen request, addr=%s, better lastPing: %d min ago, projected mn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetAdjustedTime() - mnb.lastPing.sigTime)/60, mnTemp.GetStateString());
                        if(mnTemp.IsValidStateForAutoStart(mnTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrint("masternode", "CMasternodeMan::CheckMnbAndUpdateMasternodeList -- masternode=%s seen good\n", mnb.outpoint.ToStringShort());
                            mMnbRecoveryGoodReplies[hash].push_back(mnb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenMasternodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));

        LogPrintf("CMasternodeMan::CheckMnbAndUpdateMasternodeList -- masternode=%s new\n", mnb.outpoint.ToStringShort());

        if(!mnb.SimpleCheck(nDos)) {
            LogPrintf("CMasternodeMan::CheckMnbAndUpdateMasternodeList -- SimpleCheck() failed, masternode=%s\n", mnb.outpoint.ToStringShort());
            return false;
        }

        // search Masternode list
        CMasternode* pmn = Find(mnb.outpoint);
        if(pmn) {
            CMasternodeBroadcast mnbOld = mapSeenMasternodeBroadcast[CMasternodeBroadcast(*pmn).GetHash()].second;
            if(!mnb.Update(pmn, nDos, connman)) {
                LogPrintf("CMasternodeMan::CheckMnbAndUpdateMasternodeList -- Update() failed, masternode=%s\n", mnb.outpoint.ToStringShort());
                return false;
            }
            if(hash != mnbOld.GetHash()) {
                mapSeenMasternodeBroadcast.erase(mnbOld.GetHash());
            }
            return true;
        }
    }

    if(mnb.CheckOutpoint(nDos) &&  mnb.CheckAddr(nDos)) {
        if(Add(mnb)){
            masternodeSync.BumpAssetLastTime("CMasternodeMan::CheckMnbAndUpdateMasternodeList - new");
            // if it matches our Masternode privkey...
            if(fMasternodeMode && mnb.pubKeyMasternode == activeMasternode.pubKeyMasternode) {
                mnb.nPoSeBanScore = -MASTERNODE_POSE_BAN_MAX_SCORE;
                if(mnb.nProtocolVersion == PROTOCOL_VERSION) {
                    // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                    LogPrintf("CMasternodeMan::CheckMnbAndUpdateMasternodeList -- Got NEW Masternode entry: masternode=%s  sigTime=%lld  addr=%s\n",
                              mnb.outpoint.ToStringShort(), mnb.sigTime, mnb.addr.ToString());
                    activeMasternode.ManageState(connman);
                } else {
                    // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                    // but also do not ban the node we get this message from
                    LogPrintf("CMasternodeMan::CheckMnbAndUpdateMasternodeList -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", mnb.nProtocolVersion, PROTOCOL_VERSION);
                    return false;
                }
            }
            mnb.Relay(connman);
        } else {
            LogPrintf("CMasternodeMan::CheckMnbAndUpdateMasternodeList -- Rejected Add Masternode entry: %s  addr=%s\n", mnb.outpoint.ToStringShort(), mnb.addr.ToString());
            return false;
        }
    } else {
        LogPrintf("CMasternodeMan::CheckMnbAndUpdateMasternodeList -- Rejected Masternode entry: %s  addr=%s\n", mnb.outpoint.ToStringShort(), mnb.addr.ToString());
        return false;
    }

    return true;
}

void CMasternodeMan::UpdateLastPaid(const CBlockIndex* pindex)
{
    LOCK(cs);

    if(fLiteMode || !masternodeSync.IsWinnersListSynced() || mapMasternodes.empty()) return;

    static int nLastRunBlockHeight = 0;
    // Scan at least LAST_PAID_SCAN_BLOCKS but no more than mnpayments.GetStorageLimit()
    int nMaxBlocksToScanBack = std::max(LAST_PAID_SCAN_BLOCKS, nCachedBlockHeight - nLastRunBlockHeight);
    nMaxBlocksToScanBack = std::min(nMaxBlocksToScanBack, mnpayments.GetStorageLimit());

    LogPrint("masternode", "CMasternodeMan::UpdateLastPaid -- nCachedBlockHeight=%d, nLastRunBlockHeight=%d, nMaxBlocksToScanBack=%d\n",
                            nCachedBlockHeight, nLastRunBlockHeight, nMaxBlocksToScanBack);

    for (auto& mnpair : mapMasternodes) {
        mnpair.second.UpdateLastPaid(pindex, nMaxBlocksToScanBack);
    }

    nLastRunBlockHeight = nCachedBlockHeight;
}

void CMasternodeMan::UpdateLastSentinelPingTime()
{
    LOCK(cs);
    nLastSentinelPingTime = GetTime();
}

bool CMasternodeMan::IsSentinelPingActive()
{
    LOCK(cs);
    // Check if any masternodes have voted recently, otherwise return false
    return (GetTime() - nLastSentinelPingTime) <= MASTERNODE_SENTINEL_PING_MAX_SECONDS;
}

bool CMasternodeMan::AddGovernanceVote(const COutPoint& outpoint, uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if(!pmn) {
        return false;
    }
    pmn->AddGovernanceVote(nGovernanceObjectHash);
    return true;
}

void CMasternodeMan::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    for(auto& mnpair : mapMasternodes) {
        mnpair.second.RemoveGovernanceObject(nGovernanceObjectHash);
    }
}

void CMasternodeMan::CheckMasternode(const CPubKey& pubKeyMasternode, bool fForce)
{
    LOCK2(cs_main, cs);
    for (auto& mnpair : mapMasternodes) {
        if (mnpair.second.pubKeyMasternode == pubKeyMasternode) {
            mnpair.second.Check(fForce);
            return;
        }
    }
}

bool CMasternodeMan::IsMasternodePingedWithin(const COutPoint& outpoint, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    return pmn ? pmn->IsPingedWithin(nSeconds, nTimeToCheckAt) : false;
}

void CMasternodeMan::SetMasternodeLastPing(const COutPoint& outpoint, const CMasternodePing& mnp)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if(!pmn) {
        return;
    }
    pmn->lastPing = mnp;
    if(mnp.fSentinelIsCurrent) {
        UpdateLastSentinelPingTime();
    }
    mapSeenMasternodePing.insert(std::make_pair(mnp.GetHash(), mnp));

    CMasternodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if(mapSeenMasternodeBroadcast.count(hash)) {
        mapSeenMasternodeBroadcast[hash].second.lastPing = mnp;
    }
}

void CMasternodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    nCachedBlockHeight = pindex->nHeight;
    LogPrint("masternode", "CMasternodeMan::UpdatedBlockTip -- nCachedBlockHeight=%d\n", nCachedBlockHeight);

    CheckSameAddr();

    if(fMasternodeMode) {
        // normal wallet does not need to update this every block, doing update on rpc call should be enough
        UpdateLastPaid(pindex);
    }
}

void CMasternodeMan::WarnMasternodeDaemonUpdates()
{
    LOCK(cs);

    static bool fWarned = false;

    if (fWarned || !size() || !masternodeSync.IsMasternodeListSynced())
        return;

    int nUpdatedMasternodes{0};

    for (const auto& mnpair : mapMasternodes) {
        if (mnpair.second.lastPing.nDaemonVersion > CLIENT_VERSION) {
            ++nUpdatedMasternodes;
        }
    }

    // Warn only when at least half of known masternodes already updated
    if (nUpdatedMasternodes < size() / 2)
        return;

    std::string strWarning;
    if (nUpdatedMasternodes != size()) {
        strWarning = strprintf(_("Warning: At least %d of %d masternodes are running on a newer software version. Please check latest releases, you might need to update too."),
                    nUpdatedMasternodes, size());
    } else {
        // someone was postponing this update for way too long probably
        strWarning = strprintf(_("Warning: Every masternode (out of %d known ones) is running on a newer software version. Please check latest releases, it's very likely that you missed a major/critical update."),
                    size());
    }

    // notify GetWarnings(), called by Qt and the JSON-RPC code to warn the user
    SetMiscWarning(strWarning);
    // trigger GUI update
    uiInterface.NotifyAlertChanged(SerializeHash(strWarning), CT_NEW);
    // trigger cmd-line notification
    //AlertNotify(strWarning, CT_NEW);

    fWarned = true;
}

void CMasternodeMan::NotifyMasternodeUpdates(CConnman& connman)
{
    // Avoid double locking
    bool fMasternodesAddedLocal = false;
    bool fMasternodesRemovedLocal = false;
    {
        LOCK(cs);
        fMasternodesAddedLocal = fMasternodesAdded;
        fMasternodesRemovedLocal = fMasternodesRemoved;
    }

    if(fMasternodesAddedLocal) {
        governance.CheckMasternodeOrphanObjects(connman);
        governance.CheckMasternodeOrphanVotes(connman);
    }
    if(fMasternodesRemovedLocal) {
        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fMasternodesAdded = false;
    fMasternodesRemoved = false;
}
