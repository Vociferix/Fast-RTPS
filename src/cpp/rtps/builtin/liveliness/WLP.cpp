// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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
 * @file WLP.cpp
 *
 */
#include <limits>

#include <fastrtps/rtps/builtin/liveliness/WLP.h>
#include <fastrtps/rtps/builtin/liveliness/WLPListener.h>
#include <fastrtps/rtps/builtin/liveliness/timedevent/WLivelinessPeriodicAssertion.h>
#include "../../participant/RTPSParticipantImpl.h"
#include <fastrtps/rtps/writer/StatefulWriter.h>
#include <fastrtps/rtps/writer/LivelinessManager.h>
#include <fastrtps/rtps/writer/WriterListener.h>
#include <fastrtps/rtps/reader/StatefulReader.h>
#include <fastrtps/rtps/history/WriterHistory.h>
#include <fastrtps/rtps/history/ReaderHistory.h>
#include <fastrtps/rtps/resources/ResourceEvent.h>

#include <fastrtps/rtps/builtin/BuiltinProtocols.h>
#include <fastrtps/rtps/builtin/discovery/participant/PDPSimple.h>

#include <fastrtps/rtps/builtin/data/ParticipantProxyData.h>
#include <fastrtps/rtps/builtin/data/WriterProxyData.h>

#include <fastrtps/log/Log.h>
#include <fastrtps/utils/TimeConversion.h>


#include <mutex>

namespace eprosima {
namespace fastrtps{
namespace rtps {


WLP::WLP(BuiltinProtocols* p)
    : min_automatic_ms_(std::numeric_limits<double>::max())
    , min_manual_by_participant_ms_(std::numeric_limits<double>::max())
    , mp_participant(nullptr)
    , mp_builtinProtocols(p)
    , mp_builtinWriter(nullptr)
    , mp_builtinReader(nullptr)
    , mp_builtinWriterHistory(nullptr)
    , mp_builtinReaderHistory(nullptr)
    , automatic_liveliness_assertion_(nullptr)
    , manual_liveliness_assertion_(nullptr)
    , automatic_writers_()
    , manual_by_participant_writers_()
    , manual_by_topic_writers_()
    , readers_()
    , automatic_readers_(false)
    , pub_liveliness_manager_(nullptr)
    , sub_liveliness_manager_(nullptr)
#if HAVE_SECURITY
    , mp_builtinWriterSecure(nullptr)
    , mp_builtinReaderSecure(nullptr)
    , mp_builtinWriterSecureHistory(nullptr)
    , mp_builtinReaderSecureHistory(nullptr)
#endif
{
}

WLP::~WLP()
{
    if (automatic_liveliness_assertion_ != nullptr)
    {
        delete automatic_liveliness_assertion_;
        automatic_liveliness_assertion_ = nullptr;
    }
    if (manual_liveliness_assertion_ != nullptr)
    {
        delete this->manual_liveliness_assertion_;
        manual_liveliness_assertion_ = nullptr;
    }

#if HAVE_SECURITY
    mp_participant->deleteUserEndpoint(mp_builtinReaderSecure);
    mp_participant->deleteUserEndpoint(mp_builtinWriterSecure);
    delete this->mp_builtinReaderSecureHistory;
    delete this->mp_builtinWriterSecureHistory;
#endif
    mp_participant->deleteUserEndpoint(mp_builtinReader);
    mp_participant->deleteUserEndpoint(mp_builtinWriter);
    delete this->mp_builtinReaderHistory;
    delete this->mp_builtinWriterHistory;
    delete this->mp_listener;

    delete pub_liveliness_manager_;
    delete sub_liveliness_manager_;
}

bool WLP::initWL(RTPSParticipantImpl* p)
{
    logInfo(RTPS_LIVELINESS,"Initializing Liveliness Protocol");

    mp_participant = p;

    pub_liveliness_manager_ = new LivelinessManager(
                [&](const GUID_t& guid,
                    const LivelinessQosPolicyKind& kind,
                    const Duration_t& lease_duration,
                    int alive_count,
                    int not_alive_count) -> void
                {
                    pub_liveliness_changed(
                                guid,
                                kind,
                                lease_duration,
                                alive_count,
                                not_alive_count);
                },
                mp_participant->getEventResource().getIOService(),
                mp_participant->getEventResource().getThread(),
                false);

    sub_liveliness_manager_ = new LivelinessManager(
                [&](const GUID_t& guid,
                    const LivelinessQosPolicyKind& kind,
                    const Duration_t& lease_duration,
                    int alive_count,
                    int not_alive_count) -> void
                {
                    sub_liveliness_changed(
                                guid,
                                kind,
                                lease_duration,
                                alive_count,
                                not_alive_count);
                },
                mp_participant->getEventResource().getIOService(),
                mp_participant->getEventResource().getThread());

    bool retVal = createEndpoints();
#if HAVE_SECURITY
    if (retVal) createSecureEndpoints();
#endif
    return retVal;
}

bool WLP::createEndpoints()
{
    // Built-in writer history

    HistoryAttributes hatt;
    hatt.initialReservedCaches = 20;
    hatt.maximumReservedCaches = 1000;
    hatt.payloadMaxSize = BUILTIN_PARTICIPANT_DATA_MAX_SIZE;
    mp_builtinWriterHistory = new WriterHistory(hatt);

    // Built-in writer

    WriterAttributes watt;
    watt.endpoint.unicastLocatorList = mp_builtinProtocols->m_metatrafficUnicastLocatorList;
    watt.endpoint.multicastLocatorList = mp_builtinProtocols->m_metatrafficMulticastLocatorList;
    watt.endpoint.remoteLocatorList = mp_builtinProtocols->m_initialPeersList;
    watt.endpoint.topicKind = WITH_KEY;
    watt.endpoint.durabilityKind = TRANSIENT_LOCAL;
    watt.endpoint.reliabilityKind = RELIABLE;
    if (mp_participant->getRTPSParticipantAttributes().throughputController.bytesPerPeriod != UINT32_MAX &&
            mp_participant->getRTPSParticipantAttributes().throughputController.periodMillisecs != 0)
    {
        watt.mode = ASYNCHRONOUS_WRITER;
    }
    RTPSWriter* wout;
    if (mp_participant->createWriter(
                &wout,
                watt,
                mp_builtinWriterHistory,
                nullptr,
                c_EntityId_WriterLiveliness,
                true))
    {
        mp_builtinWriter = dynamic_cast<StatefulWriter*>(wout);
        logInfo(RTPS_LIVELINESS,"Builtin Liveliness Writer created");
    }
    else
    {
        logError(RTPS_LIVELINESS,"Liveliness Writer Creation failed ");
        delete(mp_builtinWriterHistory);
        mp_builtinWriterHistory = nullptr;
        return false;
    }

    // Built-in reader history

    hatt.initialReservedCaches = 100;
    hatt.maximumReservedCaches = 2000;
    hatt.payloadMaxSize = BUILTIN_PARTICIPANT_DATA_MAX_SIZE;
    mp_builtinReaderHistory = new ReaderHistory(hatt);

    // WLP listener

    mp_listener = new WLPListener(this);

    // Built-in reader

    ReaderAttributes ratt;
    ratt.endpoint.topicKind = WITH_KEY;
    ratt.endpoint.durabilityKind = TRANSIENT_LOCAL;
    ratt.endpoint.reliabilityKind = RELIABLE;
    ratt.expectsInlineQos = true;
    ratt.endpoint.unicastLocatorList =  mp_builtinProtocols->m_metatrafficUnicastLocatorList;
    ratt.endpoint.multicastLocatorList = mp_builtinProtocols->m_metatrafficMulticastLocatorList;
    ratt.endpoint.remoteLocatorList = mp_builtinProtocols->m_initialPeersList;
    ratt.endpoint.topicKind = WITH_KEY;
    RTPSReader* rout;
    if (mp_participant->createReader(
                &rout,
                ratt,
                mp_builtinReaderHistory,
                (ReaderListener*)mp_listener,
                c_EntityId_ReaderLiveliness,
                true))
    {
        mp_builtinReader = dynamic_cast<StatefulReader*>(rout);
        logInfo(RTPS_LIVELINESS,"Builtin Liveliness Reader created");
    }
    else
    {
        logError(RTPS_LIVELINESS,"Liveliness Reader Creation failed.");
        delete(mp_builtinReaderHistory);
        mp_builtinReaderHistory = nullptr;
        delete(mp_listener);
        mp_listener = nullptr;
        return false;
    }

    return true;
}

#if HAVE_SECURITY

bool WLP::createSecureEndpoints()
{
    //CREATE WRITER
    HistoryAttributes hatt;
    hatt.initialReservedCaches = 20;
    hatt.maximumReservedCaches = 1000;
    hatt.payloadMaxSize = BUILTIN_PARTICIPANT_DATA_MAX_SIZE;
    mp_builtinWriterSecureHistory = new WriterHistory(hatt);
    WriterAttributes watt;
    watt.endpoint.unicastLocatorList = mp_builtinProtocols->m_metatrafficUnicastLocatorList;
    watt.endpoint.multicastLocatorList = mp_builtinProtocols->m_metatrafficMulticastLocatorList;
    //	Wparam.topic.topicName = "DCPSParticipantMessageSecure";
    //	Wparam.topic.topicDataType = "RTPSParticipantMessageData";
    watt.endpoint.topicKind = WITH_KEY;
    watt.endpoint.durabilityKind = TRANSIENT_LOCAL;
    watt.endpoint.reliabilityKind = RELIABLE;
    if (mp_participant->getRTPSParticipantAttributes().throughputController.bytesPerPeriod != UINT32_MAX &&
        mp_participant->getRTPSParticipantAttributes().throughputController.periodMillisecs != 0)
        watt.mode = ASYNCHRONOUS_WRITER;

    const security::ParticipantSecurityAttributes& part_attrs = mp_participant->security_attributes();
    security::PluginParticipantSecurityAttributes plugin_attrs(part_attrs.plugin_participant_attributes);
    security::EndpointSecurityAttributes* sec_attrs = &watt.endpoint.security_attributes();
    sec_attrs->is_submessage_protected = part_attrs.is_liveliness_protected;
    if (part_attrs.is_liveliness_protected)
    {
        sec_attrs->plugin_endpoint_attributes |= PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID;
        if (plugin_attrs.is_liveliness_encrypted)
            sec_attrs->plugin_endpoint_attributes |= PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_ENCRYPTED;
        if (plugin_attrs.is_liveliness_origin_authenticated)
            sec_attrs->plugin_endpoint_attributes |= PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_ORIGIN_AUTHENTICATED;
    }

    RTPSWriter* wout;
    if (mp_participant->createWriter(&wout, watt, mp_builtinWriterSecureHistory, nullptr, c_EntityId_WriterLivelinessSecure, true))
    {
        mp_builtinWriterSecure = dynamic_cast<StatefulWriter*>(wout);
        logInfo(RTPS_LIVELINESS, "Builtin Secure Liveliness Writer created");
    }
    else
    {
        logError(RTPS_LIVELINESS, "Secure Liveliness Writer Creation failed ");
        delete(mp_builtinWriterSecureHistory);
        mp_builtinWriterSecureHistory = nullptr;
        return false;
    }
    hatt.initialReservedCaches = 100;
    hatt.maximumReservedCaches = 2000;
    hatt.payloadMaxSize = BUILTIN_PARTICIPANT_DATA_MAX_SIZE;
    mp_builtinReaderSecureHistory = new ReaderHistory(hatt);
    ReaderAttributes ratt;
    ratt.endpoint.topicKind = WITH_KEY;
    ratt.endpoint.durabilityKind = TRANSIENT_LOCAL;
    ratt.endpoint.reliabilityKind = RELIABLE;
    ratt.expectsInlineQos = true;
    ratt.endpoint.unicastLocatorList = mp_builtinProtocols->m_metatrafficUnicastLocatorList;
    ratt.endpoint.multicastLocatorList = mp_builtinProtocols->m_metatrafficMulticastLocatorList;
    //Rparam.topic.topicName = "DCPSParticipantMessageSecure";
    //Rparam.topic.topicDataType = "RTPSParticipantMessageData";
    ratt.endpoint.topicKind = WITH_KEY;
    sec_attrs = &ratt.endpoint.security_attributes();
    sec_attrs->is_submessage_protected = part_attrs.is_liveliness_protected;
    if (part_attrs.is_liveliness_protected)
    {
        sec_attrs->plugin_endpoint_attributes |= PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID;
        if (plugin_attrs.is_liveliness_encrypted)
            sec_attrs->plugin_endpoint_attributes |= PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_ENCRYPTED;
        if (plugin_attrs.is_liveliness_origin_authenticated)
            sec_attrs->plugin_endpoint_attributes |= PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_ORIGIN_AUTHENTICATED;
    }
    RTPSReader* rout;
    if (mp_participant->createReader(
                &rout,
                ratt,
                mp_builtinReaderSecureHistory,
                (ReaderListener*)mp_listener,
                c_EntityId_ReaderLivelinessSecure,
                true))
    {
        mp_builtinReaderSecure = dynamic_cast<StatefulReader*>(rout);
        logInfo(RTPS_LIVELINESS, "Builtin Liveliness Reader created");
    }
    else
    {
        logError(RTPS_LIVELINESS, "Liveliness Reader Creation failed.");
        delete(mp_builtinReaderSecureHistory);
        mp_builtinReaderSecureHistory = nullptr;
        return false;
    }

    return true;
}

bool WLP::pairing_remote_reader_with_local_writer_after_security(const GUID_t& local_writer,
    const ReaderProxyData& remote_reader_data)
{
    if (local_writer.entityId == c_EntityId_WriterLivelinessSecure)
    {
        RemoteReaderAttributes attrs = remote_reader_data.toRemoteReaderAttributes();
        mp_builtinWriterSecure->matched_reader_add(attrs);
        return true;
    }

    return false;
}

bool WLP::pairing_remote_writer_with_local_reader_after_security(const GUID_t& local_reader,
    const WriterProxyData& remote_writer_data)
{
    if (local_reader.entityId == c_EntityId_ReaderLivelinessSecure)
    {
        RemoteWriterAttributes attrs = remote_writer_data.toRemoteWriterAttributes();
        mp_builtinReaderSecure->matched_writer_add(attrs);
        return true;
    }

    return false;
}

#endif

bool WLP::assignRemoteEndpoints(const ParticipantProxyData& pdata)
{
    uint32_t endp = pdata.m_availableBuiltinEndpoints;
    uint32_t partdet = endp;
    uint32_t auxendp = endp;
    partdet &= DISC_BUILTIN_ENDPOINT_PARTICIPANT_DETECTOR; //Habria que quitar esta linea que comprueba si tiene PDP.
    auxendp &= BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_WRITER;

    if ((auxendp!=0 || partdet!=0) && this->mp_builtinReader!=nullptr)
    {
        logInfo(RTPS_LIVELINESS,"Adding remote writer to my local Builtin Reader");
        RemoteWriterAttributes watt(pdata.m_VendorId);
        watt.guid.guidPrefix = pdata.m_guid.guidPrefix;
        watt.guid.entityId = c_EntityId_WriterLiveliness;
        watt.endpoint.persistence_guid = watt.guid;
        watt.endpoint.unicastLocatorList = pdata.m_metatrafficUnicastLocatorList;
        watt.endpoint.multicastLocatorList = pdata.m_metatrafficMulticastLocatorList;
        watt.endpoint.topicKind = WITH_KEY;
        watt.endpoint.durabilityKind = TRANSIENT_LOCAL;
        watt.endpoint.reliabilityKind = RELIABLE;
        mp_builtinReader->matched_writer_add(watt);
    }
    auxendp = endp;
    auxendp &=BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_READER;
    if ((auxendp!=0 || partdet!=0) && this->mp_builtinWriter!=nullptr)
    {
        logInfo(RTPS_LIVELINESS,"Adding remote reader to my local Builtin Writer");
        RemoteReaderAttributes ratt(pdata.m_VendorId);
        ratt.expectsInlineQos = false;
        ratt.guid.guidPrefix = pdata.m_guid.guidPrefix;
        ratt.guid.entityId = c_EntityId_ReaderLiveliness;
        ratt.endpoint.unicastLocatorList = pdata.m_metatrafficUnicastLocatorList;
        ratt.endpoint.multicastLocatorList = pdata.m_metatrafficMulticastLocatorList;
        ratt.endpoint.topicKind = WITH_KEY;
        ratt.endpoint.durabilityKind = TRANSIENT_LOCAL;
        ratt.endpoint.reliabilityKind = RELIABLE;
        mp_builtinWriter->matched_reader_add(ratt);
    }

#if HAVE_SECURITY
    auxendp = endp;
    auxendp &= BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_SECURE_DATA_WRITER;
    if ((auxendp != 0 || partdet != 0) && this->mp_builtinReaderSecure != nullptr)
    {
        logInfo(RTPS_LIVELINESS, "Adding remote writer to my local Builtin Secure Reader");
        WriterProxyData watt;
        watt.guid().guidPrefix = pdata.m_guid.guidPrefix;
        watt.guid().entityId = c_EntityId_WriterLivelinessSecure;
        watt.persistence_guid(watt.guid());
        watt.unicastLocatorList(pdata.m_metatrafficUnicastLocatorList);
        watt.multicastLocatorList(pdata.m_metatrafficMulticastLocatorList);
        watt.topicKind(WITH_KEY);
        watt.m_qos.m_durability.kind = TRANSIENT_LOCAL_DURABILITY_QOS;
        watt.m_qos.m_reliability.kind = RELIABLE_RELIABILITY_QOS;
        if (!mp_participant->security_manager().discovered_builtin_writer(
            mp_builtinReaderSecure->getGuid(), pdata.m_guid, watt,
            mp_builtinReaderSecure->getAttributes().security_attributes()))
        {
            logError(RTPS_EDP, "Security manager returns an error for reader " <<
                mp_builtinReaderSecure->getGuid());
        }
    }
    auxendp = endp;
    auxendp &= BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_SECURE_DATA_READER;
    if ((auxendp != 0 || partdet != 0) && this->mp_builtinWriterSecure != nullptr)
    {
        logInfo(RTPS_LIVELINESS, "Adding remote reader to my local Builtin Secure Writer");
        ReaderProxyData ratt;
        ratt.m_expectsInlineQos = false;
        ratt.guid().guidPrefix = pdata.m_guid.guidPrefix;
        ratt.guid().entityId = c_EntityId_ReaderLivelinessSecure;
        ratt.unicastLocatorList(pdata.m_metatrafficUnicastLocatorList);
        ratt.multicastLocatorList(pdata.m_metatrafficMulticastLocatorList);
        ratt.m_qos.m_durability.kind = TRANSIENT_LOCAL_DURABILITY_QOS;
        ratt.m_qos.m_reliability.kind = RELIABLE_RELIABILITY_QOS;
        ratt.topicKind(WITH_KEY);
        if (!mp_participant->security_manager().discovered_builtin_reader(
            mp_builtinWriterSecure->getGuid(), pdata.m_guid, ratt,
            mp_builtinWriterSecure->getAttributes().security_attributes()))
        {
            logError(RTPS_EDP, "Security manager returns an error for writer " <<
                mp_builtinWriterSecure->getGuid());
        }
    }
#endif

    return true;
}

void WLP::removeRemoteEndpoints(ParticipantProxyData* pdata)
{
    logInfo(RTPS_LIVELINESS,"for RTPSParticipant: "<<pdata->m_guid);
    uint32_t endp = pdata->m_availableBuiltinEndpoints;
    uint32_t partdet = endp;
    uint32_t auxendp = endp;
    partdet &= DISC_BUILTIN_ENDPOINT_PARTICIPANT_DETECTOR; //Habria que quitar esta linea que comprueba si tiene PDP.
    auxendp &= BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_WRITER;

    if ((auxendp!=0 || partdet!=0) && this->mp_builtinReader!=nullptr)
    {
        logInfo(RTPS_LIVELINESS,"Removing remote writer from my local Builtin Reader");
        RemoteWriterAttributes watt;
        watt.guid.guidPrefix = pdata->m_guid.guidPrefix;
        watt.guid.entityId = c_EntityId_WriterLiveliness;
        watt.endpoint.persistence_guid = watt.guid;
        watt.endpoint.unicastLocatorList = pdata->m_metatrafficUnicastLocatorList;
        watt.endpoint.multicastLocatorList = pdata->m_metatrafficMulticastLocatorList;
        watt.endpoint.topicKind = WITH_KEY;
        watt.endpoint.durabilityKind = TRANSIENT_LOCAL;
        watt.endpoint.reliabilityKind = RELIABLE;
        mp_builtinReader->matched_writer_remove(watt);
    }
    auxendp = endp;
    auxendp &=BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_READER;
    if ((auxendp!=0 || partdet!=0) && this->mp_builtinWriter!=nullptr)
    {
        logInfo(RTPS_LIVELINESS,"Removing remote reader from my local Builtin Writer");
        RemoteReaderAttributes ratt;
        ratt.expectsInlineQos = false;
        ratt.guid.guidPrefix = pdata->m_guid.guidPrefix;
        ratt.guid.entityId = c_EntityId_ReaderLiveliness;
        ratt.endpoint.unicastLocatorList = pdata->m_metatrafficUnicastLocatorList;
        ratt.endpoint.multicastLocatorList = pdata->m_metatrafficMulticastLocatorList;
        ratt.endpoint.topicKind = WITH_KEY;
        ratt.endpoint.durabilityKind = TRANSIENT_LOCAL;
        ratt.endpoint.reliabilityKind = RELIABLE;
        mp_builtinWriter->matched_reader_remove(ratt);
    }

#if HAVE_SECURITY
    auxendp = endp;
    auxendp &= BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_SECURE_DATA_WRITER;
    if ((auxendp != 0 || partdet != 0) && this->mp_builtinReaderSecure != nullptr)
    {
        logInfo(RTPS_LIVELINESS, "Removing remote writer from my local Builtin Secure Reader");
        RemoteWriterAttributes watt;
        watt.guid.guidPrefix = pdata->m_guid.guidPrefix;
        watt.guid.entityId = c_EntityId_WriterLivelinessSecure;
        watt.endpoint.persistence_guid = watt.guid;
        watt.endpoint.unicastLocatorList = pdata->m_metatrafficUnicastLocatorList;
        watt.endpoint.multicastLocatorList = pdata->m_metatrafficMulticastLocatorList;
        watt.endpoint.topicKind = WITH_KEY;
        watt.endpoint.durabilityKind = TRANSIENT_LOCAL;
        watt.endpoint.reliabilityKind = RELIABLE;
        watt.endpoint.security_attributes() = mp_builtinReaderSecure->getAttributes().security_attributes();
        if (mp_builtinReaderSecure->matched_writer_remove(watt))
        {
            mp_participant->security_manager().remove_writer(
                mp_builtinReaderSecure->getGuid(), pdata->m_guid, watt.guid);
        }
    }
    auxendp = endp;
    auxendp &= BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_SECURE_DATA_READER;
    if ((auxendp != 0 || partdet != 0) && this->mp_builtinWriterSecure != nullptr)
    {
        logInfo(RTPS_LIVELINESS, "Removing remote reader from my local Builtin Secure Writer");
        RemoteReaderAttributes ratt;
        ratt.expectsInlineQos = false;
        ratt.guid.guidPrefix = pdata->m_guid.guidPrefix;
        ratt.guid.entityId = c_EntityId_ReaderLivelinessSecure;
        ratt.endpoint.unicastLocatorList = pdata->m_metatrafficUnicastLocatorList;
        ratt.endpoint.multicastLocatorList = pdata->m_metatrafficMulticastLocatorList;
        ratt.endpoint.topicKind = WITH_KEY;
        ratt.endpoint.durabilityKind = TRANSIENT_LOCAL;
        ratt.endpoint.reliabilityKind = RELIABLE;
        ratt.endpoint.security_attributes() = mp_builtinWriterSecure->getAttributes().security_attributes();
        if (mp_builtinWriterSecure->matched_reader_remove(ratt))
        {
            mp_participant->security_manager().remove_reader(
                mp_builtinWriterSecure->getGuid(), pdata->m_guid, ratt.guid);
        }
    }
#endif
}

bool WLP::add_local_writer(RTPSWriter* W, const WriterQos& wqos)
{
    std::lock_guard<std::recursive_mutex> guard(*mp_builtinProtocols->mp_PDP->getMutex());
    logInfo(RTPS_LIVELINESS, W->getGuid().entityId << " to Liveliness Protocol");

    double wAnnouncementPeriodMilliSec(TimeConv::Duration_t2MilliSecondsDouble(wqos.m_liveliness.announcement_period));

    if (wqos.m_liveliness.kind == AUTOMATIC_LIVELINESS_QOS )
    {
        if (automatic_liveliness_assertion_ == nullptr)
        {
            automatic_liveliness_assertion_ = new WLivelinessPeriodicAssertion(this,AUTOMATIC_LIVELINESS_QOS);
            automatic_liveliness_assertion_->update_interval_millisec(wAnnouncementPeriodMilliSec);
            automatic_liveliness_assertion_->restart_timer();
            min_automatic_ms_ = wAnnouncementPeriodMilliSec;
        }
        else if (min_automatic_ms_ > wAnnouncementPeriodMilliSec)
        {
            min_automatic_ms_ = wAnnouncementPeriodMilliSec;
            automatic_liveliness_assertion_->update_interval_millisec(wAnnouncementPeriodMilliSec);
            //CHECK IF THE TIMER IS GOING TO BE CALLED AFTER THIS NEW SET LEASE DURATION
            if (automatic_liveliness_assertion_->getRemainingTimeMilliSec() > min_automatic_ms_)
            {
                automatic_liveliness_assertion_->cancel_timer();
            }
            automatic_liveliness_assertion_->restart_timer();
        }
        automatic_writers_.push_back(W);
    }
    else if (wqos.m_liveliness.kind == MANUAL_BY_PARTICIPANT_LIVELINESS_QOS)
    {
        if (manual_liveliness_assertion_ == nullptr)
        {
            manual_liveliness_assertion_ = new WLivelinessPeriodicAssertion(this,MANUAL_BY_PARTICIPANT_LIVELINESS_QOS);
            manual_liveliness_assertion_->update_interval_millisec(wAnnouncementPeriodMilliSec);
            manual_liveliness_assertion_->restart_timer();
            min_manual_by_participant_ms_ = wAnnouncementPeriodMilliSec;
        }
        else if (min_manual_by_participant_ms_ > wAnnouncementPeriodMilliSec)
        {
            min_manual_by_participant_ms_ = wAnnouncementPeriodMilliSec;
            manual_liveliness_assertion_->update_interval_millisec(min_manual_by_participant_ms_);
            //CHECK IF THE TIMER IS GOING TO BE CALLED AFTER THIS NEW SET LEASE DURATION
            if (manual_liveliness_assertion_->getRemainingTimeMilliSec() > min_manual_by_participant_ms_)
            {
                manual_liveliness_assertion_->cancel_timer();
            }
            manual_liveliness_assertion_->restart_timer();
        }
        manual_by_participant_writers_.push_back(W);

        if (!pub_liveliness_manager_->add_writer(
                    W->getGuid(),
                    wqos.m_liveliness.kind,
                    wqos.m_liveliness.lease_duration))
        {
            logError(RTPS_LIVELINESS, "Could not add writer " << W->getGuid() << " to liveliness manager");
        }
    }
    else if (wqos.m_liveliness.kind == MANUAL_BY_TOPIC_LIVELINESS_QOS)
    {
        manual_by_topic_writers_.push_back(W);

        if (!pub_liveliness_manager_->add_writer(
                    W->getGuid(),
                    wqos.m_liveliness.kind,
                    wqos.m_liveliness.lease_duration))
        {
            logError(RTPS_LIVELINESS, "Could not add writer " << W->getGuid() << " to liveliness manager");
        }
    }

    return true;
}

typedef std::vector<RTPSWriter*>::iterator t_WIT;

bool WLP::remove_local_writer(RTPSWriter* W)
{
    std::lock_guard<std::recursive_mutex> guard(*mp_builtinProtocols->mp_PDP->getMutex());

    logInfo(RTPS_LIVELINESS, W->getGuid().entityId <<" from Liveliness Protocol");

    t_WIT wToEraseIt;
    ParticipantProxyData pdata;
    WriterProxyData wdata;
    if (this->mp_builtinProtocols->mp_PDP->lookupWriterProxyData(W->getGuid(), wdata, pdata))
    {
        bool found = false;
        if (wdata.m_qos.m_liveliness.kind == AUTOMATIC_LIVELINESS_QOS)
        {
            min_automatic_ms_ = std::numeric_limits<double>::max();
            for (t_WIT it = automatic_writers_.begin(); it != automatic_writers_.end(); ++it)
            {
                ParticipantProxyData pdata2;
                WriterProxyData wdata2;
                if (this->mp_builtinProtocols->mp_PDP->lookupWriterProxyData((*it)->getGuid(), wdata2, pdata2))
                {
                    double mintimeWIT(TimeConv::Duration_t2MilliSecondsDouble(
                        wdata2.m_qos.m_liveliness.announcement_period));

                    if (W->getGuid().entityId == (*it)->getGuid().entityId)
                    {
                        found = true;
                        wToEraseIt = it;
                        continue;
                    }
                    if (min_automatic_ms_ > mintimeWIT)
                    {
                        min_automatic_ms_ = mintimeWIT;
                    }
                }
            }
            if (found)
            {
                automatic_writers_.erase(wToEraseIt);
                if (automatic_liveliness_assertion_ != nullptr)
                {
                    if (automatic_writers_.size() > 0)
                    {
                        automatic_liveliness_assertion_->update_interval_millisec(min_automatic_ms_);
                    }
                    else
                    {
                        automatic_liveliness_assertion_->cancel_timer();
                    }
                }
            }
        }
        else if (wdata.m_qos.m_liveliness.kind == MANUAL_BY_PARTICIPANT_LIVELINESS_QOS)
        {
            min_manual_by_participant_ms_ = std::numeric_limits<double>::max();
            for(t_WIT it = manual_by_participant_writers_.begin(); it != manual_by_participant_writers_.end(); ++it)
            {
                ParticipantProxyData pdata2;
                WriterProxyData wdata2;
                if (this->mp_builtinProtocols->mp_PDP->lookupWriterProxyData((*it)->getGuid(), wdata2, pdata2))
                {
                    double mintimeWIT(TimeConv::Duration_t2MilliSecondsDouble(
                        wdata2.m_qos.m_liveliness.announcement_period));
                    if (W->getGuid().entityId == (*it)->getGuid().entityId)
                    {
                        found = true;
                        wToEraseIt = it;
                        continue;
                    }
                    if (min_manual_by_participant_ms_ > mintimeWIT)
                    {
                        min_manual_by_participant_ms_ = mintimeWIT;
                    }
                }
            }
            if (found)
            {
                manual_by_participant_writers_.erase(wToEraseIt);
                if (manual_liveliness_assertion_ != nullptr)
                {
                    if (manual_by_participant_writers_.size() > 0)
                    {
                        manual_liveliness_assertion_->update_interval_millisec(min_manual_by_participant_ms_);
                    }
                    else
                    {
                        manual_liveliness_assertion_->cancel_timer();
                    }
                }
            }

            if (!pub_liveliness_manager_->remove_writer(
                        W->getGuid(),
                        wdata.m_qos.m_liveliness.kind,
                        wdata.m_qos.m_liveliness.lease_duration))
            {
                logError(RTPS_LIVELINESS, "Could not remove writer " << W->getGuid() << " from liveliness manager");
            }
        }
        else if (wdata.m_qos.m_liveliness.kind == MANUAL_BY_TOPIC_LIVELINESS_QOS)
        {
            for (auto it=manual_by_topic_writers_.begin(); it!=manual_by_topic_writers_.end(); ++it)
            {
                if (W->getGuid().entityId == (*it)->getGuid().entityId)
                {
                    found = true;
                    wToEraseIt = it;
                }
            }
            if (found)
            {
                manual_by_topic_writers_.erase(wToEraseIt);
            }

            if (!pub_liveliness_manager_->remove_writer(
                        W->getGuid(),
                        wdata.m_qos.m_liveliness.kind,
                        wdata.m_qos.m_liveliness.lease_duration))
            {
                logError(RTPS_LIVELINESS, "Could not remove writer " << W->getGuid() << " from liveliness manager");
            }
        }

        if (found)
        {
            return true;
        }
        else
        {
            return false;
        }
    }
    logWarning(RTPS_LIVELINESS,"Writer "<<W->getGuid().entityId << " not found.");
    return false;
}

bool WLP::add_local_reader(RTPSReader* reader, const ReaderQos &rqos)
{
    std::lock_guard<std::recursive_mutex> guard(*mp_builtinProtocols->mp_PDP->getMutex());

    if (rqos.m_liveliness.kind == AUTOMATIC_LIVELINESS_QOS)
    {
        automatic_readers_ = true;
    }

    readers_.push_back(reader);

    return true;
}

bool WLP::remove_local_reader(RTPSReader* reader)
{
    auto it = std::find(
                readers_.begin(),
                readers_.end(),
                reader);
    if (it != readers_.end())
    {
        readers_.erase(it);
        return true;
    }

    logWarning(RTPS_LIVELINESS, "Reader not removed from WLP, unknown reader");
    return false;
}

StatefulWriter* WLP::getBuiltinWriter()
{
    StatefulWriter* ret_val = mp_builtinWriter;

#if HAVE_SECURITY
    if (mp_participant->security_attributes().is_liveliness_protected)
    {
        ret_val = mp_builtinWriterSecure;
    }
#endif

    return ret_val;
}

WriterHistory* WLP::getBuiltinWriterHistory()
{
    WriterHistory* ret_val = mp_builtinWriterHistory;

#if HAVE_SECURITY
    if (mp_participant->security_attributes().is_liveliness_protected)
    {
        ret_val = mp_builtinWriterSecureHistory;
    }
#endif

    return ret_val;
}

bool WLP::assert_liveliness(
        GUID_t writer,
        LivelinessQosPolicyKind kind,
        Duration_t lease_duration)
{
    return pub_liveliness_manager_->assert_liveliness(
                writer,
                kind,
                lease_duration);
}

bool WLP::assert_liveliness_manual_by_participant()
{
    if (manual_by_participant_writers_.size() > 0)
    {
        return pub_liveliness_manager_->assert_liveliness(MANUAL_BY_PARTICIPANT_LIVELINESS_QOS);
    }
    return false;
}

void WLP::pub_liveliness_changed(
        const GUID_t& writer,
        const LivelinessQosPolicyKind& kind,
        const Duration_t& lease_duration,
        int32_t alive_change,
        int32_t not_alive_change)
{
    (void)lease_duration;
    (void)alive_change;

    // On the publishing side we only have to notify if one of our writers loses liveliness
    if (not_alive_change != 1)
    {
        return;
    }

    if (kind == AUTOMATIC_LIVELINESS_QOS)
    {
        for (RTPSWriter* w: automatic_writers_)
        {
            if (w->getGuid() == writer)
            {
                std::unique_lock<std::recursive_timed_mutex> lock(w->getMutex());

                w->liveliness_lost_status_.total_count++;
                w->liveliness_lost_status_.total_count_change++;
                if (w->getListener() != nullptr)
                {
                    w->getListener()->on_liveliness_lost(w, w->liveliness_lost_status_);
                }
                w->liveliness_lost_status_.total_count_change = 0u;

                return;
            }
        }
    }
    else if (kind == MANUAL_BY_PARTICIPANT_LIVELINESS_QOS)
    {
        for (RTPSWriter* w: manual_by_participant_writers_)
        {
            if (w->getGuid() == writer)
            {
                std::unique_lock<std::recursive_timed_mutex> lock(w->getMutex());

                w->liveliness_lost_status_.total_count++;
                w->liveliness_lost_status_.total_count_change++;
                if (w->getListener() != nullptr)
                {
                    w->getListener()->on_liveliness_lost(w, w->liveliness_lost_status_);
                }
                w->liveliness_lost_status_.total_count_change = 0u;

                return;
            }
        }
    }
    else if (kind == MANUAL_BY_TOPIC_LIVELINESS_QOS)
    {
        for (RTPSWriter* w: manual_by_topic_writers_)
        {
            if (w->getGuid() == writer)
            {
                std::unique_lock<std::recursive_timed_mutex> lock(w->getMutex());

                w->liveliness_lost_status_.total_count++;
                w->liveliness_lost_status_.total_count_change++;
                if (w->getListener() != nullptr)
                {
                    w->getListener()->on_liveliness_lost(w, w->liveliness_lost_status_);
                }
                w->liveliness_lost_status_.total_count_change = 0u;

                return;
            }
        }
    }
}

void WLP::sub_liveliness_changed(
        const GUID_t& writer,
        const LivelinessQosPolicyKind& kind,
        const Duration_t& lease_duration,
        int32_t alive_change,
        int32_t not_alive_change)
{
    // Writer with given guid lost liveliness, check which readers were matched and inform them

    RemoteWriterAttributes ratt;
    ratt.guid = writer;

    for (RTPSReader* reader : readers_)
    {
        if (reader->liveliness_kind_ == kind &&
                reader->liveliness_lease_duration_ == lease_duration)
        {
            if (reader->matched_writer_is_matched(ratt))
            {
                update_liveliness_changed_status(
                            writer,
                            reader,
                            alive_change,
                            not_alive_change);
            }
        }
    }
}

void WLP::update_liveliness_changed_status(
        GUID_t writer,
        RTPSReader* reader,
        int32_t alive_change,
        int32_t not_alive_change)
{
    reader->liveliness_changed_status_.alive_count += alive_change;
    reader->liveliness_changed_status_.alive_count_change += alive_change;
    reader->liveliness_changed_status_.not_alive_count += not_alive_change;
    reader->liveliness_changed_status_.not_alive_count_change += not_alive_change;
    reader->liveliness_changed_status_.last_publication_handle = writer;

    if (reader->getListener() != nullptr)
    {
        reader->getListener()->on_liveliness_changed(reader, reader->liveliness_changed_status_);

        reader->liveliness_changed_status_.alive_count_change = 0;
        reader->liveliness_changed_status_.not_alive_count_change = 0;
    }
}

} /* namespace rtps */
} /* namespace fastrtps */
} /* namespace eprosima */
