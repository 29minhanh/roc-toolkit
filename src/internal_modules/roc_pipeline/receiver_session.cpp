/*
 * Copyright (c) 2017 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_pipeline/receiver_session.h"
#include "roc_audio/resampler_map.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_fec/codec_map.h"

namespace roc {
namespace pipeline {

ReceiverSession::ReceiverSession(
    const ReceiverSessionConfig& session_config,
    const ReceiverCommonConfig& common_config,
    const address::SocketAddr& src_address,
    const rtp::FormatMap& format_map,
    packet::PacketFactory& packet_factory,
    core::BufferFactory<uint8_t>& byte_buffer_factory,
    core::BufferFactory<audio::sample_t>& sample_buffer_factory,
    core::IAllocator& allocator)
    : RefCounted(allocator)
    , src_address_(src_address)
    , audio_reader_(NULL) {
    const rtp::Format* format = format_map.format(session_config.payload_type);
    if (!format) {
        return;
    }

    queue_router_.reset(new (queue_router_) packet::Router(allocator));
    if (!queue_router_) {
        return;
    }

    source_queue_.reset(new (source_queue_) packet::SortedQueue(0));
    if (!source_queue_) {
        return;
    }

    packet::IWriter* pwriter = source_queue_.get();

    if (!queue_router_->add_route(*pwriter, packet::Packet::FlagAudio)) {
        return;
    }

    packet::IReader* preader = source_queue_.get();

    payload_decoder_.reset(format->new_decoder(allocator), allocator);
    if (!payload_decoder_) {
        return;
    }

    validator_.reset(new (validator_) rtp::Validator(
        *preader, session_config.rtp_validator, format->sample_spec));
    if (!validator_) {
        return;
    }
    preader = validator_.get();

    populator_.reset(new (populator_) rtp::Populator(*preader, *payload_decoder_,
                                                     format->sample_spec));
    if (!populator_) {
        return;
    }
    preader = populator_.get();

    delayed_reader_.reset(new (delayed_reader_) packet::DelayedReader(
        *preader, session_config.target_latency, format->sample_spec));
    if (!delayed_reader_) {
        return;
    }
    preader = delayed_reader_.get();

    if (session_config.fec_decoder.scheme != packet::FEC_None) {
        repair_queue_.reset(new (repair_queue_) packet::SortedQueue(0));
        if (!repair_queue_) {
            return;
        }
        if (!queue_router_->add_route(*repair_queue_, packet::Packet::FlagRepair)) {
            return;
        }

        fec_decoder_.reset(
            fec::CodecMap::instance().new_decoder(session_config.fec_decoder,
                                                  byte_buffer_factory, allocator),
            allocator);
        if (!fec_decoder_) {
            return;
        }

        fec_parser_.reset(new (fec_parser_) rtp::Parser(format_map, NULL));
        if (!fec_parser_) {
            return;
        }

        fec_reader_.reset(new (fec_reader_) fec::Reader(
            session_config.fec_reader, session_config.fec_decoder.scheme, *fec_decoder_,
            *preader, *repair_queue_, *fec_parser_, packet_factory, allocator));
        if (!fec_reader_ || !fec_reader_->valid()) {
            return;
        }
        preader = fec_reader_.get();

        fec_validator_.reset(new (fec_validator_) rtp::Validator(
            *preader, session_config.rtp_validator, format->sample_spec));
        if (!fec_validator_) {
            return;
        }
        preader = fec_validator_.get();
    }

    depacketizer_.reset(new (depacketizer_) audio::Depacketizer(
        *preader, *payload_decoder_, format->sample_spec, common_config.beeping));
    if (!depacketizer_) {
        return;
    }

    audio::IFrameReader* areader = depacketizer_.get();

    if (session_config.watchdog.no_playback_timeout != 0
        || session_config.watchdog.broken_playback_timeout != 0
        || session_config.watchdog.frame_status_window != 0) {
        watchdog_.reset(new (watchdog_) audio::Watchdog(
            *areader, format->sample_spec, session_config.watchdog, allocator));
        if (!watchdog_ || !watchdog_->valid()) {
            return;
        }
        areader = watchdog_.get();
    }

    if (format->sample_spec.channel_mask()
        != common_config.output_sample_spec.channel_mask()) {
        channel_mapper_reader_.reset(
            new (channel_mapper_reader_) audio::ChannelMapperReader(
                *areader, sample_buffer_factory, common_config.internal_frame_length,
                format->sample_spec,
                audio::SampleSpec(format->sample_spec.sample_rate(),
                                  common_config.output_sample_spec.channel_mask())));
        if (!channel_mapper_reader_ || !channel_mapper_reader_->valid()) {
            return;
        }
        areader = channel_mapper_reader_.get();
    }

    if (common_config.resampling) {
        if (common_config.poisoning) {
            resampler_poisoner_.reset(new (resampler_poisoner_)
                                          audio::PoisonReader(*areader));
            if (!resampler_poisoner_) {
                return;
            }
            areader = resampler_poisoner_.get();
        }

        resampler_.reset(
            audio::ResamplerMap::instance().new_resampler(
                session_config.resampler_backend, allocator, sample_buffer_factory,
                session_config.resampler_profile, common_config.internal_frame_length,
                audio::SampleSpec(format->sample_spec.sample_rate(),
                                  common_config.output_sample_spec.channel_mask())),
            allocator);

        if (!resampler_) {
            return;
        }

        resampler_reader_.reset(new (resampler_reader_) audio::ResamplerReader(
            *areader, *resampler_,
            audio::SampleSpec(format->sample_spec.sample_rate(),
                              common_config.output_sample_spec.channel_mask()),
            common_config.output_sample_spec));

        if (!resampler_reader_ || !resampler_reader_->valid()) {
            return;
        }
        areader = resampler_reader_.get();
    }

    if (common_config.poisoning) {
        session_poisoner_.reset(new (session_poisoner_) audio::PoisonReader(*areader));
        if (!session_poisoner_) {
            return;
        }
        areader = session_poisoner_.get();
    }

    latency_monitor_.reset(new (latency_monitor_) audio::LatencyMonitor(
        *source_queue_, *depacketizer_, resampler_reader_.get(),
        session_config.latency_monitor, session_config.target_latency,
        format->sample_spec, common_config.output_sample_spec,
        session_config.freq_estimator_config));
    if (!latency_monitor_ || !latency_monitor_->valid()) {
        return;
    }

    audio_reader_ = areader;
}

bool ReceiverSession::valid() const {
    return audio_reader_;
}

bool ReceiverSession::handle(const packet::PacketPtr& packet) {
    roc_panic_if(!valid());

    packet::UDP* udp = packet->udp();
    if (!udp) {
        return false;
    }

    if (udp->src_addr != src_address_) {
        return false;
    }

    queue_router_->write(packet);
    return true;
}

bool ReceiverSession::advance(packet::timestamp_t timestamp) {
    roc_panic_if(!valid());

    if (watchdog_) {
        if (!watchdog_->update()) {
            return false;
        }
    }

    if (latency_monitor_) {
        if (!latency_monitor_->update(timestamp)) {
            return false;
        }
    }

    return true;
}

bool ReceiverSession::reclock(packet::ntp_timestamp_t) {
    roc_panic_if(!valid());

    // no-op
    return true;
}

audio::IFrameReader& ReceiverSession::reader() {
    roc_panic_if(!valid());

    return *audio_reader_;
}

void ReceiverSession::add_sending_metrics(const rtcp::SendingMetrics& metrics) {
    // TODO
    (void)metrics;
}

void ReceiverSession::add_link_metrics(const rtcp::LinkMetrics& metrics) {
    // TODO
    (void)metrics;
}

} // namespace pipeline
} // namespace roc
