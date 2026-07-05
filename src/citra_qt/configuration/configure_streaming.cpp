// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_qt/configuration/configure_streaming.h"
#include "common/settings.h"
#include "ui_configure_streaming.h"

ConfigureStreaming::ConfigureStreaming(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureStreaming>()) {
    ui->setupUi(this);
    connect(ui->streaming_custom_res_check, &QCheckBox::toggled, this,
            [this]() { UpdateCustomResolutionEnabled(); });
    SetConfiguration();
}

ConfigureStreaming::~ConfigureStreaming() = default;

void ConfigureStreaming::UpdateCustomResolutionEnabled() {
    const bool custom = ui->streaming_custom_res_check->isChecked();
    ui->streaming_width_spin->setEnabled(custom);
    ui->streaming_height_spin->setEnabled(custom);
    ui->streaming_width_label->setEnabled(custom);
    ui->streaming_height_label->setEnabled(custom);
}

void ConfigureStreaming::SetConfiguration() {
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
    UpdateCustomResolutionEnabled();
}

void ConfigureStreaming::ApplyConfiguration() {
    Settings::values.streaming_enabled = ui->streaming_enabled_check->isChecked();
    Settings::values.streaming_screen =
        static_cast<Settings::StreamingScreen>(ui->streaming_screen_combo->currentIndex());
    Settings::values.streaming_encoder =
        static_cast<Settings::StreamingEncoder>(ui->streaming_encoder_combo->currentIndex());
    Settings::values.streaming_gpu_device = ui->streaming_gpu_edit->text().toStdString();
    Settings::values.streaming_custom_resolution = ui->streaming_custom_res_check->isChecked();
    Settings::values.streaming_width =
        static_cast<u32>(ui->streaming_width_spin->value());
    Settings::values.streaming_height =
        static_cast<u32>(ui->streaming_height_spin->value());
    Settings::values.streaming_target_ip = ui->target_ip_edit->text().toStdString();
    Settings::values.streaming_target_port =
        static_cast<u16>(ui->target_port_spin->value());
}

void ConfigureStreaming::RetranslateUI() {
    ui->retranslateUi(this);
}
