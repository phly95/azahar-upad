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
            [this]() { UpdateCustomResEnabled(); });
    connect(ui->streaming_custom_res_2_check, &QCheckBox::toggled, this,
            [this]() { UpdateCustomResEnabled(); });
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

    UpdateCustomResEnabled();
}

void ConfigureStreaming::ApplyConfiguration() {
    // Stream 1
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

    // Stream 2
    Settings::values.streaming_enabled_2 = ui->streaming_enabled_2_check->isChecked();
    Settings::values.streaming_screen_2 =
        static_cast<Settings::StreamingScreen>(ui->streaming_screen_2_combo->currentIndex());
    Settings::values.streaming_encoder_2 =
        static_cast<Settings::StreamingEncoder>(ui->streaming_encoder_2_combo->currentIndex());
    Settings::values.streaming_gpu_device_2 = ui->streaming_gpu_2_edit->text().toStdString();
    Settings::values.streaming_custom_resolution_2 = ui->streaming_custom_res_2_check->isChecked();
    Settings::values.streaming_width_2 =
        static_cast<u32>(ui->streaming_width_2_spin->value());
    Settings::values.streaming_height_2 =
        static_cast<u32>(ui->streaming_height_2_spin->value());
    Settings::values.streaming_target_ip_2 = ui->target_ip_2_edit->text().toStdString();
    Settings::values.streaming_target_port_2 =
        static_cast<u16>(ui->target_port_2_spin->value());
}

void ConfigureStreaming::RetranslateUI() {
    ui->retranslateUi(this);
}
