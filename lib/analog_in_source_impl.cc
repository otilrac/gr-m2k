/*
 * Copyright 2019 Analog Devices Inc.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "analog_in_source_impl.h"
#include <m2k/analog_in_converter.h>
#include <gnuradio/io_signature.h>
#include <libm2k/m2k.hpp>
#include <libm2k/contextbuilder.hpp>
#include <libm2k/analog/m2kanalogin.hpp>
#include <boost/lexical_cast.hpp>


using namespace gr::m2k;

analog_in_source::sptr
analog_in_source::make(const std::string &uri,
                       int buffer_size,
                       const std::vector<int> &channels,
                       std::vector<int> ranges,
                       double sampling_frequency,
                       int oversampling_ratio,
                       int kernel_buffers,
                       bool calibrate_ADC,
                       bool stream_voltage_values,
                       std::vector<int> trigger_condition,
                       std::vector<int> trigger_mode,
                       int trigger_source,
                       int trigger_delay,
                       std::vector<double> trigger_level)
{
    return gnuradio::get_initial_sptr
        (new analog_in_source_impl(uri, buffer_size, channels, ranges, sampling_frequency, oversampling_ratio,
                                   kernel_buffers,
                                   calibrate_ADC, stream_voltage_values, trigger_condition, trigger_mode,
                                   trigger_source,
                                   trigger_delay, trigger_level));
}

analog_in_source_impl::analog_in_source_impl(const std::string &uri,
                                             int buffer_size,
                                             const std::vector<int> &channels,
                                             std::vector<int> ranges,
                                             double sampling_frequency,
                                             int oversampling_ratio,
                                             int kernel_buffers,
                                             bool calibrate_ADC,
                                             bool stream_voltage_values,
                                             std::vector<int> trigger_condition,
                                             std::vector<int> trigger_mode,
                                             int trigger_source,
                                             int trigger_delay,
                                             std::vector<double> trigger_level)
    : gr::sync_block("analog_in_source",
                     gr::io_signature::make(0, 0, 0),
                     gr::io_signature::make(1, 2, sizeof(float))),
    d_uri(uri),
    d_buffer_size(buffer_size),
    d_channels(channels),
    port_id(pmt::mp("msg")),
    d_stream_voltage_values(stream_voltage_values)
{
    libm2k::contexts::M2k *context = analog_in_source_impl::get_context(uri);
    d_analog_in = context->getAnalogIn();

    d_analog_in->setKernelBuffersCount(kernel_buffers);
    set_params(ranges, sampling_frequency, oversampling_ratio);
    set_trigger(trigger_condition, trigger_mode, trigger_source, trigger_delay, trigger_level);

    if (calibrate_ADC) {
        context->calibrateADC();
    }

    if (!stream_voltage_values) {
        set_output_signature(gr::io_signature::make(1, 2, sizeof(short)));
    }

    d_samples = {std::vector<double>(), std::vector<double>()};
    set_output_multiple(0x400);
    message_port_register_out(port_id);
}

analog_in_source_impl::~analog_in_source_impl()
{
    remove_contexts(d_uri);
}

void analog_in_source_impl::set_params(std::vector<int> ranges,
                                       double sampling_frequency,
                                       int oversampling_ratio)
{
    for (int i = 0; i < d_channels.size(); i++) {
        if (d_channels.at(i)) {
            d_analog_in->enableChannel(i, true);
            d_analog_in->setRange(static_cast<libm2k::analog::ANALOG_IN_CHANNEL>(i),
                                  (static_cast<libm2k::analog::M2K_RANGE>(ranges[i])));
        }
    }

    d_analog_in->setSampleRate(sampling_frequency);
    d_analog_in->setOversamplingRatio(oversampling_ratio);
}

void analog_in_source_impl::set_trigger(std::vector<int> trigger_condition,
                                        std::vector<int> trigger_mode,
                                        int trigger_source,
                                        int trigger_delay,
                                        std::vector<double> trigger_level)
{
    libm2k::M2kHardwareTrigger *trigger = d_analog_in->getTrigger();

    for (int i = 0; i < d_channels.size(); ++i) {
        if (d_channels.at(i)) {
            trigger->setAnalogCondition(i, static_cast<libm2k::M2K_TRIGGER_CONDITION_ANALOG>(trigger_condition[i]));
            trigger->setAnalogMode(i, static_cast<libm2k::M2K_TRIGGER_MODE>(trigger_mode[i]));
            trigger->setAnalogLevel(i, trigger_level[i]);
        }
    }
    trigger->setAnalogSource(static_cast<libm2k::M2K_TRIGGER_SOURCE_ANALOG>(trigger_source));
    trigger->setAnalogDelay(trigger_delay);
}

libm2k::contexts::M2k *analog_in_source_impl::get_context(const std::string &uri)
{
    auto element = s_contexts.find(uri);
    if (element == s_contexts.end()) {
        libm2k::contexts::M2k *ctx = libm2k::contexts::m2kOpen(uri.c_str());
        if (ctx == nullptr) {
            throw std::runtime_error("Unable to create the context!");
        }
        s_contexts.insert(std::pair<std::string, libm2k::contexts::M2k *>(ctx->getUri(), ctx));
        return ctx;
    }
    return element->second;
}

void analog_in_source_impl::remove_contexts(const std::string &uri)
{
    boost::lock_guard <boost::mutex> lock(s_ctx_mutex);
    auto element = s_contexts.find(uri);
    if (element != s_contexts.end()) {
        libm2k::contexts::contextClose(element->second, true);
        s_contexts.erase(element);
    }
}

void analog_in_source_impl::refill_buffer()
{
    boost::unique_lock <boost::mutex> lock(d_mutex);

    while (true) {
        if (d_thread_stopped) {
            break;
        }
        d_cond_wait.wait(lock, [&] { return d_empty_buffer; });

        try {
            lock.unlock();
            d_samples = d_analog_in->getSamplesRaw(d_buffer_size);
            lock.lock();
        } catch (std::exception &e) {
            std::cout << e.what() << std::endl;
            d_thread_stopped = true;
            break;
        }
        d_items_in_buffer = (unsigned long) d_samples[0].size();
        d_sample_index = 0;
        d_empty_buffer = false;
        d_cond_wait.notify_one();
    }
}

int analog_in_source_impl::work(int noutput_items,
                                gr_vector_const_void_star &input_items,
                                gr_vector_void_star &output_items)
{
    boost::unique_lock <boost::mutex> lock(d_mutex);
    if (d_thread_stopped) {
        return -1;
    }
    if (!d_items_in_buffer) {
        d_empty_buffer = true;
        d_cond_wait.notify_one();
    }

    while (d_empty_buffer) {
        // use wait_for to avoid permanent blocking in the work function
        bool buffer_refilled = d_cond_wait.wait_for(lock, boost::chrono::milliseconds(100),
                                                [&] { return !d_empty_buffer; });
        if (d_thread_stopped) {
            return -1;
        }

        if (!buffer_refilled) {
            message_port_pub(port_id, pmt::mp("timeout"));
        }
    }

    unsigned long nb_samples = std::min(d_items_in_buffer, (unsigned long) noutput_items);
    unsigned int sample_index, channel_index, out_stream_index;

    for (out_stream_index = 0, channel_index = 0;
         out_stream_index < output_items.size(); out_stream_index++, channel_index++) {
        while (!d_channels[channel_index]) {
            channel_index++;
        }

        if (!d_sample_index) {
            tag_t tag;
            tag.value = pmt::from_long(d_items_in_buffer);
            tag.offset = nitems_written(out_stream_index);
            tag.key = pmt::intern("buffer_start");
            tag.srcid = alias_pmt();

            add_item_tag(out_stream_index, tag);
        }

        if (d_stream_voltage_values) {
            float *out = (float *) output_items[out_stream_index];
            for (sample_index = 0; sample_index < nb_samples; sample_index++) {
                out[sample_index] = boost::lexical_cast<float>(
                    d_analog_in->convertRawToVolts(channel_index,
                                                   d_samples[channel_index][sample_index + d_sample_index]));
            }
        } else {
            short *out = (short *) output_items[out_stream_index];
            for (sample_index = 0; sample_index < nb_samples; sample_index++) {
                out[sample_index] = boost::lexical_cast<short>(d_samples[channel_index][sample_index + d_sample_index]);
            }
        }
    }
    d_items_in_buffer -= nb_samples;
    d_sample_index += nb_samples;

    return (int) nb_samples;
}

bool analog_in_source_impl::start()
{
    boost::unique_lock <boost::mutex> lock(d_mutex);

    d_items_in_buffer = 0;
    d_empty_buffer = true;
    d_thread_stopped = false;
    d_refill_thread = gr::thread::thread(boost::bind(&analog_in_source_impl::refill_buffer, this));

    return true;
}

bool analog_in_source_impl::stop()
{
    d_analog_in->cancelBuffer();
    boost::unique_lock <boost::mutex> lock(d_mutex);
    d_empty_buffer = true;
    d_thread_stopped = true;
    d_cond_wait.notify_one();
    lock.unlock();
    d_refill_thread.join();
    d_analog_in->flushBuffer();
    return true;
}