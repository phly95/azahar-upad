// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_qt/configuration/configure_streaming.h"

#include "common/settings.h"
#include "ui_configure_streaming.h"

static bool IsVAAPI(Settings::StreamingEncoder enc) {
    return enc == Settings::StreamingEncoder::VAAPI ||
           enc == Settings::StreamingEncoder::VAAPI_LowPower;
}

ConfigureStreaming::ConfigureStreaming(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureStreaming>()) {
    ui->setupUi(this);
    connect(ui->streaming_custom_res_check, &QCheckBox::toggled, this,
            [this]() { UpdateCustomResEnabled(); });
    connect(ui->streaming_custom_res_2_check, &QCheckBox::toggled, this,
            [this]() { UpdateCustomResEnabled(); });
    connect(ui->streaming_encoder_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { UpdateEncoderControls(0); });
    connect(ui->streaming_encoder_2_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { UpdateEncoderControls(1); });
    SetConfiguration();
}

ConfigureStreaming::~ConfigureStreaming() = default;

void ConfigureStreaming::UpdateCustomResEnabled() {
    const bool custom = ui->streaming_custom_res_check->isChecked();
    ui->streaming_width_spin->setEnabled(custom);
    ui->streaming_height_spin->setEnabled(custom);
    ui->streaming_width_label->setEnabled(custom);
    ui->streaming_height_label->setEnabled(custom);

    const bool custom2 = ui->streaming_custom_res_2_check->isChecked();
    ui->streaming_width_2_spin->setEnabled(custom2);
    ui->streaming_height_2_spin->setEnabled(custom2);
    ui->streaming_width_2_label->setEnabled(custom2);
    ui->streaming_height_2_label->setEnabled(custom2);
}

void ConfigureStreaming::UpdateEncoderControls(int stream_index) {
    const auto* combo = stream_index == 0 ? ui->streaming_encoder_combo
                                          : ui->streaming_encoder_2_combo;
    auto* bitrate_label = stream_index == 0 ? ui->streaming_bitrate_label
                                            : ui->streaming_bitrate_2_label;
    auto* bitrate_spin = stream_index == 0 ? ui->streaming_bitrate_spin
                                           : ui->streaming_bitrate_2_spin;

    const auto encoder = static_cast<Settings::StreamingEncoder>(combo->currentIndex());
    const bool vaapi = IsVAAPI(encoder);

    if (vaapi) {
        bitrate_label->setText(QStringLiteral("Quality (QP):"));
        bitrate_spin->setMinimum(1);
        bitrate_spin->setMaximum(51);
        bitrate_spin->setSuffix(QStringLiteral(""));
        const auto qp = stream_index == 0 ? Settings::values.streaming_qp.GetValue()
                                          : Settings::values.streaming_qp_2.GetValue();
        bitrate_spin->setValue(static_cast<int>(qp));
    } else {
        bitrate_label->setText(QStringLiteral("Bitrate (kbps):"));
        bitrate_spin->setMinimum(100);
        bitrate_spin->setMaximum(100000);
        bitrate_spin->setSuffix(QStringLiteral(" kbps"));
        const auto br = stream_index == 0 ? Settings::values.streaming_bitrate.GetValue()
                                          : Settings::values.streaming_bitrate_2.GetValue();
        bitrate_spin->setValue(static_cast<int>(br));
    }
}

void ConfigureStreaming::SetConfiguration() {
    // Stream 1
    ui->streaming_enabled_check->setChecked(Settings::values.streaming_enabled.GetValue());
    ui->streaming_screen_combo->setCurrentIndex(
        static_cast<int>(Settings::values.streaming_screen.GetValue()));
    ui->streaming_encoder_combo->setCurrentIndex(
        static_cast<int>(Settings::values.streaming_encoder.GetValue()));
    ui->streaming_gpu_edit->setText(
        QString::fromStdString(Settings::values.streaming_gpu_device.GetValue()));
    ui->streaming_custom_res_check->setChecked(
        Settings::values.streaming_custom_resolution.GetValue());
    ui->streaming_width_spin->setValue(
        static_cast<int>(Settings::values.streaming_width.GetValue()));
    ui->streaming_height_spin->setValue(
        static_cast<int>(Settings::values.streaming_height.GetValue()));
    ui->target_ip_edit->setText(
        QString::fromStdString(Settings::values.streaming_target_ip.GetValue()));
    ui->target_port_spin->setValue(
        static_cast<int>(Settings::values.streaming_target_port.GetValue()));
    ui->streaming_input_enabled_check->setChecked(
        Settings::values.streaming_input_enabled.GetValue());
    ui->streaming_input_port_spin->setValue(
        static_cast<int>(Settings::values.streaming_input_port.GetValue()));
    UpdateEncoderControls(0);

    // Stream 2
    ui->streaming_enabled_2_check->setChecked(Settings::values.streaming_enabled_2.GetValue());
    ui->streaming_screen_2_combo->setCurrentIndex(
        static_cast<int>(Settings::values.streaming_screen_2.GetValue()));
    ui->streaming_encoder_2_combo->setCurrentIndex(
        static_cast<int>(Settings::values.streaming_encoder_2.GetValue()));
    ui->streaming_gpu_2_edit->setText(
        QString::fromStdString(Settings::values.streaming_gpu_device_2.GetValue()));
    ui->streaming_custom_res_2_check->setChecked(
        Settings::values.streaming_custom_resolution_2.GetValue());
    ui->streaming_width_2_spin->setValue(
        static_cast<int>(Settings::values.streaming_width_2.GetValue()));
    ui->streaming_height_2_spin->setValue(
        static_cast<int>(Settings::values.streaming_height_2.GetValue()));
    ui->target_ip_2_edit->setText(
        QString::fromStdString(Settings::values.streaming_target_ip_2.GetValue()));
    ui->target_port_2_spin->setValue(
        static_cast<int>(Settings::values.streaming_target_port_2.GetValue()));
    UpdateEncoderControls(1);

    UpdateCustomResEnabled();
}

void ConfigureStreaming::ApplyConfiguration() {
    // Stream 1
    Settings::values.streaming_enabled = ui->streaming_enabled_check->isChecked();
    Settings::values.streaming_screen =
        static_cast<Settings::StreamingScreen>(ui->streaming_screen_combo->currentIndex());
    const auto encoder1 =
        static_cast<Settings::StreamingEncoder>(ui->streaming_encoder_combo->currentIndex());
    Settings::values.streaming_encoder = encoder1;
    Settings::values.streaming_gpu_device = ui->streaming_gpu_edit->text().toStdString();
    Settings::values.streaming_custom_resolution = ui->streaming_custom_res_check->isChecked();
    Settings::values.streaming_width =
        static_cast<u32>(ui->streaming_width_spin->value());
    Settings::values.streaming_height =
        static_cast<u32>(ui->streaming_height_spin->value());
    Settings::values.streaming_target_ip = ui->target_ip_edit->text().toStdString();
    Settings::values.streaming_target_port =
        static_cast<u16>(ui->target_port_spin->value());
    Settings::values.streaming_input_enabled = ui->streaming_input_enabled_check->isChecked();
    Settings::values.streaming_input_port =
        static_cast<u16>(ui->streaming_input_port_spin->value());
    if (IsVAAPI(encoder1)) {
        Settings::values.streaming_qp =
            static_cast<u32>(ui->streaming_bitrate_spin->value());
    } else {
        Settings::values.streaming_bitrate =
            static_cast<u32>(ui->streaming_bitrate_spin->value());
    }

    // Stream 2
    Settings::values.streaming_enabled_2 = ui->streaming_enabled_2_check->isChecked();
    Settings::values.streaming_screen_2 =
        static_cast<Settings::StreamingScreen>(ui->streaming_screen_2_combo->currentIndex());
    const auto encoder2 =
        static_cast<Settings::StreamingEncoder>(ui->streaming_encoder_2_combo->currentIndex());
    Settings::values.streaming_encoder_2 = encoder2;
    Settings::values.streaming_gpu_device_2 = ui->streaming_gpu_2_edit->text().toStdString();
    Settings::values.streaming_custom_resolution_2 = ui->streaming_custom_res_2_check->isChecked();
    Settings::values.streaming_width_2 =
        static_cast<u32>(ui->streaming_width_2_spin->value());
    Settings::values.streaming_height_2 =
        static_cast<u32>(ui->streaming_height_2_spin->value());
    Settings::values.streaming_target_ip_2 = ui->target_ip_2_edit->text().toStdString();
    Settings::values.streaming_target_port_2 =
        static_cast<u16>(ui->target_port_2_spin->value());
    if (IsVAAPI(encoder2)) {
        Settings::values.streaming_qp_2 =
            static_cast<u32>(ui->streaming_bitrate_2_spin->value());
    } else {
        Settings::values.streaming_bitrate_2 =
            static_cast<u32>(ui->streaming_bitrate_2_spin->value());
    }
}

void ConfigureStreaming::RetranslateUI() {
    ui->retranslateUi(this);
}
