/*
 * Copyright 2014 Intel Corporation All Rights Reserved. 
 * 
 * The source code contained or described herein and all documents related to the 
 * source code ("Material") are owned by Intel Corporation or its suppliers or 
 * licensors. Title to the Material remains with Intel Corporation or its suppliers 
 * and licensors. The Material contains trade secrets and proprietary and 
 * confidential information of Intel or its suppliers and licensors. The Material 
 * is protected by worldwide copyright and trade secret laws and treaty provisions. 
 * No part of the Material may be used, copied, reproduced, modified, published, 
 * uploaded, posted, transmitted, distributed, or disclosed in any way without 
 * Intel's prior express written permission.
 * 
 * No license under any patent, copyright, trade secret or other intellectual 
 * property right is granted to or conferred upon you by disclosure or delivery of 
 * the Materials, either expressly, by implication, inducement, estoppel or 
 * otherwise. Any license under such intellectual property rights must be express 
 * and approved by Intel in writing.
 */

#include "AudioMixer.h"

#include <rtputils.h>
#include <webrtc/common_types.h>
#include <webrtc/modules/audio_device/include/audio_device_defines.h>
#include <webrtc/modules/interface/module_common_types.h>
#include <webrtc/voice_engine/include/voe_codec.h>
#include <webrtc/voice_engine/include/voe_external_media.h>
#include <webrtc/voice_engine/include/voe_network.h>
#include <webrtc/voice_engine/include/voe_rtp_rtcp.h>

using namespace erizo;
using namespace webrtc;

namespace mcu {

DEFINE_LOGGER(AudioMixer, "mcu.media.AudioMixer");

AudioMixer::AudioMixer(erizo::RTPDataReceiver* receiver)
    : m_dataReceiver(receiver)
    , m_addSourceOnDemand(false)
{
    m_voiceEngine = VoiceEngine::Create();

    VoEBase* voe = VoEBase::GetInterface(m_voiceEngine);
    voe->Init();
    m_sharedChannel.id = voe->CreateChannel();    // id is always 0

    VoEExternalMedia* externalMedia = VoEExternalMedia::GetInterface(m_voiceEngine);
    externalMedia->SetExternalRecordingStatus(true);
    externalMedia->SetExternalPlayoutStatus(true);

    m_sharedChannel.transport.reset(new woogeen_base::WoogeenTransport<erizo::AUDIO>(receiver, nullptr));
    VoENetwork* network = VoENetwork::GetInterface(m_voiceEngine);
    network->RegisterExternalTransport(m_sharedChannel.id, *(m_sharedChannel.transport));

    // FIXME: hard coded timer interval.
    m_jobTimer.reset(new JobTimer(100, this));
}

AudioMixer::~AudioMixer()
{
    VoEBase* voe = VoEBase::GetInterface(m_voiceEngine);
    VoENetwork* network = VoENetwork::GetInterface(m_voiceEngine);

    voe->StopSend(m_sharedChannel.id);
    network->DeRegisterExternalTransport(m_sharedChannel.id);
    voe->DeleteChannel(m_sharedChannel.id);

    boost::unique_lock<boost::shared_mutex> lock(m_sourceMutex);
    std::map<uint32_t, VoiceChannel>::iterator it = m_sourceChannels.begin();
    for (; it != m_sourceChannels.end(); ++it) {
        int channel = it->second.id;
        voe->StopPlayout(channel);
        voe->StopReceive(channel);
        network->DeRegisterExternalTransport(channel);
        voe->DeleteChannel(channel);
    }
    m_sourceChannels.clear();
    lock.unlock();

    boost::unique_lock<boost::shared_mutex> outputLock(m_outputMutex);
    std::map<std::string, VoiceChannel>::iterator outputIt = m_outputChannels.begin();
    for (; outputIt != m_outputChannels.end(); ++outputIt) {
        int channel = outputIt->second.id;
        voe->StopSend(channel);
        network->DeRegisterExternalTransport(channel);
        voe->DeleteChannel(channel);
    }
    m_outputChannels.clear();
    outputLock.unlock();

    voe->Terminate();
    VoiceEngine::Delete(m_voiceEngine);
}

int32_t AudioMixer::addSource(uint32_t from, bool isAudio, erizo::FeedbackSink* feedback, const std::string& participantId)
{
    assert(isAudio);

    boost::upgrade_lock<boost::shared_mutex> lock(m_sourceMutex);
    std::map<uint32_t, VoiceChannel>::iterator it = m_sourceChannels.find(from);
    if (it != m_sourceChannels.end())
        return it->second.id;

    int channel = -1;
    boost::shared_ptr<woogeen_base::WoogeenTransport<erizo::AUDIO>> transport;
    VoEBase* voe = VoEBase::GetInterface(m_voiceEngine);
    bool existingParticipant = false;

    boost::upgrade_lock<boost::shared_mutex> outputLock(m_outputMutex);
    std::map<std::string, VoiceChannel>::iterator outputIt = m_outputChannels.find(participantId);
    if (outputIt != m_outputChannels.end()) {
        channel = outputIt->second.id;
        transport = outputIt->second.transport;
        existingParticipant = true;
        outputLock.unlock();
    } else
        channel = voe->CreateChannel();

    if (channel != -1) {
        if (existingParticipant) {
            assert(transport);
            // Set the Feedback sink for the transport, because this channel is going to be a source channel.
            transport->setFeedbackSink(feedback);
            if (voe->StartReceive(channel) == -1 || voe->StartPlayout(channel) == -1)
                return -1;
        } else {
            VoENetwork* network = VoENetwork::GetInterface(m_voiceEngine);
            transport.reset(new woogeen_base::WoogeenTransport<erizo::AUDIO>(m_dataReceiver, feedback));
            if (network->RegisterExternalTransport(channel, *(transport.get())) == -1
                || voe->StartReceive(channel) == -1
                || voe->StartPlayout(channel) == -1) {
                voe->DeleteChannel(channel);
                return -1;
            }
            boost::upgrade_to_unique_lock<boost::shared_mutex> uniquePartLock(outputLock);
            m_outputChannels[participantId] = {channel, transport};
        }

        // TODO: Another option is that we can implement
        // an External mixer. We may need to investigate whether it's
        // better than the current approach.
        // VoEExternalMedia* externalMedia = VoEExternalMedia::GetInterface(m_voiceEngine);
        // externalMedia->SetExternalMixing(channel, true);

        boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
        if (m_sourceChannels.size() == 0)
            voe->StartSend(m_sharedChannel.id);

        m_sourceChannels[from] = {channel, transport};
    }
    return channel;
}

int32_t AudioMixer::removeSource(uint32_t from, bool isAudio)
{
    assert(isAudio);

    VoEBase* voe = VoEBase::GetInterface(m_voiceEngine);
    VoENetwork* network = VoENetwork::GetInterface(m_voiceEngine);

    boost::unique_lock<boost::shared_mutex> lock(m_sourceMutex);
    std::map<uint32_t, VoiceChannel>::iterator it = m_sourceChannels.find(from);
    if (it != m_sourceChannels.end()) {
        int channel = it->second.id;

        voe->StopPlayout(channel);
        voe->StopReceive(channel);

        bool outputExisted = false;
        boost::shared_lock<boost::shared_mutex> outputLock(m_outputMutex);
        std::map<std::string, VoiceChannel>::iterator outputIt = m_outputChannels.begin();
        for (; outputIt != m_outputChannels.end(); ++outputIt) {
            if (outputIt->second.id == channel) {
                outputIt->second.transport->setFeedbackSink(nullptr);
                outputExisted = true;
                break;
            }
        }
        outputLock.unlock();

        if (!outputExisted) {
            network->DeRegisterExternalTransport(channel);
            voe->DeleteChannel(channel);
        }

        m_sourceChannels.erase(it);

        if (m_sourceChannels.size() == 0)
            voe->StopSend(m_sharedChannel.id);

        return 0;
    }

    return -1;
}

#define global_ns

int AudioMixer::deliverAudioData(char* buf, int len)
{
    uint32_t id = 0;
    RTCPHeader* chead = reinterpret_cast<RTCPHeader*>(buf);
    uint8_t packetType = chead->getPacketType();
    assert(packetType != RTCP_Receiver_PT && packetType != RTCP_PS_Feedback_PT && packetType != RTCP_RTP_Feedback_PT);
    if (packetType == RTCP_Sender_PT)
        id = chead->getSSRC();
    else {
        global_ns::RTPHeader* head = reinterpret_cast<global_ns::RTPHeader*>(buf);
        id = head->getSSRC();
    }

    boost::shared_lock<boost::shared_mutex> lock(m_sourceMutex);
    std::map<uint32_t, VoiceChannel>::iterator it = m_sourceChannels.find(id);
    if (it == m_sourceChannels.end()) {
        if (m_addSourceOnDemand) {
            lock.unlock();
            addSource(id, true, nullptr, "");
        }
        return 0;
    }

    int channel = it->second.id;
    VoENetwork* network = VoENetwork::GetInterface(m_voiceEngine);

    if (packetType == RTCP_Sender_PT)
        return network->ReceivedRTCPPacket(channel, buf, len) == -1 ? 0 : len;

    return network->ReceivedRTPPacket(channel, buf, len) == -1 ? 0 : len;
}

int AudioMixer::deliverVideoData(char* buf, int len)
{
    assert(false);
    return 0;
}

int AudioMixer::deliverFeedback(char* buf, int len)
{
    VoENetwork* network = VoENetwork::GetInterface(m_voiceEngine);

    // TODO: For now we just send the feedback to all of the output channels.
    // The output channel will filter out the feedback which does not belong
    // to it. In the future we may do the filtering at a higher level?
    boost::shared_lock<boost::shared_mutex> lock(m_outputMutex);
    std::map<std::string, VoiceChannel>::iterator it = m_outputChannels.begin();
    for (; it != m_outputChannels.end(); ++it)
        network->ReceivedRTCPPacket(it->second.id, buf, len);

    return len;
}

void AudioMixer::onTimeout()
{
    performMix();
}

int32_t AudioMixer::addOutput(const std::string& participant)
{
    int channel = -1;
    bool existingParticipant = false;
    VoEBase* voe = VoEBase::GetInterface(m_voiceEngine);

    boost::upgrade_lock<boost::shared_mutex> lock(m_outputMutex);
    std::map<std::string, VoiceChannel>::iterator it = m_outputChannels.find(participant);
    if (it != m_outputChannels.end()) {
        channel = it->second.id;
        existingParticipant = true;
        lock.unlock();
    } else
        channel = voe->CreateChannel();

    if (channel != -1) {
        if (!existingParticipant) {
            boost::shared_ptr<woogeen_base::WoogeenTransport<erizo::AUDIO>> transport(new woogeen_base::WoogeenTransport<erizo::AUDIO>(m_dataReceiver, nullptr));
            VoENetwork* network = VoENetwork::GetInterface(m_voiceEngine);
            if (network->RegisterExternalTransport(channel, *(transport.get())) == -1
                || voe->StartSend(channel) == -1) {
                voe->DeleteChannel(channel);
                return -1;
            }
            boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
            m_outputChannels[participant] = {channel, transport};
        } else if (voe->StartSend(channel) == -1)
            return -1;
    }

    return channel;
}

int32_t AudioMixer::removeOutput(const std::string& participant)
{
    VoEBase* voe = VoEBase::GetInterface(m_voiceEngine);
    VoENetwork* network = VoENetwork::GetInterface(m_voiceEngine);

    boost::unique_lock<boost::shared_mutex> lock(m_outputMutex);
    std::map<std::string, VoiceChannel>::iterator it = m_outputChannels.find(participant);
    if (it != m_outputChannels.end()) {
        int channel = it->second.id;

        voe->StopSend(channel);

        bool sourceExisted = false;
        boost::shared_lock<boost::shared_mutex> sourceLock(m_sourceMutex);
        std::map<uint32_t, VoiceChannel>::iterator sourceIt = m_sourceChannels.begin();
        for (; sourceIt != m_sourceChannels.end(); ++sourceIt) {
            if (sourceIt->second.id == channel) {
                sourceExisted = true;
                break;
            }
        }
        sourceLock.unlock();

        if (!sourceExisted) {
            network->DeRegisterExternalTransport(channel);
            voe->DeleteChannel(channel);
        }

        m_outputChannels.erase(it);
        return 0;
    }

    return -1;
}

int32_t AudioMixer::getChannelId(uint32_t sourceId)
{
    boost::shared_lock<boost::shared_mutex> lock(m_sourceMutex);
    std::map<uint32_t, VoiceChannel>::iterator it = m_sourceChannels.find(sourceId);
    if (it != m_sourceChannels.end())
        return it->second.id;

    return -1;
}

uint32_t AudioMixer::getSendSSRC(int32_t channelId)
{
    VoERTP_RTCP* rtpRtcp = VoERTP_RTCP::GetInterface(m_voiceEngine);
    uint32_t ssrc = 0;
    rtpRtcp->GetLocalSSRC(channelId, ssrc);
    return ssrc;
}

int32_t AudioMixer::performMix()
{
    VoECodec* codec = VoECodec::GetInterface(m_voiceEngine);
    CodecInst audioCodec;
    VoEBase* voe = VoEBase::GetInterface(m_voiceEngine);
    AudioTransport* audioTransport = voe->audio_transport();
    int16_t data[AudioFrame::kMaxDataSizeSamples];
    uint32_t nSamplesOut = 0;
    boost::shared_lock<boost::shared_mutex> lock(m_sourceMutex);
    if (codec->GetSendCodec(m_sharedChannel.id, audioCodec) != -1) {
        if (audioTransport->NeedMorePlayData(
            audioCodec.plfreq / 1000 * 10, // samples per channel in a 10 ms period. FIXME: hard coded timer interval.
            0,
            audioCodec.channels,
            audioCodec.plfreq,
            data,
            nSamplesOut,
            -1) == 0)    // ugly to use -1 to represents the shared channel id
            audioTransport->OnData(m_sharedChannel.id, data, 0, audioCodec.plfreq, audioCodec.channels, nSamplesOut);
    }
    for (std::map<std::string, VoiceChannel>::iterator it = m_outputChannels.begin();
        it != m_outputChannels.end();
        ++it) {
        if (codec->GetSendCodec(it->second.id, audioCodec) != -1) {
            if (audioTransport->NeedMorePlayData(
                audioCodec.plfreq / 1000 * 10, // samples per channel in a 10 ms period. FIXME: hard coded timer interval.
                0,
                audioCodec.channels,
                audioCodec.plfreq,
                data,
                nSamplesOut,
                it->second.id) == 0)
                audioTransport->OnData(it->second.id, data, 0, audioCodec.plfreq, audioCodec.channels, nSamplesOut);
        }
    }

    return 0;
}

} /* namespace mcu */