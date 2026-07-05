// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_qt/configuration/configure_streaming.h"
#include "common/settings.h"
#include "ui_configure_streaming.h"

ConfigureStreaming::ConfigureStreaming(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureStreaming>()) {
    ui->setupUi(this);
    SetConfiguration();
}

ConfigureStreaming::~ConfigureStreaming() = default;

void ConfigureStreaming::SetConfiguration() {
    ui->streaming_enabled_check->setChecked(Settings::values.streaming_enabled.GetValue());
    ui->streaming_screen_combo->setCurrentIndex(
        static_cast<int>(Settings::values.streaming_screen.GetValue()));
    ui->target_ip_edit->setText(
        QString::fromStdString(Settings::values.streaming_target_ip.GetValue()));
    ui->target_port_spin->setValue(
        static_cast<int>(Settings::values.streaming_target_port.GetValue()));
}

void ConfigureStreaming::ApplyConfiguration() {
    Settings::values.streaming_enabled = ui->streaming_enabled_check->isChecked();
    Settings::values.streaming_screen =
        static_cast<Settings::StreamingScreen>(ui->streaming_screen_combo->currentIndex());
    Settings::values.streaming_target_ip = ui->target_ip_edit->text().toStdString();
    Settings::values.streaming_target_port =
        static_cast<u16>(ui->target_port_spin->value());
}

void ConfigureStreaming::RetranslateUI() {
    ui->retranslateUi(this);
}
