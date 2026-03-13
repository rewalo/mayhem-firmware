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

#include "ui_rds.hpp"

#include "portapack.hpp"
#include "baseband_api.hpp"
#include "portapack_shared_memory.hpp"
#include "ui_fileman.hpp"
#include "file_path.hpp"
#include "io_wave.hpp"
#include "replay_thread.hpp"

#include <cstring>

using namespace portapack;
using namespace rds;

namespace ui {

RDSPSNView::RDSPSNView(
    NavigationView& nav,
    Rect parent_rect)
    : OptionTabView(parent_rect) {
    set_type("PSN");

    add_children({&labels,
                  &text_psn,
                  &button_set,
                  &check_mono_stereo,
                  &check_TA,
                  &check_MS});

    set_enabled(true);

    check_TA.set_value(true);

    check_mono_stereo.on_select = [this](Checkbox&, bool value) {
        mono_stereo = value;
    };
    check_TA.on_select = [this](Checkbox&, bool value) {
        TA = value;
    };
    check_MS.on_select = [this](Checkbox&, bool value) {
        MS = value;
    };

    button_set.on_select = [this, &nav](Button&) {
        text_prompt(
            nav,
            PSN,
            8,
            ENTER_KEYBOARD_MODE_ALPHA,
            [this](std::string& s) {
                text_psn.set(s);
            });
    };
}

RDSRadioTextView::RDSRadioTextView(
    NavigationView& nav,
    Rect parent_rect)
    : OptionTabView(parent_rect) {
    set_type("Radiotext");

    add_children({&labels,
                  &button_set,
                  &text_radiotext});

    button_set.on_select = [this, &nav](Button&) {
        text_prompt(
            nav,
            radiotext,
            28,
            ENTER_KEYBOARD_MODE_ALPHA,
            [this](std::string& s) {
                text_radiotext.set(s);
            });
    };
}

RDSDateTimeView::RDSDateTimeView(
    Rect parent_rect)
    : OptionTabView(parent_rect) {
    set_type("date & time");

    add_children({&labels});
}

RDSAudioView::RDSAudioView(
    NavigationView& nav,
    Rect parent_rect)
    : OptionTabView(parent_rect),
      nav_{nav} {
    set_type("audio");

    auto open_file_picker = [this]() {
        auto open_view = nav_.push<FileLoadView>(".WAV");
        open_view->push_dir(wav_dir);
        open_view->on_changed = [this](std::filesystem::path path) {
            file_path_ = path;
            text_file.set(path.filename().string().substr(0, 22));
        };
    };

    options_source.on_change = [this, open_file_picker](size_t, OptionsField::value_t v) {
        if (v != 2) {
            file_path_ = std::filesystem::path{};
            text_file.set("-");
        }

        button_file.hidden(v != 2);
        text_file.hidden(v != 2);
        button_mic_hold.hidden(v != 1);

        if (on_source_change) {
            on_source_change(v);
        }

        if (v == 2 && file_path_.empty()) {
            open_file_picker();
        }
    };

    button_file.on_select = [open_file_picker](Button&) {
        open_file_picker();
    };

    button_file.hidden(true);
    text_file.hidden(true);
    button_mic_hold.hidden(true);

    button_mic_hold.on_touch_press = [this](Button&) {
        if (on_mic_press) {
            on_mic_press();
        }
    };

    button_mic_hold.on_touch_release = [this](Button&) {
        if (on_mic_release) {
            on_mic_release();
        }
    };

    add_children({&labels,
                  &options_source,
                  &button_file,
                  &text_file,
                  &button_mic_hold});
}

RDSThread::RDSThread(
    std::vector<RDSGroup>** frames)
    : frames_{std::move(frames)} {
    thread = chThdCreateFromHeap(NULL, 1024, NORMALPRIO + 10, RDSThread::static_fn, this);
}

RDSThread::~RDSThread() {
    if (thread) {
        chThdTerminate(thread);
        chThdWait(thread);
        thread = nullptr;
    }
}

msg_t RDSThread::static_fn(void* arg) {
    auto obj = static_cast<RDSThread*>(arg);
    obj->run();
    return 0;
}

void RDSThread::run() {
    std::vector<RDSGroup>* frame_ptr;
    size_t block_count, c;
    uint32_t* tx_data_u32 = (uint32_t*)shared_memory.bb_data.data;
    uint32_t frame_index = 0;

    while (!chThdShouldTerminate()) {
        do {
            frame_ptr = frames_[frame_index];

            if (frame_index == 2) {
                frame_index = 0;
            } else {
                frame_index++;
            }
        } while (!(block_count = frame_ptr->size() * 4));

        for (c = 0; c < block_count; c++)
            tx_data_u32[c] = frame_ptr->at(c >> 2).block[c & 3];

        baseband::set_rds_data(block_count * 26);

        chThdSleepMilliseconds(1000);
    }
}

void RDSView::focus() {
    tab_view.focus();
}

RDSView::~RDSView() {
    stop_tx();
    baseband::shutdown();
}

bool RDSView::start_tx() {
    rds_flags.PI_code = sym_pi_code.to_integer();
    rds_flags.PTY = options_pty.selected_index_value();
    rds_flags.DI = view_PSN.mono_stereo ? 1 : 0;
    rds_flags.TP = check_TP.value();
    rds_flags.TA = view_PSN.TA;
    rds_flags.MS = view_PSN.MS;

    if (view_PSN.is_enabled())
        gen_PSN(frame_psn, view_PSN.PSN, &rds_flags);
    else
        frame_psn.clear();

    if (view_radiotext.is_enabled())
        gen_RadioText(frame_radiotext, view_radiotext.radiotext, 0, &rds_flags);
    else
        frame_radiotext.clear();

    // DEBUG
    if (view_datetime.is_enabled())
        gen_ClockTime(frame_datetime, &rds_flags, 2016, 12, 1, 9, 23, 2);
    else
        frame_datetime.clear();

    /* Audio source config to baseband. */
    uint8_t src = view_audio.audio_source_index();
    float rds_gain = (src == 0) ? 1.0f : 0.04f;
    uint8_t audio_bps = 16;

    /* File mode: start replay before TX */
    if (src == 2) {
        if (view_audio.file_path().empty()) {
            nav_.display_modal("Error", "Select a WAV file first.");
            return false;
        }

        replay_ready_signal = false;
        auto reader = std::make_unique<WAVFileReader>();
        if (!reader->open(view_audio.file_path())) {
            nav_.display_modal("Error", "Cannot open WAV file.");
            return false;
        }
        if ((reader->channels() != 1) || ((reader->bits_per_sample() != 8) && (reader->bits_per_sample() != 16))) {
            nav_.display_modal("Error", "WAV must be 8 or 16-bit mono.");
            return false;
        }
        audio_bps = reader->bits_per_sample();
        baseband::set_sample_rate(reader->sample_rate());
        replay_thread = std::make_unique<ReplayThread>(
            std::move(reader),
            replay_read_size,
            replay_buffer_count,
            &replay_ready_signal,
            [this](uint32_t code) {
                ReplayThreadDoneMessage msg{code};
                EventDispatcher::send_message(msg);
            });
    }

    // Keep output hot; tune this to max practical loudness.
    const float max_audio_gain = 2.0f;
    baseband::set_rds_audio_config(src, max_audio_gain, rds_gain, audio_bps);

    transmitter_model.enable();
    tx_thread.reset();
    tx_thread = std::make_unique<RDSThread>(frames);
    return true;
}

void RDSView::stop_tx() {
    tx_thread.reset();
    replay_thread.reset();
    baseband::replay_stop();
    tx_view.set_transmitting(false);
    transmitter_model.disable();
    txing = false;
    mic_hold_active_ = false;
}

RDSView::RDSView(
    NavigationView& nav)
    : nav_{nav} {
    baseband::run_image(portapack::spi_flash::image_tag_rds);

    add_children({
        &tab_view,
        &labels,
        &sym_pi_code,
        &check_TP,
        &options_pty,
        &view_PSN,
        &view_radiotext,
        &view_datetime,
        &view_audio,
        &tx_view,
    });

    check_TP.set_value(true);

    sym_pi_code.set_value(0xF3E0);
    sym_pi_code.on_change = [this](SymField&) {
        rds_flags.PI_code = sym_pi_code.to_integer();
    };

    options_pty.set_selected_index(0);  // None

    view_audio.on_source_change = [this](uint8_t source) {
        mic_hold_active_ = false;
        if (source != 1 && txing) {
            stop_tx();
        }
    };

    view_audio.on_mic_press = [this]() {
        if (view_audio.audio_source_index() != 1) {
            return;
        }

        if (!txing && start_tx()) {
            tx_view.set_transmitting(true);
            txing = true;
        }

        mic_hold_active_ = txing;
    };

    view_audio.on_mic_release = [this]() {
        if (mic_hold_active_ && view_audio.audio_source_index() == 1) {
            stop_tx();
        }
    };

    tx_view.on_edit_frequency = [this, &nav]() {
        auto new_view = nav.push<FrequencyKeypadView>(transmitter_model.target_frequency());
        new_view->on_changed = [this](rf::Frequency f) {
            transmitter_model.set_target_frequency(f);
        };
    };

    tx_view.on_start = [this]() {
        if (start_tx()) {
            tx_view.set_transmitting(true);
            txing = true;
        }
    };

    tx_view.on_stop = [this]() {
        stop_tx();
    };
}

} /* namespace ui */
