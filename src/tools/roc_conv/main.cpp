/*
 * Copyright (c) 2019 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_address/io_uri.h"
#include "roc_audio/resampler_profile.h"
#include "roc_core/crash_handler.h"
#include "roc_core/heap_allocator.h"
#include "roc_core/log.h"
#include "roc_core/parse_duration.h"
#include "roc_core/scoped_ptr.h"
#include "roc_pipeline/converter_sink.h"
#include "roc_sndio/backend_dispatcher.h"
#include "roc_sndio/backend_map.h"
#include "roc_sndio/print_supported.h"
#include "roc_sndio/pump.h"

#include "roc_conv/cmdline.h"

using namespace roc;

int main(int argc, char** argv) {
    core::HeapAllocator::enable_panic_on_leak();

    core::CrashHandler crash_handler;

    gengetopt_args_info args;

    const int code = cmdline_parser(argc, argv, &args);
    if (code != 0) {
        return code;
    }

    core::ScopedPtr<gengetopt_args_info, core::CustomAllocation> args_holder(
        &args, &cmdline_parser_free);

    core::Logger::instance().set_verbosity(args.verbose_given);

    switch (args.color_arg) {
    case color_arg_auto:
        core::Logger::instance().set_colors(core::ColorsAuto);
        break;

    case color_arg_always:
        core::Logger::instance().set_colors(core::ColorsEnabled);
        break;

    case color_arg_never:
        core::Logger::instance().set_colors(core::ColorsDisabled);
        break;

    default:
        break;
    }

    core::HeapAllocator allocator;
    sndio::BackendDispatcher backend_dispatcher;

    if (args.list_supported_given) {
        if (!sndio::print_supported(backend_dispatcher, allocator)) {
            return 1;
        }
        return 0;
    }

    pipeline::ConverterConfig converter_config;

    if (args.frame_length_given) {
        if (!core::parse_duration(args.frame_length_arg,
                                  converter_config.internal_frame_length)) {
            roc_log(LogError, "invalid --frame-length: bad format");
            return 1;
        }
        if (converter_config.input_sample_spec.ns_2_samples_overall(
                converter_config.internal_frame_length)
            <= 0) {
            roc_log(LogError, "invalid --frame-length: should be > 0");
            return 1;
        }
    }

    sndio::BackendMap::instance().set_frame_size(converter_config.internal_frame_length,
                                                 converter_config.input_sample_spec);

    core::BufferFactory<audio::sample_t> buffer_factory(
        allocator,
        converter_config.input_sample_spec.ns_2_samples_overall(
            converter_config.internal_frame_length),
        args.poisoning_flag);

    sndio::Config source_config;
    source_config.sample_spec.set_channel_mask(
        converter_config.input_sample_spec.channel_mask());
    source_config.sample_spec.set_sample_rate(0);
    source_config.frame_length = converter_config.internal_frame_length;

    address::IoUri input_uri(allocator);
    if (args.input_given) {
        if (!address::parse_io_uri(args.input_arg, input_uri) || !input_uri.is_file()) {
            roc_log(LogError, "invalid --input file URI");
            return 1;
        }
    }

    if (!args.input_format_given && input_uri.is_special_file()) {
        roc_log(LogError, "--input-format should be specified if --input is \"-\"");
        return 1;
    }

    core::ScopedPtr<sndio::ISource> input_source;
    if (input_uri.is_valid()) {
        input_source.reset(backend_dispatcher.open_source(input_uri,
                                                          args.input_format_arg,
                                                          source_config, allocator),
                           allocator);
    } else {
        input_source.reset(
            backend_dispatcher.open_default_source(source_config, allocator), allocator);
    }
    if (!input_source) {
        roc_log(LogError, "can't open input: %s", args.input_arg);
        return 1;
    }
    if (input_source->has_clock()) {
        roc_log(LogError, "unsupported input: %s", args.input_arg);
        return 1;
    }

    converter_config.input_sample_spec.set_sample_rate(
        input_source->sample_spec().sample_rate());

    if (args.rate_given) {
        converter_config.output_sample_spec.set_sample_rate((size_t)args.rate_arg);
    } else {
        converter_config.output_sample_spec.set_sample_rate(
            converter_config.input_sample_spec.sample_rate());
    }

    switch (args.resampler_backend_arg) {
    case resampler_backend_arg_default:
        converter_config.resampler_backend = audio::ResamplerBackend_Default;
        break;
    case resampler_backend_arg_builtin:
        converter_config.resampler_backend = audio::ResamplerBackend_Builtin;
        break;
    case resampler_backend_arg_speex:
        converter_config.resampler_backend = audio::ResamplerBackend_Speex;
        break;
    default:
        break;
    }

    switch (args.resampler_profile_arg) {
    case resampler_profile_arg_low:
        converter_config.resampler_profile = audio::ResamplerProfile_Low;
        break;

    case resampler_profile_arg_medium:
        converter_config.resampler_profile = audio::ResamplerProfile_Medium;
        break;

    case resampler_profile_arg_high:
        converter_config.resampler_profile = audio::ResamplerProfile_High;
        break;

    default:
        break;
    }

    converter_config.resampling = !args.no_resampling_flag;
    converter_config.poisoning = args.poisoning_flag;
    converter_config.profiling = args.profiling_flag;

    audio::IFrameWriter* output_writer = NULL;

    sndio::Config sink_config;
    sink_config.sample_spec = converter_config.output_sample_spec;
    sink_config.frame_length = converter_config.internal_frame_length;

    address::IoUri output_uri(allocator);
    if (args.output_given) {
        if (!address::parse_io_uri(args.output_arg, output_uri)
            || !output_uri.is_file()) {
            roc_log(LogError, "invalid --output file URI");
            return 1;
        }
    }

    if (!args.output_format_given && output_uri.is_special_file()) {
        roc_log(LogError, "--output-format should be specified if --output is \"-\"");
        return 1;
    }

    core::ScopedPtr<sndio::ISink> output_sink;
    if (args.output_given) {
        if (output_uri.is_valid()) {
            output_sink.reset(backend_dispatcher.open_sink(output_uri,
                                                           args.output_format_arg,
                                                           sink_config, allocator),
                              allocator);
        } else {
            output_sink.reset(
                backend_dispatcher.open_default_sink(sink_config, allocator), allocator);
        }
        if (!output_sink) {
            roc_log(LogError, "can't open output: %s", args.output_arg);
            return 1;
        }
        if (output_sink->has_clock()) {
            roc_log(LogError, "unsupported output: %s", args.output_arg);
            return 1;
        }
        output_writer = output_sink.get();
    }

    pipeline::ConverterSink converter(converter_config, output_writer, buffer_factory,
                                      allocator);
    if (!converter.valid()) {
        roc_log(LogError, "can't create converter pipeline");
        return 1;
    }

    sndio::Pump pump(buffer_factory, *input_source, NULL, converter,
                     converter_config.internal_frame_length,
                     converter_config.input_sample_spec, sndio::Pump::ModePermanent);
    if (!pump.valid()) {
        roc_log(LogError, "can't create audio pump");
        return 1;
    }

    const bool ok = pump.run();

    return ok ? 0 : 1;
}
