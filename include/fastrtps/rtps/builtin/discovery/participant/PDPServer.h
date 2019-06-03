// Copyright 2019 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file PDPServer.h
 *
 */

#ifndef PDPSERVER_H_
#define PDPSERVER_H_
#ifndef DOXYGEN_SHOULD_SKIP_THIS_PUBLIC

#include "PDP.h"
#include <fastrtps/rtps/messages/RTPSMessageGroup.h>

#include "timedevent/DServerEvent.h"

 // TODO: remove when the Writer API issue is resolved
#include <fastrtps/rtps/attributes/WriterAttributes.h>

namespace eprosima {
namespace fastrtps{
namespace rtps {

class StatefulWriter;
class StatefulReader;

/**
 * Class PDPServer manages server side of the discovery server mechanism
 *@ingroup DISCOVERY_MODULE
 */
class PDPServer : public PDP
{
    friend class DServerEvent;
    friend class PDPServerListener;

    typedef std::set<const ParticipantProxyData*> pending_matches_list;
    typedef std::set<InstanceHandle_t> key_list;

    //! EDP pending matches
    pending_matches_list _p2match;

    //! Keys to wipe out from WriterHistory because its related Participants have been removed
    key_list _demises;

    //! TRANSIENT or TRANSIENT_LOCAL durability;
    DurabilityKind_t _durability;

    //! Messages announcement ancillary
    RTPSMessageGroup_t _msgbuffer;

    //! Temporary locator list to solve new Writer API issue
    // TODO: remove when the Writer API issue is resolved
    std::map<GUID_t, RemoteReaderAttributes> clients_;

    public:
    /**
     * Constructor
     * @param builtin Pointer to the BuiltinProcols object.
     */
    PDPServer(
        BuiltinProtocols* builtin,
        DurabilityKind_t durability_kind = TRANSIENT_LOCAL);
    ~PDPServer();

    void initializeParticipantProxyData(ParticipantProxyData* participant_data) override;

    /**
     * Initialize the PDP.
     * @param part Pointer to the RTPSParticipant.
     * @return True on success
     */
    bool initPDP(RTPSParticipantImpl* part) override;

    /**
     * Creates an initializes a new participant proxy from a DATA(p) raw info
     * @param ParticipantProxyData from DATA msg deserialization
     * @param CacheChange_t from DATA msg
     * @return new ParticipantProxyData * or nullptr on failure
     */
    ParticipantProxyData* createParticipantProxyData(
        const ParticipantProxyData&,
        const CacheChange_t&) override;

    /**
     * Create the SPDP Writer and Reader
     * @return True if correct.
     */
    bool createPDPEndpoints() override;

    /**
     * This method removes a remote RTPSParticipant and all its writers and readers.
     * @param partGUID GUID_t of the remote RTPSParticipant.
     * @return true if correct.
     */
    bool removeRemoteParticipant(GUID_t& partGUID) override;

    /**
     * Methods to update WriterHistory with reader information
     */

    /**
     * Some History data is flag for defer removal till every client
     * acknowledges reception
     * @return True if trimming must be done
     */
    bool pendingHistoryCleaning();

    /**
     *! Callback to remove unnecesary WriterHistory info from PDP and EDP
     * @return True if trimming is completed
     */
    bool trimWriterHistory();

    /**
     * Add participant CacheChange_ts from reader to writer
     * @param metatraffic CacheChange_t
     * @return True if successfully modified WriterHistory
     */
    bool addRelayedChangeToHistory(CacheChange_t&);

    /**
     * Trigger the participant CacheChange_t removal system
     * @param instanceHandle associated with participants CacheChange_ts
     * @return True if successfully modified WriterHistory
     */
    void removeParticipantFromHistory(const InstanceHandle_t&);

    /**
     * Methods to synchronize EDP matching
     */

    /**
     * Add a participant to the queue of pending participants to EDP matching
     * @param ParticipantProxyData associated with the new participant
     */
    void queueParticipantForEDPMatch(const ParticipantProxyData*);

    /**
     * Remove a participant from the queue of pending participants to EDP matching
     * @param ParticipantProxyData associated with the new participant
     */
    void removeParticipantForEDPMatch(const ParticipantProxyData*);

    /**
     * Check if all client have acknowledge the server PDP data
     * @return True if all clients known each other
     */
    bool all_clients_acknowledge_PDP();

    /**
     * Check if there are pending matches.
     * @return True if all participants EDP endpoints are already matched
     */
    inline bool pendingEDPMatches()
    {
        std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);

        return !_p2match.empty();
    }

    //! Matches all clients EDP endpoints
    void match_all_clients_EDP_endpoints();

    /**
     * Methods to synchronize with another servers
     */

    /**
    * Check if all servers have acknowledge this server PDP data
    * This method must be called from a mutex protected context.
    * @return True if all can reach the client
    */
    bool all_servers_acknowledge_PDP();

    /**
     * Check if we have our PDP received data updated
     * This method must be called from a mutex protected context.
     * @return True if we known all the participants the servers are aware of
     */
    bool is_all_servers_PDPdata_updated();

    /**
     * Matching server EDP endpoints
     * @return true if all servers have been discovered
     */
    bool match_servers_EDP_endpoints();

    /**
     * Force the sending of our local PDP to all servers
     * @param new_change If true a new change (with new seqNum) is created and sent; if false the last change is re-sent
     * @param dispose Sets change kind to NOT_ALIVE_DISPOSED_UNREGISTERED
     */
    void announceParticipantState(
        bool new_change,
        bool dispose = false,
        WriteParams& wparams = WriteParams::WRITE_PARAM_DEFAULT) override;

    /**
     * These methods wouldn't be needed under perfect server operation (no need of dynamic endpoint allocation)
     * but must be implemented to solve server shutdown situations.
     * @param pdata Pointer to the RTPSParticipantProxyData object.
     */
    void assignRemoteEndpoints(ParticipantProxyData* pdata) override;
    void removeRemoteEndpoints(ParticipantProxyData * pdata) override;
    void notifyAboveRemoteEndpoints(const ParticipantProxyData& pdata) override;

    //! Get filename for persistence database file
    std::string GetPersistenceFileName();

    //! Wakes up the DServerEvent for new matching or trimming
    void awakeServerThread() { mp_sync->restart_timer(); }

    private:

     /**
     * Callback to remove unnecesary WriterHistory info from PDP alone
     * @return True if trimming is completed
     */
     bool trimPDPWriterHistory();

    /**
    * TimedEvent for server synchronization:
    *   first stage: periodically resend the local RTPSParticipant information until all servers have acknowledge reception
    *   second stage: waiting PDP info is up to date before allowing EDP matching
    */
    DServerEvent* mp_sync;
};

}
} /* namespace rtps */
} /* namespace eprosima */
#endif
#endif /* PDPSERVER_H_ */
