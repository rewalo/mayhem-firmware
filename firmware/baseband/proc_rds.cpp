/*
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/* RDS baseband processor with optional program audio (mic or file).
 * Signal path: Audio Source (Mic/File) -> Audio Buffer -> Mix With RDS Subcarrier
 *              -> FM Modulator -> Complex I/Q Output
 */

#include "proc_rds.hpp"
#include "portapack_shared_memory.hpp"
#include "sine_table_int8.hpp"
#include "event_m4.hpp"
#include "audio_dma.hpp"

#include <cstdint>

void RDSProcessor::execute(const buffer_c8_t& buffer) {
    // Audio: Mic provides 24 kHz, we run at 2.28 MHz -> over = 95
    // File: resampling via resample_acc similar to proc_audiotx
    constexpr uint32_t audio_over_mic = BASEBAND_FS / 24000;  // 95

    if (audio_source == 1) {
        audio_input.read_audio_buffer(audio_buffer);
    }

    for (size_t i = 0; i < buffer.count; i++) {
        // --- RDS subcarrier generation (228 kHz effective) ---
        int32_t rds_sample = 0;
        if (s >= 9) {
            s = 0;
            if (sample_count >= SAMPLES_PER_BIT) {
                if (bit_pos >= message_length) {
                    bit_pos = 0;
                    cur_output = 0;
                }

                cur_bit = (rdsdata[(bit_pos / 26) & 127] >> (25 - (bit_pos % 26))) & 1;
                prev_output = cur_output;
                cur_output = prev_output ^ cur_bit;

                const int32_t* src = waveform_biphase;
                int idx = in_sample_index;

                for (int j = 0; j < FILTER_SIZE; j++) {
                    val = (*src++);
                    if (cur_output) val = -val;
                    sample_buffer[idx++] += val;
                    if (idx >= SAMPLE_BUFFER_SIZE) idx = 0;
                }

                in_sample_index += SAMPLES_PER_BIT;
                if (in_sample_index >= SAMPLE_BUFFER_SIZE) in_sample_index -= SAMPLE_BUFFER_SIZE;

                bit_pos++;

                sample_count = 0;
            }

            rds_sample = sample_buffer[out_sample_index];
            sample_buffer[out_sample_index] = 0;
            out_sample_index++;
            if (out_sample_index >= SAMPLE_BUFFER_SIZE) out_sample_index = 0;

            // AM @ 228k/4 = 57kHz (BPSK subcarrier)
            switch (mphase & 3) {
                case 0:
                case 2:
                    rds_sample = 0;
                    break;
                case 1:
                    break;
                case 3:
                    rds_sample = -rds_sample;
                    break;
            }
            mphase++;

            sample_count++;
        } else {
            s++;
        }

        // --- Audio sample (mic or file) ---
        int32_t audio_sample = 0;
        if (audio_source == 1) {
            // Mic: read from audio_input at 24 kHz
            if (i % audio_over_mic == 0) {
                size_t idx = (i / audio_over_mic) & (AUDIO_BUF_COUNT - 1);
                audio_sample = (int32_t)audio_buffer.p[idx];
            }
        } else if (audio_source == 2 && stream) {
            // File: resample from stream
            resample_acc += resample_inc;
            if (resample_acc >= 0x10000) {
                resample_acc -= 0x10000;
                uint32_t read_val = 0;
                stream->read(&read_val, bytes_per_sample);
                if (bytes_per_sample == 1) {
                    audio_sample = ((int32_t)(read_val & 0xFF) - 128) * 256;
                } else {
                    audio_sample = (int32_t)(int16_t)(read_val & 0xFFFF);
                }
            }
        }

        // --- Mix: combined = audio * gain + rds * rds_injection_gain ---
        // Audio scaled for ~75 kHz deviation; RDS subcarrier uses existing scaling
        int32_t audio_scaled = (int32_t)((float)audio_sample * audio_gain);
        int32_t rds_scaled = (int32_t)((float)(rds_sample >> 10) * rds_injection_gain * 1024.0f);
        int32_t combined = audio_scaled + rds_scaled;

        // Clamp to avoid excessive FM deviation
        if (combined > 32767) combined = 32767;
        if (combined < -32768) combined = -32768;

        // --- FM modulation ---
        delta = combined * fm_delta_audio;
        delta += (rds_sample >> 16) * 386760;  // RDS delta (original scaling)

        phase += delta;
        sphase = phase + (64 << 18);

        re = (sine_table_i8[(sphase & 0x03FF0000) >> 18]);
        im = (sine_table_i8[(phase & 0x03FF0000) >> 18]);

        buffer.p[i] = {re, im};
    }
}

void RDSProcessor::on_message(const Message* const msg) {
    switch (msg->id) {
        case Message::ID::RDSConfigure: {
            const auto message = *reinterpret_cast<const RDSConfigureMessage*>(msg);
            rdsdata = (uint32_t*)shared_memory.bb_data.data;
            message_length = message.length;
            configured = true;
            break;
        }

        case Message::ID::RDSAudioConfig: {
            const auto message = *reinterpret_cast<const RDSAudioConfigMessage*>(msg);
            audio_source = message.audio_source;
            audio_gain = message.audio_gain;
            rds_injection_gain = message.rds_injection_gain;
            break;
        }

        case Message::ID::ReplayConfig: {
            const auto message = *reinterpret_cast<const ReplayConfigMessage*>(msg);
            if (message.config) {
                stream = std::make_unique<StreamOutput>(message.config);
                RequestSignalMessage sig_msg{RequestSignalMessage::Signal::FillRequest};
                shared_memory.application_queue.push(sig_msg);
            } else {
                stream.reset();
            }
            break;
        }

        case Message::ID::SampleRateConfig: {
            const auto message = *reinterpret_cast<const SampleRateConfigMessage*>(msg);
            resample_inc = (((uint64_t)message.sample_rate) << 16) / BASEBAND_FS;
            bytes_per_sample = 2;  // WAV from Soundboard typically 16-bit
            break;
        }

        case Message::ID::FIFOData:
            break;

        default:
            break;
    }
}

int main() {
    audio::dma::init_audio_in();

    EventDispatcher event_dispatcher{std::make_unique<RDSProcessor>()};
    event_dispatcher.run();
    return 0;
}
